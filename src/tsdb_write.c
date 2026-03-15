/**
 * @file tsdb_write.c
 * @brief Write operations with LRU eviction and ring buffer
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <string.h>
#include <unistd.h>

static const char *TAG = "TSDB_WRITE";

// ============================================================================
// BLOCK I/O
// ============================================================================

/**
 * @brief Read a data block from file
 */
esp_err_t tsdb_read_block(FILE *file, uint32_t block_num, tsdb_block_t *block) {
    if (file == NULL || block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t block_offset = tsdb_calc_block_offset(&g_state.header, block_num);

    fseek(file, block_offset, SEEK_SET);
    size_t read = fread(block, TSDB_BLOCK_SIZE, 1, file);

    if (read != 1) {
        ESP_LOGD(TAG, "Block %lu not found or uninitialized", (unsigned long)block_num);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/**
 * @brief Write a data block to file
 */
esp_err_t tsdb_write_block(FILE *file, uint32_t block_num, const tsdb_block_t *block) {
    if (file == NULL || block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t block_offset = tsdb_calc_block_offset(&g_state.header, block_num);

    fseek(file, block_offset, SEEK_SET);
    size_t written = fwrite(block, TSDB_BLOCK_SIZE, 1, file);
    fflush(file);
    fsync(fileno(file));

    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write block %lu", (unsigned long)block_num);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Wrote block %lu at offset %lu",
             (unsigned long)block_num, (unsigned long)block_offset);

    return ESP_OK;
}

// ============================================================================
// WRITE OPERATIONS
// ============================================================================

esp_err_t tsdb_write(uint32_t timestamp, const int16_t *values) {
    if (!g_state.is_open) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (values == NULL) {
        ESP_LOGE(TAG, "NULL values pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate record index in ring buffer
    uint32_t record_idx = g_state.header.total_records % g_state.header.max_records;

    // Determine if this overwrites old data (LRU eviction)
    bool is_eviction = (g_state.header.total_records >= g_state.header.max_records);

    if (is_eviction) {
        g_state.header.total_evictions++;
        g_state.header.oldest_record_idx = (g_state.header.oldest_record_idx + 1) %
                                           g_state.header.max_records;
        ESP_LOGD(TAG, "LRU eviction: oldest_idx=%lu",
                 (unsigned long)g_state.header.oldest_record_idx);
    }

    // Calculate block number and offset within block
    uint32_t block_num = record_idx / g_state.header.records_per_block;
    uint16_t offset_in_block = record_idx % g_state.header.records_per_block;

    ESP_LOGD(TAG, "Writing record %lu: block=%lu, offset=%d",
             (unsigned long)g_state.header.total_records,
             (unsigned long)block_num, offset_in_block);

    // Get pointer to write buffer in pool
    tsdb_block_t *block = (tsdb_block_t*)tsdb_get_buffer_ptr(&g_state.pool,
                                                              g_state.write_cache_offset,
                                                              sizeof(tsdb_block_t));

    tsdb_block_t temp_block;
    if (block == NULL) {
        // Paged mode and spans pages, use temp buffer
        block = &temp_block;
    }

    // Read existing block
    esp_err_t ret = tsdb_read_block(g_state.file, block_num, block);

    // Initialize block if new or read failed
    uint8_t *raw_blk = (uint8_t *)block;
    if (ret != ESP_OK || TSDB_BLOCK_MAGIC(raw_blk) != 0x424C4B54) {
        ESP_LOGD(TAG, "Initializing new block %lu", (unsigned long)block_num);
        memset(block, 0, TSDB_BLOCK_SIZE);
        TSDB_BLOCK_MAGIC(raw_blk) = 0x424C4B54;  // "BLKT"
        TSDB_BLOCK_COUNT(raw_blk) = 0;
    }

    // Write data in columnar format (runtime offsets for correct disk layout)
    uint16_t rpb = g_state.header.records_per_block;
    uint8_t *raw = (uint8_t *)block;
    TSDB_BLOCK_TS(raw, offset_in_block) = timestamp;
    for (uint8_t i = 0; i < g_state.header.num_params; i++) {
        TSDB_BLOCK_PARAM(raw, rpb, i, offset_in_block) = values[i];
    }

    // Update block record count
    if (offset_in_block >= TSDB_BLOCK_COUNT(raw)) {
        TSDB_BLOCK_COUNT(raw) = offset_in_block + 1;
    }

    // Write block back to file
    ret = tsdb_write_block(g_state.file, block_num, block);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write block");
        return ret;
    }

    // Write overflow data if active
    if (g_state.extra_param_count > 0 &&
        g_state.header.total_records >= g_state.first_overflow_record_idx) {
        uint32_t overflow_idx = g_state.header.total_records - g_state.first_overflow_record_idx;
        uint32_t ovf_offset = g_state.overflow_data_offset +
                              (overflow_idx * g_state.overflow_record_size);

        fseek(g_state.file, ovf_offset, SEEK_SET);
        size_t ovf_written = fwrite(&values[g_state.header.num_params],
                                     sizeof(int16_t),
                                     g_state.extra_param_count,
                                     g_state.file);
        if (ovf_written != g_state.extra_param_count) {
            ESP_LOGE(TAG, "Failed to write overflow data");
            // Don't fail the whole write -- base data is already written
        }
    }

    // Update header
    g_state.header.total_records++;
    g_state.header.total_writes++;
    g_state.header.newest_record_idx = record_idx;
    g_state.header.newest_timestamp = timestamp;

    // Update oldest timestamp if needed
    if (is_eviction) {
        // Read oldest timestamp from file
        uint32_t oldest_block = g_state.header.oldest_record_idx / g_state.header.records_per_block;
        uint16_t oldest_offset = g_state.header.oldest_record_idx % g_state.header.records_per_block;

        tsdb_block_t *oldest_block_data = (tsdb_block_t*)tsdb_get_buffer_ptr(&g_state.pool,
                                                                              g_state.read_buffer_offset,
                                                                              sizeof(tsdb_block_t));
        tsdb_block_t temp_oldest;
        if (oldest_block_data == NULL) {
            oldest_block_data = &temp_oldest;
        }

        if (tsdb_read_block(g_state.file, oldest_block, oldest_block_data) == ESP_OK) {
            g_state.header.oldest_timestamp = TSDB_BLOCK_TS((uint8_t *)oldest_block_data, oldest_offset);
        }
    } else if (g_state.header.total_records == 1) {
        g_state.header.oldest_timestamp = timestamp;
    }

    // Update sparse index if at stride boundary
    if (record_idx % g_state.header.index_stride == 0) {
        uint32_t index_entry_num = record_idx / g_state.header.index_stride;
        tsdb_index_entry_t entry = {
            .timestamp = timestamp,
            .block_number = block_num
        };

        uint32_t index_file_offset = g_state.header.index_offset +
                                     (index_entry_num * sizeof(tsdb_index_entry_t));

        fseek(g_state.file, index_file_offset, SEEK_SET);
        fwrite(&entry, sizeof(tsdb_index_entry_t), 1, g_state.file);

        ESP_LOGD(TAG, "Updated index entry %lu: timestamp=%lu, block=%lu",
                 (unsigned long)index_entry_num,
                 (unsigned long)timestamp,
                 (unsigned long)block_num);
    }

    // Update header in file
    tsdb_write_header(g_state.file, &g_state.header);
    fflush(g_state.file);
    fsync(fileno(g_state.file));

    ESP_LOGD(TAG, "Write complete: total_records=%lu, newest_ts=%lu",
             (unsigned long)g_state.header.total_records,
             (unsigned long)g_state.header.newest_timestamp);

    return ESP_OK;
}

esp_err_t tsdb_write_batch(const uint32_t *timestamps,
                            const int16_t **values,
                            uint32_t count) {
    if (!g_state.is_open) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timestamps == NULL || values == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Batch writing %lu records", (unsigned long)count);

    // Write records one by one
    // TODO: Optimize by batching block writes
    for (uint32_t i = 0; i < count; i++) {
        esp_err_t ret = tsdb_write(timestamps[i], values[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write record %lu in batch", (unsigned long)i);
            return ret;
        }
    }

    ESP_LOGI(TAG, "Batch write complete: %lu records", (unsigned long)count);

    return ESP_OK;
}
