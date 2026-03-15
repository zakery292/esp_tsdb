/**
 * @file tsdb_core.c
 * @brief Core initialization and lifecycle management
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static const char *TAG = "TSDB_CORE";

// Global state
tsdb_state_t g_state = {0};

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

/**
 * @brief Flush and sync file to ensure data persists across reboots
 *
 * fflush() only flushes the C stdio buffer. fsync() forces the filesystem
 * to write metadata (file size, allocation tables) to flash. Without this,
 * LittleFS files can appear as 0 bytes after an unexpected reboot.
 */
static void tsdb_flush_and_sync(FILE *file) {
    if (file == NULL) return;
    fflush(file);
    fsync(fileno(file));
}

/**
 * @brief Read header from file
 */
esp_err_t tsdb_read_header(FILE *file, tsdb_header_t *header) {
    if (file == NULL || header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fseek(file, 0, SEEK_SET);
    size_t read = fread(header, sizeof(tsdb_header_t), 1, file);

    if (read != 1) {
        ESP_LOGE(TAG, "Failed to read header");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Write header to file
 */
esp_err_t tsdb_write_header(FILE *file, const tsdb_header_t *header) {
    if (file == NULL || header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fseek(file, 0, SEEK_SET);
    size_t written = fwrite(header, sizeof(tsdb_header_t), 1, file);
    tsdb_flush_and_sync(file);

    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write header");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Calculate block file offset
 */
uint32_t tsdb_calc_block_offset(const tsdb_header_t *header, uint32_t block_num) {
    uint32_t data_offset = header->index_offset +
                          (header->index_entries * sizeof(tsdb_index_entry_t));
    return data_offset + (block_num * TSDB_BLOCK_SIZE);
}

/**
 * @brief Attempt to reconstruct header from data blocks
 *
 * Called when header is corrupted but data blocks may be intact.
 * Scans all blocks to rebuild metadata.
 */
static esp_err_t tsdb_reconstruct_header(FILE *file, tsdb_header_t *header,
                                         const tsdb_config_t *config) {
    ESP_LOGW(TAG, "Attempting header reconstruction from data blocks");

    // Get file size to determine max blocks
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);

    if (file_size < 512) {
        ESP_LOGE(TAG, "File too small to reconstruct");
        return ESP_FAIL;
    }

    // Rebuild basic header structure from config
    memset(header, 0, sizeof(tsdb_header_t));
    header->magic = TSDB_MAGIC;
    header->version = TSDB_VERSION;
    header->num_params = config->num_params;
    header->param_size = sizeof(int16_t);
    header->record_size = 4 + (config->num_params * 2);

    // Calculate records per block
    size_t overhead = 8;
    size_t per_record = 4 + (config->num_params * 2);
    header->records_per_block = (TSDB_BLOCK_SIZE - overhead) / per_record;

    header->max_records = config->max_records;
    header->index_stride = config->index_stride > 0 ? config->index_stride : 380;
    header->index_offset = 512;
    header->index_entries = config->max_records > 0 ?
        (config->max_records / header->index_stride) + 1 : 256;

    // Copy parameter names
    if (config->param_names) {
        for (int i = 0; i < config->num_params && i < 16; i++) {
            strncpy(header->param_names[i], config->param_names[i], 31);
            header->param_names[i][31] = '\0';
        }
    }

    // Calculate data region start and max blocks
    uint32_t data_offset = header->index_offset +
                          (header->index_entries * sizeof(tsdb_index_entry_t));
    uint32_t max_blocks = (file_size - data_offset) / TSDB_BLOCK_SIZE;

    ESP_LOGI(TAG, "Scanning up to %lu blocks for data recovery", (unsigned long)max_blocks);

    // Scan blocks to find data
    tsdb_block_t block;
    uint32_t oldest_ts = UINT32_MAX;
    uint32_t newest_ts = 0;
    uint32_t oldest_idx = 0;
    uint32_t newest_idx = 0;
    uint32_t total_records = 0;

    for (uint32_t block_num = 0; block_num < max_blocks; block_num++) {
        uint32_t block_offset = data_offset + (block_num * TSDB_BLOCK_SIZE);
        fseek(file, block_offset, SEEK_SET);

        if (fread(&block, TSDB_BLOCK_SIZE, 1, file) != 1) {
            continue;
        }

        // Check if block is valid
        uint8_t *rraw = (uint8_t *)&block;
        if (TSDB_BLOCK_MAGIC(rraw) != 0x424C4B54 || TSDB_BLOCK_COUNT(rraw) == 0) {
            continue;
        }

        // Scan records in this block
        for (uint16_t i = 0; i < TSDB_BLOCK_COUNT(rraw) && i < header->records_per_block; i++) {
            uint32_t ts = TSDB_BLOCK_TS(rraw, i);

            if (ts == 0) continue;  // Skip empty records

            uint32_t record_idx = (block_num * header->records_per_block) + i;

            // Track oldest
            if (ts < oldest_ts) {
                oldest_ts = ts;
                oldest_idx = record_idx;
            }

            // Track newest
            if (ts > newest_ts) {
                newest_ts = ts;
                newest_idx = record_idx;
            }

            total_records++;
        }
    }

    if (total_records == 0) {
        ESP_LOGW(TAG, "No valid records found in blocks - empty database");
        // Initialize as empty DB
        header->total_records = 0;
        header->oldest_record_idx = 0;
        header->newest_record_idx = 0;
        header->oldest_timestamp = 0;
        header->newest_timestamp = 0;
        header->total_writes = 0;
        header->total_evictions = 0;
        return ESP_OK;
    }

    // Populate reconstructed header
    header->total_records = total_records;
    header->oldest_record_idx = oldest_idx;
    header->newest_record_idx = newest_idx;
    header->oldest_timestamp = oldest_ts;
    header->newest_timestamp = newest_ts;
    header->total_writes = total_records;  // Best guess

    // Estimate evictions (if total records > max, we've wrapped around)
    if (total_records > config->max_records) {
        header->total_evictions = total_records - config->max_records;
    } else {
        header->total_evictions = 0;
    }

    ESP_LOGW(TAG, "Header reconstructed: %lu records (%lu to %lu), %lu evictions",
             (unsigned long)total_records,
             (unsigned long)oldest_ts,
             (unsigned long)newest_ts,
             (unsigned long)header->total_evictions);

    return ESP_OK;
}

// ============================================================================
// PUBLIC API
// ============================================================================

esp_err_t tsdb_init(const tsdb_config_t *config) {
    // Validation
    if (config == NULL || config->filepath == NULL) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->num_params == 0 || config->num_params > 16) {
        ESP_LOGE(TAG, "Invalid num_params: %d (must be 1-16)", config->num_params);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->buffer_pool_size == 0) {
        ESP_LOGE(TAG, "buffer_pool_size must be > 0");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_state.is_open) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing TSDB: %s", config->filepath);
    ESP_LOGI(TAG, "Parameters: %d, Max records: %lu, Buffer: %d KB",
             config->num_params, (unsigned long)config->max_records,
             config->buffer_pool_size / 1024);

    // Allocate buffer pool
    esp_err_t ret = tsdb_alloc_buffer_pool(&g_state.pool,
                                           config->buffer_pool_size,
                                           config->use_paged_allocation,
                                           config->page_size,
                                           config->alloc_strategy);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate buffer pool");
        return ret;
    }

    // Partition buffer pool into logical regions
    size_t offset = 0;
    g_state.read_buffer_offset = offset;
    offset += TSDB_BLOCK_SIZE;

    g_state.write_cache_offset = offset;
    offset += TSDB_BLOCK_SIZE;

    g_state.query_buffer_offset = offset;
    offset += TSDB_BLOCK_SIZE;

    g_state.stream_buffer_offset = offset;
    g_state.stream_buffer_size = g_state.pool.total_size - offset;

    ESP_LOGI(TAG, "Buffer regions: read=%d, write=%d, query=%d, stream=%d (%d bytes)",
             g_state.read_buffer_offset,
             g_state.write_cache_offset,
             g_state.query_buffer_offset,
             g_state.stream_buffer_offset,
             g_state.stream_buffer_size);

    // Check if file exists
    struct stat st;
    bool file_exists = (stat(config->filepath, &st) == 0);
    bool db_opened_successfully = false;

    if (file_exists) {
        // Open existing file
        ESP_LOGI(TAG, "Opening existing database file");
        g_state.file = fopen(config->filepath, "r+b");
        if (g_state.file == NULL) {
            ESP_LOGE(TAG, "Failed to open existing file");
            tsdb_free_buffer_pool(&g_state.pool);
            return ESP_FAIL;
        }

        // Read and validate header
        bool needs_reconstruction = false;

        if (tsdb_read_header(g_state.file, &g_state.header) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read header - will attempt reconstruction");
            needs_reconstruction = true;
        } else if (g_state.header.magic != TSDB_MAGIC) {
            ESP_LOGW(TAG, "Invalid magic number: 0x%08lX (expected 0x%08X) - will attempt reconstruction",
                     (unsigned long)g_state.header.magic, TSDB_MAGIC);
            needs_reconstruction = true;
        }

        if (needs_reconstruction) {
            // Check file size - if too small, delete and recreate
            fseek(g_state.file, 0, SEEK_END);
            long file_size = ftell(g_state.file);

            if (file_size < 512) {
                ESP_LOGW(TAG, "File too small (%ld bytes) - deleting and recreating", file_size);
                fclose(g_state.file);
                unlink(config->filepath);
                file_exists = false;
            } else {
                // Attempt to reconstruct header from data blocks
                if (tsdb_reconstruct_header(g_state.file, &g_state.header, config) != ESP_OK) {
                    ESP_LOGE(TAG, "Header reconstruction failed - deleting and recreating");
                    fclose(g_state.file);
                    unlink(config->filepath);
                    file_exists = false;
                } else {
                    // Write reconstructed header to disk
                    if (tsdb_write_header(g_state.file, &g_state.header) != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to write reconstructed header");
                        fclose(g_state.file);
                        tsdb_free_buffer_pool(&g_state.pool);
                        return ESP_FAIL;
                    }

                    ESP_LOGI(TAG, "Header successfully reconstructed and saved");
                    db_opened_successfully = true;
                }
            }
        } else {
            // Header read successfully, no reconstruction needed
            db_opened_successfully = true;
        }

        if (db_opened_successfully) {
            // V1 backward compatibility: base_params was reserved (0)
            if (g_state.header.base_params == 0) {
                g_state.header.base_params = g_state.header.num_params;
            }

            if (g_state.header.num_params != config->num_params) {
                ESP_LOGW(TAG, "Parameter count mismatch: file has %d, config has %d",
                         g_state.header.num_params, config->num_params);
                // Allow opening but warn
            }

            // V2->V3 block layout migration: fix databases where records_per_block > 38
            // V2 used struct-based offsets (timestamps[38], params[16][38]) which are wrong
            // when records_per_block > 38. V3 uses runtime-calculated offsets.
            if (g_state.header.version < 3 && g_state.header.records_per_block > 38) {
                ESP_LOGW(TAG, "Migrating V2 block layout (rpb=%d) to V3",
                         g_state.header.records_per_block);

                uint16_t rpb = g_state.header.records_per_block;
                uint8_t np = g_state.header.num_params;
                uint32_t max_recs = g_state.header.max_records > 0 ?
                    g_state.header.max_records : g_state.header.total_records;
                uint32_t total_blocks = (max_recs + rpb - 1) / rpb;

                // Old struct-based offsets
                #define OLD_TS_OFFSET(r)       (8 + (r) * 4)
                #define OLD_PARAM_OFFSET(p, r) (160 + (p) * 76 + (r) * 2)

                uint8_t disk_block[TSDB_BLOCK_SIZE];
                uint8_t new_block[TSDB_BLOCK_SIZE];

                for (uint32_t b = 0; b < total_blocks; b++) {
                    uint32_t blk_offset = tsdb_calc_block_offset(&g_state.header, b);
                    fseek(g_state.file, blk_offset, SEEK_SET);
                    if (fread(disk_block, TSDB_BLOCK_SIZE, 1, g_state.file) != 1) continue;
                    if (TSDB_BLOCK_MAGIC(disk_block) != 0x424C4B54) continue;

                    uint16_t count = TSDB_BLOCK_COUNT(disk_block);
                    if (count == 0) continue;

                    memset(new_block, 0, TSDB_BLOCK_SIZE);
                    TSDB_BLOCK_MAGIC(new_block) = 0x424C4B54;
                    TSDB_BLOCK_COUNT(new_block) = count;

                    for (uint16_t r = 0; r < count && r < rpb; r++) {
                        // Timestamps at same offset in both layouts
                        TSDB_BLOCK_TS(new_block, r) = *(uint32_t*)(disk_block + OLD_TS_OFFSET(r));

                        for (uint8_t p = 0; p < np; p++) {
                            uint32_t old_off = OLD_PARAM_OFFSET(p, r);
                            int16_t val = (old_off + 2 <= TSDB_BLOCK_SIZE) ?
                                          *(int16_t*)(disk_block + old_off) : 0;
                            TSDB_BLOCK_PARAM(new_block, rpb, p, r) = val;
                        }
                    }

                    fseek(g_state.file, blk_offset, SEEK_SET);
                    fwrite(new_block, TSDB_BLOCK_SIZE, 1, g_state.file);

                    if (b % 100 == 0 && b > 0) {
                        ESP_LOGI(TAG, "  Block migration: %lu / %lu",
                                 (unsigned long)b, (unsigned long)total_blocks);
                    }
                }

                #undef OLD_TS_OFFSET
                #undef OLD_PARAM_OFFSET

                fflush(g_state.file);
                fsync(fileno(g_state.file));

                g_state.header.version = 3;
                tsdb_write_header(g_state.file, &g_state.header);
                ESP_LOGI(TAG, "V2->V3 block layout migration complete (%lu blocks)",
                         (unsigned long)total_blocks);
            } else if (g_state.header.version < 3) {
                // No migration needed (rpb <= 38), just bump version
                g_state.header.version = 3;
                tsdb_write_header(g_state.file, &g_state.header);
            }

            // Load overflow state from header
            if (g_state.header.extra_param_count > 0 && g_state.header.overflow_offset > 0) {
                g_state.extra_param_count = g_state.header.extra_param_count;
                g_state.overflow_record_size = g_state.header.overflow_record_size;
                g_state.first_overflow_record_idx = g_state.header.first_overflow_record_idx;
                g_state.overflow_data_offset = g_state.header.overflow_offset + TSDB_OVERFLOW_HEADER_SIZE;
                ESP_LOGI(TAG, "Overflow active: %d extra params, first_idx=%lu",
                         g_state.extra_param_count,
                         (unsigned long)g_state.first_overflow_record_idx);
            }

            ESP_LOGI(TAG, "Opened existing database: %lu records, %lu writes, %lu evictions",
                     (unsigned long)g_state.header.total_records,
                     (unsigned long)g_state.header.total_writes,
                     (unsigned long)g_state.header.total_evictions);
        }
    }

    // Create new file if it doesn't exist or was deleted due to corruption
    if (!file_exists || !db_opened_successfully) {
        // Create new file
        ESP_LOGI(TAG, "Creating new database file");
        g_state.file = fopen(config->filepath, "w+b");
        if (g_state.file == NULL) {
            ESP_LOGE(TAG, "Failed to create new file");
            tsdb_free_buffer_pool(&g_state.pool);
            return ESP_FAIL;
        }

        // Initialize header
        memset(&g_state.header, 0, sizeof(tsdb_header_t));
        g_state.header.magic = TSDB_MAGIC;
        g_state.header.version = TSDB_VERSION;
        g_state.header.num_params = config->num_params;
        g_state.header.base_params = config->num_params;
        g_state.header.param_size = sizeof(int16_t);
        g_state.header.record_size = 4 + (config->num_params * 2);

        // Calculate records per block
        // Block has: 4 bytes magic + 2 bytes count + 2 bytes flags = 8 bytes overhead
        // Then: timestamps array (4 bytes each) + params arrays (2 bytes each)
        size_t overhead = 8;
        size_t per_record = 4 + (config->num_params * 2);  // timestamp + params
        g_state.header.records_per_block = (TSDB_BLOCK_SIZE - overhead) / per_record;

        ESP_LOGI(TAG, "Records per block: %d", g_state.header.records_per_block);

        g_state.header.max_records = config->max_records;
        g_state.header.index_stride = config->index_stride > 0 ? config->index_stride : 380;

        // Calculate index offset (right after 512-byte header)
        g_state.header.index_offset = 512;
        g_state.header.index_entries = config->max_records > 0 ?
            (config->max_records / g_state.header.index_stride) + 1 : 256;  // 256 default for unlimited

        ESP_LOGI(TAG, "Index: %lu entries, stride=%lu",
                 (unsigned long)g_state.header.index_entries,
                 (unsigned long)g_state.header.index_stride);

        // Copy parameter names if provided
        if (config->param_names) {
            for (int i = 0; i < config->num_params && i < 16; i++) {
                strncpy(g_state.header.param_names[i], config->param_names[i], 31);
                g_state.header.param_names[i][31] = '\0';
            }
        }

        // Write initial header
        if (tsdb_write_header(g_state.file, &g_state.header) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write header");
            fclose(g_state.file);
            tsdb_free_buffer_pool(&g_state.pool);
            return ESP_FAIL;
        }

        // Pre-allocate index space (write zeros)
        fseek(g_state.file, g_state.header.index_offset, SEEK_SET);
        tsdb_index_entry_t zero_entry = {0};
        for (uint32_t i = 0; i < g_state.header.index_entries; i++) {
            fwrite(&zero_entry, sizeof(zero_entry), 1, g_state.file);
        }
        tsdb_flush_and_sync(g_state.file);

        ESP_LOGI(TAG, "New database created successfully");
    }

    // Save filepath
    strncpy(g_state.filepath, config->filepath, sizeof(g_state.filepath) - 1);
    g_state.is_open = true;

    ESP_LOGI(TAG, "TSDB initialized successfully");

    return ESP_OK;
}

esp_err_t tsdb_close(void) {
    if (!g_state.is_open) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Closing TSDB");

    // Flush any cached writes
    if (g_state.cache_dirty) {
        ESP_LOGW(TAG, "Flushing dirty cache on close");
        // TODO: Implement write cache flush
    }

    // Update header
    tsdb_write_header(g_state.file, &g_state.header);

    // Close file
    fclose(g_state.file);
    g_state.file = NULL;

    // Free buffer pool
    tsdb_free_buffer_pool(&g_state.pool);

    g_state.is_open = false;

    ESP_LOGI(TAG, "TSDB closed");

    return ESP_OK;
}

bool tsdb_is_initialized(void) {
    return g_state.is_open;
}

esp_err_t tsdb_get_stats(tsdb_stats_t *stats) {
    if (!g_state.is_open) {
        return ESP_ERR_INVALID_STATE;
    }

    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    stats->total_records = (g_state.header.max_records == 0 ||
                            g_state.header.total_records < g_state.header.max_records) ?
                           g_state.header.total_records : g_state.header.max_records;
    stats->max_records = g_state.header.max_records;
    stats->oldest_timestamp = g_state.header.oldest_timestamp;
    stats->newest_timestamp = g_state.header.newest_timestamp;
    stats->total_writes = g_state.header.total_writes;
    stats->total_evictions = g_state.header.total_evictions;
    stats->num_params = g_state.header.num_params;
    stats->extra_params = g_state.extra_param_count;
    stats->buffer_pool_size = g_state.pool.total_size;
    stats->using_paged_allocation = g_state.pool.is_paged;

    // Get file size
    struct stat st;
    if (stat(g_state.filepath, &st) == 0) {
        stats->storage_bytes = st.st_size;
    } else {
        stats->storage_bytes = 0;
    }

    return ESP_OK;
}

esp_err_t tsdb_clear(void) {
    if (!g_state.is_open) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Clearing all data");

    // Reset header counters
    g_state.header.total_records = 0;
    g_state.header.oldest_record_idx = 0;
    g_state.header.newest_record_idx = 0;
    g_state.header.oldest_timestamp = 0;
    g_state.header.newest_timestamp = 0;
    g_state.header.total_writes = 0;
    g_state.header.total_evictions = 0;

    // Reset overflow if present
    if (g_state.header.overflow_offset > 0) {
        g_state.header.first_overflow_record_idx = 0;
        g_state.first_overflow_record_idx = 0;
    }

    // Clear index
    fseek(g_state.file, g_state.header.index_offset, SEEK_SET);
    tsdb_index_entry_t zero_entry = {0};
    for (uint32_t i = 0; i < g_state.header.index_entries; i++) {
        fwrite(&zero_entry, sizeof(zero_entry), 1, g_state.file);
    }

    // Write updated header
    tsdb_write_header(g_state.file, &g_state.header);
    tsdb_flush_and_sync(g_state.file);

    ESP_LOGI(TAG, "Database cleared");

    return ESP_OK;
}

esp_err_t tsdb_delete(void) {
    if (!g_state.is_open) {
        return ESP_ERR_INVALID_STATE;
    }

    char filepath_copy[128];
    strncpy(filepath_copy, g_state.filepath, sizeof(filepath_copy));

    // Close first
    tsdb_close();

    // Delete file
    if (unlink(filepath_copy) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath_copy);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Database deleted: %s", filepath_copy);

    return ESP_OK;
}

// ============================================================================
// OVERFLOW PARAMETER API
// ============================================================================

esp_err_t tsdb_add_extra_params(const char **param_names, uint8_t count) {
    if (!g_state.is_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count == 0 || count > TSDB_MAX_EXTRA_PARAMS || param_names == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_state.extra_param_count > 0) {
        ESP_LOGW(TAG, "Overflow already active with %d params", g_state.extra_param_count);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Adding %d extra parameters (overflow region)", count);

    // Calculate overflow offset = end of current file
    fseek(g_state.file, 0, SEEK_END);
    uint32_t overflow_offset = (uint32_t)ftell(g_state.file);

    // Build overflow header
    tsdb_overflow_header_t ovf_header;
    memset(&ovf_header, 0, sizeof(ovf_header));
    ovf_header.magic = TSDB_OVERFLOW_MAGIC;
    ovf_header.param_count = count;
    ovf_header.first_record_idx = g_state.header.total_records;

    for (uint8_t i = 0; i < count; i++) {
        strncpy(ovf_header.param_names[i], param_names[i], 19);
        ovf_header.param_names[i][19] = '\0';
    }

    // Write overflow header
    fseek(g_state.file, overflow_offset, SEEK_SET);
    if (fwrite(&ovf_header, sizeof(ovf_header), 1, g_state.file) != 1) {
        ESP_LOGE(TAG, "Failed to write overflow header");
        return ESP_FAIL;
    }
    fflush(g_state.file);
    fsync(fileno(g_state.file));

    // Update main header
    g_state.header.extra_param_count = count;
    g_state.header.overflow_record_size = count * sizeof(int16_t);
    g_state.header.overflow_offset = overflow_offset;
    g_state.header.first_overflow_record_idx = g_state.header.total_records;

    if (tsdb_write_header(g_state.file, &g_state.header) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update header with overflow info");
        return ESP_FAIL;
    }

    // Update runtime state
    g_state.extra_param_count = count;
    g_state.overflow_record_size = count * sizeof(int16_t);
    g_state.first_overflow_record_idx = g_state.header.total_records;
    g_state.overflow_data_offset = overflow_offset + TSDB_OVERFLOW_HEADER_SIZE;

    ESP_LOGI(TAG, "Overflow region created: offset=%lu, %d params, record_size=%d",
             (unsigned long)overflow_offset, count, g_state.overflow_record_size);

    return ESP_OK;
}

esp_err_t tsdb_migrate_overflow(const char **new_names, uint8_t new_count) {
    if (!g_state.is_open) return ESP_ERR_INVALID_STATE;
    if (new_count > TSDB_MAX_EXTRA_PARAMS) return ESP_ERR_INVALID_ARG;
    if (new_count > 0 && new_names == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t old_count = g_state.extra_param_count;

    // Case: nothing to nothing
    if (old_count == 0 && new_count == 0) {
        ESP_LOGI(TAG, "No overflow to migrate");
        return ESP_OK;
    }

    // Case: no existing overflow, just add
    if (old_count == 0 && new_count > 0) {
        ESP_LOGI(TAG, "No existing overflow, creating new with %d params", new_count);
        return tsdb_add_extra_params(new_names, new_count);
    }

    // Case: remove overflow entirely
    if (new_count == 0) {
        ESP_LOGI(TAG, "Removing overflow region (%d params)", old_count);
        g_state.header.extra_param_count = 0;
        g_state.header.overflow_record_size = 0;
        g_state.header.overflow_offset = 0;
        g_state.header.first_overflow_record_idx = 0;
        g_state.extra_param_count = 0;
        g_state.overflow_record_size = 0;
        g_state.overflow_data_offset = 0;
        g_state.first_overflow_record_idx = 0;
        tsdb_write_header(g_state.file, &g_state.header);
        ESP_LOGI(TAG, "Overflow removed");
        return ESP_OK;
    }

    // Case: full migration (old overflow exists, new params specified)
    ESP_LOGI(TAG, "Migrating overflow: %d -> %d params", old_count, new_count);

    // 1. Read old overflow header to get old param names
    tsdb_overflow_header_t old_ovf;
    fseek(g_state.file, g_state.header.overflow_offset, SEEK_SET);
    if (fread(&old_ovf, sizeof(old_ovf), 1, g_state.file) != 1) {
        ESP_LOGE(TAG, "Failed to read old overflow header");
        return ESP_FAIL;
    }
    if (old_ovf.magic != TSDB_OVERFLOW_MAGIC) {
        ESP_LOGE(TAG, "Old overflow header corrupted (magic=0x%08lX)", (unsigned long)old_ovf.magic);
        return ESP_FAIL;
    }

    // Check if params already match (idempotent migration)
    if (old_count == new_count) {
        bool match = true;
        for (uint8_t i = 0; i < new_count; i++) {
            if (strncmp(new_names[i], old_ovf.param_names[i], 19) != 0) {
                match = false;
                break;
            }
        }
        if (match) {
            ESP_LOGI(TAG, "Overflow already matches requested layout, skipping migration");
            return ESP_OK;
        }
    }

    // 2. Build column mapping: new_col[i] -> old_col[j] or -1
    int8_t col_map[TSDB_MAX_EXTRA_PARAMS];
    for (uint8_t i = 0; i < new_count; i++) {
        col_map[i] = -1;
        for (uint8_t j = 0; j < old_count; j++) {
            if (strncmp(new_names[i], old_ovf.param_names[j], 19) == 0) {
                col_map[i] = (int8_t)j;
                ESP_LOGI(TAG, "  %s: copy from old[%d]", new_names[i], j);
                break;
            }
        }
        if (col_map[i] < 0) {
            ESP_LOGI(TAG, "  %s: new column (zeros)", new_names[i]);
        }
    }

    // 3. New overflow goes at end of file
    fseek(g_state.file, 0, SEEK_END);
    uint32_t new_overflow_offset = (uint32_t)ftell(g_state.file);

    // 4. Write new overflow header
    tsdb_overflow_header_t new_ovf;
    memset(&new_ovf, 0, sizeof(new_ovf));
    new_ovf.magic = TSDB_OVERFLOW_MAGIC;
    new_ovf.param_count = new_count;
    new_ovf.first_record_idx = g_state.first_overflow_record_idx;
    for (uint8_t i = 0; i < new_count; i++) {
        strncpy(new_ovf.param_names[i], new_names[i], 19);
        new_ovf.param_names[i][19] = '\0';
    }

    fseek(g_state.file, new_overflow_offset, SEEK_SET);
    if (fwrite(&new_ovf, sizeof(new_ovf), 1, g_state.file) != 1) {
        ESP_LOGE(TAG, "Failed to write new overflow header");
        return ESP_FAIL;
    }
    fflush(g_state.file);
    fsync(fileno(g_state.file));

    // 5. Migrate data record by record
    uint32_t new_data_offset = new_overflow_offset + TSDB_OVERFLOW_HEADER_SIZE;
    uint16_t new_record_size = new_count * sizeof(int16_t);
    uint16_t old_record_size = g_state.overflow_record_size;
    uint32_t old_data_offset = g_state.overflow_data_offset;

    uint32_t overflow_records = 0;
    if (g_state.header.total_records > g_state.first_overflow_record_idx) {
        overflow_records = g_state.header.total_records - g_state.first_overflow_record_idx;
    }

    ESP_LOGI(TAG, "Migrating %lu overflow records", (unsigned long)overflow_records);

    int16_t old_vals[TSDB_MAX_EXTRA_PARAMS];
    int16_t new_vals[TSDB_MAX_EXTRA_PARAMS];

    for (uint32_t r = 0; r < overflow_records; r++) {
        // Read old record
        uint32_t old_pos = old_data_offset + (r * old_record_size);
        fseek(g_state.file, old_pos, SEEK_SET);
        memset(old_vals, 0, sizeof(old_vals));
        if (fread(old_vals, sizeof(int16_t), old_count, g_state.file) != old_count) {
            // Partial read — fill with zeros
        }

        // Remap columns
        memset(new_vals, 0, sizeof(new_vals));
        for (uint8_t i = 0; i < new_count; i++) {
            if (col_map[i] >= 0) {
                new_vals[i] = old_vals[(uint8_t)col_map[i]];
            }
        }

        // Write to new position
        uint32_t new_pos = new_data_offset + (r * new_record_size);
        fseek(g_state.file, new_pos, SEEK_SET);
        fwrite(new_vals, sizeof(int16_t), new_count, g_state.file);

        if (r > 0 && r % 10000 == 0) {
            ESP_LOGI(TAG, "  Migrated %lu / %lu records", (unsigned long)r, (unsigned long)overflow_records);
            fflush(g_state.file);
            fsync(fileno(g_state.file));
        }
    }

    fflush(g_state.file);
    fsync(fileno(g_state.file));

    // 6. Update header — this is the commit point (crash-safe)
    g_state.header.overflow_offset = new_overflow_offset;
    g_state.header.extra_param_count = new_count;
    g_state.header.overflow_record_size = new_record_size;
    // first_overflow_record_idx stays the same

    tsdb_write_header(g_state.file, &g_state.header);

    // 7. Update runtime state
    g_state.extra_param_count = new_count;
    g_state.overflow_record_size = new_record_size;
    g_state.overflow_data_offset = new_data_offset;

    ESP_LOGI(TAG, "Migration complete: %d params, %lu records migrated",
             new_count, (unsigned long)overflow_records);

    return ESP_OK;
}

uint8_t tsdb_get_total_params(void) {
    if (!g_state.is_open) return 0;
    return g_state.header.num_params + g_state.extra_param_count;
}

const char* tsdb_get_param_name(uint8_t index) {
    if (!g_state.is_open) return NULL;

    if (index < g_state.header.num_params) {
        return g_state.header.param_names[index];
    }

    // Extra param -- read from overflow header in file
    uint8_t extra_idx = index - g_state.header.num_params;
    if (extra_idx >= g_state.extra_param_count || g_state.header.overflow_offset == 0) {
        return NULL;
    }

    // Read the name from overflow header on file
    static char name_buf[20];
    uint32_t name_offset = g_state.header.overflow_offset +
                           offsetof(tsdb_overflow_header_t, param_names) +
                           (extra_idx * 20);
    fseek(g_state.file, name_offset, SEEK_SET);
    if (fread(name_buf, 20, 1, g_state.file) != 1) {
        return NULL;
    }
    name_buf[19] = '\0';
    return name_buf;
}

bool tsdb_has_overflow(void) {
    return g_state.is_open && g_state.extra_param_count > 0;
}
