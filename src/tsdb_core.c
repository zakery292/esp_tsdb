/**
 * @file tsdb_core.c
 * @brief Handle lifecycle, open/close, schema and capacity operations, stats.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "TSDB_CORE";

static esp_err_t schema_bump(tsdb_t *db);

// Legacy single-DB handle backing the global API.
tsdb_t *g_default_handle = NULL;

// ============================================================================
// HANDLE RECYCLER
// ============================================================================
// A closed handle's MUTEX MUST NEVER BE DELETED: a task that passed its
// pre-lock is_open check can be blocked inside xSemaphoreTake on that exact
// mutex, and deleting it under a blocked waiter is a use-after-free (seen
// live). Retired handles are parked here, mutex alive, and reused by the next
// tsdb_open(); stragglers wake on a valid mutex, re-check is_open +
// generation (TSDB_LOCK_OPEN_OR_RETURN), and bail.

static tsdb_t *s_recycled_handle = NULL;

static void tsdb_retire_handle(tsdb_t *db) {
    if (db == NULL) return;
    free(db->segs);
    db->segs = NULL;
    db->nsegs = db->segs_cap = 0;
    if (s_recycled_handle == NULL) {
        s_recycled_handle = db;
    } else {
        ESP_LOGW(TAG, "handle recycler occupied — leaking retired handle (%u bytes + mutex)",
                 (unsigned) sizeof(tsdb_t));
    }
}

static tsdb_t *tsdb_acquire_handle(void) {
    tsdb_t *db = s_recycled_handle;
    if (db != NULL) {
        s_recycled_handle = NULL;
        SemaphoreHandle_t m = db->mutex;
        uint32_t gen = db->generation;
        memset(db, 0, sizeof(*db));
        db->mutex = m;
        db->generation = gen + 1;
    } else {
        db = (tsdb_t *) calloc(1, sizeof(tsdb_t));
        if (db == NULL) return NULL;
        db->mutex = xSemaphoreCreateRecursiveMutex();
        if (db->mutex == NULL) {
            free(db);
            return NULL;
        }
    }
    tsdb_write_cache_invalidate(db);
    db->next_seq = 1;
    return db;
}

// ============================================================================
// SCHEMA HELPERS
// ============================================================================

static void schema_set_name(char *dst, const char *src) {
    memset(dst, 0, TSDB_NAME_LEN);
    if (src) strncpy(dst, src, TSDB_NAME_LEN - 1);
}

static bool schema_equal(const tsdb_schema_t *a, uint8_t count, uint8_t base,
                         char names[][TSDB_NAME_LEN], char aliases[][TSDB_NAME_LEN]) {
    if (a->count != count || a->base_count != base) return false;
    for (uint8_t i = 0; i < count; i++) {
        if (strncmp(a->names[i], names[i], TSDB_NAME_LEN - 1) != 0) return false;
        if (aliases && strncmp(a->aliases[i], aliases[i], TSDB_NAME_LEN - 1) != 0) return false;
    }
    return true;
}

uint32_t tsdb_total_writes(const tsdb_t *db) {
    if (db->nsegs == 0) return 0;
    // Every entry carries its lifetime base, legacy pieces and converted
    // segments included (the 2.x write count, less what came after them).
    const tsdb_seg_t *n = &db->segs[db->nsegs - 1];
    return n->lifetime_base + n->records;
}

void *tsdb_scratch_block(tsdb_t *db, void **heap) {
    *heap = NULL;
    void *buf = tsdb_get_buffer_ptr(&db->pool, db->read_buffer_offset, TSDB_BLOCK_SIZE);
    if (buf == NULL) {
        *heap = malloc(TSDB_BLOCK_SIZE);   // never a 1 KB stack buffer
        buf = *heap;
    }
    return buf;
}

uint32_t tsdb_oldest_retained_ts(tsdb_t *db) {
    if (db->nsegs == 0 || tsdb_retained(db) == 0) return 0;
    tsdb_seg_t *o = &db->segs[0];
    if (db->head_skip == 0) return o->first_ts;

    // Pool read buffer when it is contiguous, else a heap block — never a
    // 1 KB stack buffer (stats are read from small tasks, including the
    // dongle's 4.6 KB main task at boot).
    void *heapbuf;
    uint8_t *buf = tsdb_scratch_block(db, &heapbuf);
    if (buf == NULL) return o->first_ts;

    uint32_t ts = o->first_ts;
    uint32_t block; uint16_t off;
    if (o->kind == TSDB_SEG_LEGACY) {
        if (db->legacy) {
            uint32_t t = tsdb_legacy_ts_at(db->legacy, o->rank + db->head_skip, buf);
            if (t != 0) ts = t;
        }
        free(heapbuf);
        return ts;
    }
    block = db->head_skip / o->rpb;
    off = (uint16_t)(db->head_skip % o->rpb);
    FILE *f = (db->nsegs == 1 && o->kind == TSDB_SEG_NORMAL) ? db->active : NULL;
    bool own = false;
    if (f == NULL) {
        char path[160];
        tsdb_entry_path(db, o, path, sizeof(path));
        f = fopen(path, "rb");
        if (f == NULL) { free(heapbuf); return ts; }
        own = true;
    }
    if (tsdb_seg_read_block(f, o->data_offset, block, buf) == ESP_OK) {
        ts = TSDB_BLOCK_TS(buf, off);
    }
    if (own) fclose(f);
    free(heapbuf);
    return ts;
}

bool tsdb_refuse_if_query_open(tsdb_t *db, const char *what) {
    if (db->open_queries == 0) return false;
    ESP_LOGE(TAG, "%s: refused while %u quer%s open on this handle — close them first",
             what, db->open_queries, db->open_queries == 1 ? "y is" : "ies are");
    tsdb_unlock(db);
    return true;
}

// ============================================================================
// OPEN
// ============================================================================

static void teardown(tsdb_t *db) {
    if (db->active) { fclose(db->active); db->active = NULL; }
    if (db->legacy) { tsdb_legacy_close(db->legacy); db->legacy = NULL; }
    tsdb_free_buffer_pool(&db->pool);
    tsdb_retire_handle(db);
}

static uint32_t auto_seg_blocks(const tsdb_config_t *config, uint8_t np) {
    if (config->segment_bytes > 0) {
        uint32_t b = config->segment_bytes / TSDB_BLOCK_SIZE;
        if (b < 1) b = 1;
        if (b > 4096) b = 4096;
        return b;
    }
    uint32_t b = 256;
    if (config->max_records > 0) {
        uint16_t rpb = (TSDB_BLOCK_SIZE - 8) / (4 + 2u * np);
        uint32_t max_blocks = (config->max_records + rpb - 1) / rpb;
        b = (max_blocks + 31) / 32;
        // Floor: a segment is a file, and a file costs at least one 4 KB
        // flash block on littlefs. At 1 data block per segment (any capacity
        // under ~1800 records) the window plus its slack took ~33 files =
        // 132 KB — more than the whole 128 KB LittleFS of a 4 MB board.
        if (b < 6) b = 6;
        if (b > 256) b = 256;
    }
    // Size the whole file (header blocks + data) at 4n - 1 KB: littlefs
    // stores a file in 4 KB blocks, each after the first carrying a few bytes
    // of skip-list pointers, so a file of exactly 4n KB spills into an
    // (n+1)th block. 4n - 1 KB fills n blocks with no spill (81 KB files took
    // 21 blocks = 84 KB each; 83 KB files take the same 21).
    uint32_t prefix = TSDB_SEG_HEADER_SIZE + 2u * np * TSDB_NAME_LEN;
    uint32_t hdr = (prefix + TSDB_BLOCK_SIZE - 1) / TSDB_BLOCK_SIZE;
    uint32_t total = hdr + b;
    total = (total + 1 + 3) / 4 * 4 - 1;
    return total - hdr;
}

tsdb_t *tsdb_open(const tsdb_config_t *config) {
    if (config == NULL || config->filepath == NULL) {
        ESP_LOGE(TAG, "Invalid config");
        return NULL;
    }
    if (config->num_params == 0 || config->num_params > TSDB_MAX_PARAMS) {
        ESP_LOGE(TAG, "Invalid num_params: %d (must be 1-%d)", config->num_params, TSDB_MAX_PARAMS);
        return NULL;
    }
    if (config->buffer_pool_size == 0) {
        ESP_LOGE(TAG, "buffer_pool_size must be > 0");
        return NULL;
    }
    if (strlen(config->filepath) >= sizeof(((tsdb_t *) 0)->filepath) - 16) {
        ESP_LOGE(TAG, "filepath too long");
        return NULL;
    }

    tsdb_t *db = tsdb_acquire_handle();
    if (db == NULL) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        return NULL;
    }
    strncpy(db->filepath, config->filepath, sizeof(db->filepath) - 1);
    db->free_space_cb = config->free_space_cb;
    db->min_free_bytes = config->min_free_bytes;
    db->cfg_max_records = config->max_records;
    db->max_records = config->max_records;

    ESP_LOGI(TAG, "Opening TSDB %s: %d params, max %lu records, buffer %u KB",
             config->filepath, config->num_params, (unsigned long) config->max_records,
             (unsigned) (config->buffer_pool_size / 1024));

    if (tsdb_alloc_buffer_pool(&db->pool, config->buffer_pool_size,
                               config->use_paged_allocation, config->page_size,
                               config->alloc_strategy) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate buffer pool");
        tsdb_retire_handle(db);
        return NULL;
    }
    db->read_buffer_offset = 0;
    db->write_cache_offset = TSDB_BLOCK_SIZE;
    db->query_buffer_offset = 2 * TSDB_BLOCK_SIZE;
    if (db->pool.total_size < 3 * TSDB_BLOCK_SIZE) {
        ESP_LOGE(TAG, "buffer_pool_size must be at least %d bytes", 3 * TSDB_BLOCK_SIZE);
        teardown(db);
        return NULL;
    }

    // ---- 1. Legacy single file (pre-3.0): adopted read-only; tsdb_convert.c
    //         moves it into segments in bounded steps after the open.
    tsdb_legacy_t *legacy = NULL;
    esp_err_t lr = tsdb_legacy_open(config->filepath, config, &legacy);
    if (lr != ESP_OK && lr != ESP_ERR_NOT_FOUND && lr != ESP_ERR_INVALID_RESPONSE) {
        // The legacy file exists and could not be adopted now (out of
        // memory, an I/O error). Failing the open would disable history until
        // the next reboot; run without the old history instead and leave its
        // files alone (never deleted or converted in this state). New records
        // go to segments, which sort after it; the next adoption attempt
        // brings it back in order.
        ESP_LOGE(TAG, "Cannot adopt the pre-3.0 database at %s now (%d) — opening without its history",
                 config->filepath, lr);
        db->legacy_retry = true;
    }
    if (lr == ESP_OK) {
        if (!legacy->needs_scan && legacy->available == 0 && !legacy->marker) {
            // Empty legacy file: nothing to adopt, drop it.
            char path[128];
            strncpy(path, legacy->path, sizeof(path));
            tsdb_legacy_close(legacy);
            tsdb_legacy_remove(path);
        } else {
            db->legacy = legacy;
            legacy->conv.phase = legacy->needs_scan ? TSDB_CONV_SCAN : TSDB_CONV_START;
        }
    }

    // ---- 2. Segment files (converted ones included).
    if (tsdb_seg_scan(db) != ESP_OK) {
        ESP_LOGE(TAG, "Segment scan failed");
        teardown(db);
        return NULL;
    }

    // ---- 2b. What the 2.x file still holds, less what is already converted.
    if (db->legacy && !db->legacy->needs_scan && tsdb_conv_adopt(db) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot adopt the pre-3.0 database now — opening without its history");
        tsdb_legacy_close(db->legacy);
        db->legacy = NULL;
        db->legacy_retry = true;
    }

    // ---- 3. Current schema = config base columns + persisted runtime extras.
    tsdb_schema_t *s = &db->schema;
    memset(s, 0, sizeof(*s));
    s->base_count = config->num_params;
    s->count = config->num_params;
    for (uint8_t i = 0; i < config->num_params; i++) {
        schema_set_name(s->names[i], config->param_names ? config->param_names[i] : NULL);
    }

    const tsdb_seg_t *newest = db->nsegs ? &db->segs[db->nsegs - 1] : NULL;
    bool newest_is_segment = newest && newest->kind == TSDB_SEG_NORMAL;
    // Records converted from a 2.x file (the 2.x file itself gone) carry its
    // columns, extras included, like a segment does.
    bool newest_is_conv = newest && newest->kind == TSDB_SEG_CONV && db->legacy == NULL;
    if (newest_is_segment || newest_is_conv) {
        char (*names)[TSDB_NAME_LEN] = calloc(newest->np, TSDB_NAME_LEN);
        char (*aliases)[TSDB_NAME_LEN] = calloc(newest->np, TSDB_NAME_LEN);
        uint8_t base = 0;
        if (names == NULL || aliases == NULL ||
            tsdb_seg_read_schema(db, newest, NULL, names, aliases, &base) != ESP_OK) {
            free(names); free(aliases);
            ESP_LOGE(TAG, "Cannot read newest segment's schema");
            teardown(db);
            return NULL;
        }
        if (base > newest->np) base = newest->np;

        if (config->param_names == NULL && base != config->num_params) {
            ESP_LOGE(TAG, "Parameter count mismatch: file has %u base columns, config %u, and no "
                          "names to match by — refusing to open", base, config->num_params);
            free(names); free(aliases);
            teardown(db);
            return NULL;
        }

        // Carry aliases for columns whose names are unchanged.
        for (uint8_t c = 0; c < s->base_count; c++) {
            for (uint8_t j = 0; j < base; j++) {
                if (strncmp(names[j], s->names[c], TSDB_NAME_LEN - 1) == 0) {
                    memcpy(s->aliases[c], aliases[j], TSDB_NAME_LEN);
                    break;
                }
            }
        }
        // Runtime extras persist in the newest segment.
        for (uint8_t j = base; j < newest->np && s->count < TSDB_MAX_PARAMS; j++) {
            memcpy(s->names[s->count], names[j], TSDB_NAME_LEN);
            memcpy(s->aliases[s->count], aliases[j], TSDB_NAME_LEN);
            s->count++;
        }
        if (newest_is_conv) {
            s->gen = 1;                            // never appended to: the next write starts a segment
        } else if (schema_equal(s, newest->np, base, names, aliases)) {
            s->gen = newest->schema_gen;           // identical: keep appending to it
        } else {
            s->gen = newest->schema_gen + 1;       // next write starts a new segment
            ESP_LOGW(TAG, "Schema differs from the newest segment — columns map by name");
        }
        free(names); free(aliases);
    } else {
        if (db->legacy) {
            if (config->param_names == NULL && db->legacy->header.num_params != config->num_params) {
                ESP_LOGE(TAG, "Parameter count mismatch: legacy file has %u columns, config %u, and "
                              "no names to match by — refusing to open",
                         db->legacy->header.num_params, config->num_params);
                teardown(db);
                return NULL;
            }
            // Legacy overflow extras carry over as runtime extras.
            for (uint8_t j = db->legacy->header.num_params;
                 j < tsdb_legacy_columns(db->legacy) && s->count < TSDB_MAX_PARAMS; j++) {
                tsdb_legacy_column_name(db->legacy, j, s->names[s->count]);
                s->count++;
            }
        }
        s->gen = 1;
    }

    db->seg_blocks = auto_seg_blocks(config, s->count);

    // ---- 4. Capacity window.
    tsdb_apply_capacity(db);

    // ---- 5. Hold the newest segment open for appends (only if it still has
    //         the current schema; otherwise the next write creates a new one).
    newest = db->nsegs ? &db->segs[db->nsegs - 1] : NULL;
    if (newest && newest->kind == TSDB_SEG_NORMAL && newest->schema_gen == s->gen) {
        char path[160];
        tsdb_seg_path(db, newest->seq, path, sizeof(path));
        db->active = fopen(path, "r+b");
        if (db->active == NULL) {
            ESP_LOGE(TAG, "Cannot reopen active segment %s", path);
            teardown(db);
            return NULL;
        }
    }

    ESP_LOGI(TAG, "TSDB open: %u segments, %lu records retained (%lu physical), schema gen %lu",
             db->nsegs, (unsigned long) tsdb_retained(db), (unsigned long) db->total_physical,
             (unsigned long) s->gen);

    tsdb_lock(db, 10000);
    db->is_open = true;
    tsdb_unlock(db);
    return db;
}

// ============================================================================
// CLOSE / SYNC / DELETE / CLEAR
// ============================================================================

esp_err_t tsdb_close_h(tsdb_t *db) {
    if (db == NULL) return ESP_OK;
    db->closing = true;
    if (!tsdb_lock(db, 30000)) {
        ESP_LOGE(TAG, "tsdb_close: lock timeout — handle left open");
        db->closing = false;
        return ESP_ERR_TIMEOUT;
    }
    if (!db->is_open) {
        db->closing = false;
        tsdb_unlock(db);
        tsdb_retire_handle(db);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Closing TSDB");
    esp_err_t ret = ESP_OK;
    if (db->cache_dirty) ret = tsdb_write_cache_flush(db);
    tsdb_write_cache_invalidate(db);
    if (db->active) {
        if (fclose(db->active) != 0 && ret == ESP_OK) ret = ESP_FAIL;
        db->active = NULL;
    }
    if (db->legacy) { tsdb_legacy_close(db->legacy); db->legacy = NULL; }
    tsdb_free_buffer_pool(&db->pool);
    db->is_open = false;
    db->closing = false;
    tsdb_unlock(db);
    tsdb_retire_handle(db);
    return ret;
}

esp_err_t tsdb_sync_h(tsdb_t *db) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    TSDB_LOCK_OPEN_OR_RETURN(db, 30000, ESP_ERR_TIMEOUT);
    esp_err_t ret = ESP_OK;
    if (db->cache_dirty) ret = tsdb_write_cache_flush(db);
    if (db->active) {
        esp_err_t sr = tsdb_file_sync(db->active);
        if (ret == ESP_OK) ret = sr;
        if (fclose(db->active) != 0 && ret == ESP_OK) ret = ESP_FAIL;
        db->active = NULL;
        char path[160];
        tsdb_seg_path(db, db->segs[db->nsegs - 1].seq, path, sizeof(path));
        db->active = fopen(path, "r+b");
        if (db->active == NULL) {
            ESP_LOGE(TAG, "tsdb_sync_h: reopen failed for %s", path);
            db->is_open = false;
            tsdb_unlock(db);
            return ESP_FAIL;
        }
        tsdb_write_cache_invalidate(db);
    }
    tsdb_unlock(db);
    return ret;
}

bool tsdb_is_initialized_h(const tsdb_t *db) {
    return db != NULL && db->is_open;
}

esp_err_t tsdb_get_stats_h(tsdb_t *db, tsdb_stats_t *stats) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (stats == NULL) return ESP_ERR_INVALID_ARG;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);

    memset(stats, 0, sizeof(*stats));
    stats->total_records = tsdb_retained(db);
    stats->max_records = db->max_records;
    stats->oldest_timestamp = tsdb_oldest_retained_ts(db);
    stats->newest_timestamp = db->nsegs ? db->segs[db->nsegs - 1].last_ts : 0;
    stats->total_writes = tsdb_total_writes(db);
    stats->total_evictions = stats->total_writes > stats->total_records ?
                             stats->total_writes - stats->total_records : 0;
    stats->num_params = db->schema.base_count;
    stats->extra_params = db->schema.count - db->schema.base_count;
    stats->buffer_pool_size = db->pool.total_size;
    stats->using_paged_allocation = db->pool.is_paged;
    // Files: the 2.x file counts once however many pieces of it are live.
    uint32_t bytes = 0;
    uint16_t files = 0;
    for (uint16_t i = 0; i < db->nsegs; i++) {
        const tsdb_seg_t *s = &db->segs[i];
        if (s->kind == TSDB_SEG_LEGACY) continue;
        files++;
        bytes += s->data_offset + ((s->records + s->rpb - 1) / s->rpb) * TSDB_BLOCK_SIZE;
    }
    if (db->legacy) {
        files++;
        bytes += (uint32_t) tsdb_legacy_file_size(db->legacy);
    }
    stats->segment_count = files;
    stats->storage_bytes = bytes;

    tsdb_unlock(db);
    return ESP_OK;
}

esp_err_t tsdb_clear_h(tsdb_t *db) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "Clearing all data");
    tsdb_remove_all_files(db);
    db->capacity_adapted = false;
    db->max_records = db->cfg_max_records;
    schema_bump(db);                        // fresh segment carries the schema (incl. extras)
    tsdb_unlock(db);
    return ESP_OK;
}

esp_err_t tsdb_delete_h(tsdb_t *db) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    tsdb_remove_all_files(db);
    tsdb_unlock(db);
    esp_err_t ret = tsdb_close_h(db);
    ESP_LOGI(TAG, "Database deleted");
    return ret;
}

// ============================================================================
// SCHEMA OPERATIONS — all instant: the next write starts a new segment
// ============================================================================

// Start a new segment carrying the current schema RIGHT NOW, so the change
// is on disk even if no write follows before a reboot (a rename's alias, or
// extras added at runtime, live only in segment headers). An active segment
// that is still empty is simply replaced.
static esp_err_t schema_bump(tsdb_t *db) {
    db->schema.gen++;
    if (db->cache_dirty) tsdb_write_cache_flush(db);
    tsdb_write_cache_invalidate(db);
    if (db->active) { fclose(db->active); db->active = NULL; }
    if (db->nsegs > 0 && db->segs[db->nsegs - 1].kind == TSDB_SEG_NORMAL && db->segs[db->nsegs - 1].records == 0) {
        char path[160];
        tsdb_seg_path(db, db->segs[db->nsegs - 1].seq, path, sizeof(path));
        unlink(path);
        db->nsegs--;
    }
    uint32_t last_ts = db->nsegs ? db->segs[db->nsegs - 1].last_ts : 0;
    esp_err_t ret = tsdb_seg_create(db, last_ts);
    if (ret != ESP_OK) {
        // Out of space: the schema still takes effect at the next write,
        // which goes through the eviction path.
        ESP_LOGW(TAG, "Could not create a segment for the new schema now (%d); deferred to the next write", ret);
    }
    return ESP_OK;
}

esp_err_t tsdb_add_extra_params_h(tsdb_t *db, const char **param_names, uint8_t count) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (count == 0 || count > TSDB_MAX_EXTRA_PARAMS || param_names == NULL) return ESP_ERR_INVALID_ARG;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    tsdb_schema_t *s = &db->schema;
    if (s->count > s->base_count) {
        ESP_LOGW(TAG, "Extras already present (%u)", s->count - s->base_count);
        tsdb_unlock(db);
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint16_t) s->base_count + count > TSDB_MAX_PARAMS) {
        tsdb_unlock(db);
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < count; i++) {
        schema_set_name(s->names[s->count], param_names[i]);
        memset(s->aliases[s->count], 0, TSDB_NAME_LEN);
        s->count++;
    }
    schema_bump(db);
    ESP_LOGI(TAG, "Added %u extra columns (now %u)", count, s->count);
    tsdb_unlock(db);
    return ESP_OK;
}

esp_err_t tsdb_migrate_overflow_h(tsdb_t *db, const char **new_names, uint8_t new_count) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (new_count > TSDB_MAX_EXTRA_PARAMS) return ESP_ERR_INVALID_ARG;
    if (new_count > 0 && new_names == NULL) return ESP_ERR_INVALID_ARG;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    tsdb_schema_t *s = &db->schema;
    if ((uint16_t) s->base_count + new_count > TSDB_MAX_PARAMS) {
        tsdb_unlock(db);
        return ESP_ERR_INVALID_ARG;
    }
    bool same = (s->count == s->base_count + new_count);
    for (uint8_t i = 0; same && i < new_count; i++) {
        same = strncmp(s->names[s->base_count + i], new_names[i], TSDB_NAME_LEN - 1) == 0;
    }
    if (same) {
        tsdb_unlock(db);
        return ESP_OK;
    }
    s->count = s->base_count;
    for (uint8_t i = 0; i < new_count; i++) {
        schema_set_name(s->names[s->count], new_names[i]);
        memset(s->aliases[s->count], 0, TSDB_NAME_LEN);
        s->count++;
    }
    schema_bump(db);
    ESP_LOGI(TAG, "Extras now %u columns", new_count);
    tsdb_unlock(db);
    return ESP_OK;
}

esp_err_t tsdb_migrate_schema_h(tsdb_t *db, const char **new_names, uint8_t new_count,
                                const tsdb_migrate_opts_t *opts) {
    (void) opts;
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (new_names == NULL || new_count == 0 || new_count > TSDB_MAX_PARAMS) return ESP_ERR_INVALID_ARG;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    tsdb_schema_t *s = &db->schema;

    // Heap: tsdb_schema_t is ~2.6 KB (64 names + 64 aliases).
    tsdb_schema_t *ns = calloc(1, sizeof(*ns));
    if (ns == NULL) { tsdb_unlock(db); return ESP_ERR_NO_MEM; }
    ns->count = ns->base_count = new_count;
    for (uint8_t i = 0; i < new_count; i++) {
        schema_set_name(ns->names[i], new_names[i]);
        // keep the alias of a column that keeps its name
        for (uint8_t j = 0; j < s->count; j++) {
            if (strncmp(s->names[j], ns->names[i], TSDB_NAME_LEN - 1) == 0) {
                memcpy(ns->aliases[i], s->aliases[j], TSDB_NAME_LEN);
                break;
            }
        }
    }
    if (schema_equal(s, ns->count, ns->base_count, ns->names, ns->aliases)) {
        ESP_LOGI(TAG, "Schema already matches — nothing to do");
        free(ns);
        tsdb_unlock(db);
        return ESP_OK;
    }
    ns->gen = s->gen;
    *s = *ns;
    free(ns);
    schema_bump(db);
    ESP_LOGI(TAG, "Schema changed to %u columns (gen %lu); takes effect at the next write",
             new_count, (unsigned long) s->gen);
    tsdb_unlock(db);
    return ESP_OK;
}

esp_err_t tsdb_resize_h(tsdb_t *db, uint32_t new_max_records, const tsdb_migrate_opts_t *opts) {
    (void) opts;
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    db->cfg_max_records = new_max_records;
    db->max_records = new_max_records;
    db->capacity_adapted = false;
    tsdb_apply_capacity(db);
    ESP_LOGI(TAG, "Capacity now %lu records (%lu retained)",
             (unsigned long) new_max_records, (unsigned long) tsdb_retained(db));
    tsdb_unlock(db);
    return ESP_OK;
}

uint8_t tsdb_get_total_params_h(const tsdb_t *db) {
    if (db == NULL || !db->is_open) return 0;
    return db->schema.count;
}

const char *tsdb_get_param_name_h(const tsdb_t *db, uint8_t index) {
    if (db == NULL || !db->is_open || index >= db->schema.count) return NULL;
    return db->schema.names[index];
}

bool tsdb_has_overflow_h(const tsdb_t *db) {
    return db != NULL && db->is_open && db->schema.count > db->schema.base_count;
}

esp_err_t tsdb_rename_param_h(tsdb_t *db, uint8_t index, const char *new_name) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (new_name == NULL || new_name[0] == '\0') return ESP_ERR_INVALID_ARG;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    tsdb_schema_t *s = &db->schema;
    if (index >= s->count) {
        tsdb_unlock(db);
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(s->names[index], new_name, TSDB_NAME_LEN - 1) == 0) {
        tsdb_unlock(db);
        return ESP_OK;
    }
    memcpy(s->aliases[index], s->names[index], TSDB_NAME_LEN);
    schema_set_name(s->names[index], new_name);
    schema_bump(db);
    ESP_LOGI(TAG, "Renamed column %u \"%s\" -> \"%s\"", index, s->aliases[index], s->names[index]);
    tsdb_unlock(db);
    return ESP_OK;
}

int tsdb_find_param_h(tsdb_t *db, const char *name) {
    if (db == NULL || !db->is_open || name == NULL) return -1;
    for (uint8_t i = 0; i < db->schema.count; i++) {
        if (strncmp(db->schema.names[i], name, TSDB_NAME_LEN - 1) == 0) return (int) i;
    }
    return -1;
}

// ============================================================================
// LEGACY GLOBAL API
// ============================================================================

esp_err_t tsdb_init(const tsdb_config_t *config) {
    if (g_default_handle != NULL && g_default_handle->is_open) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    g_default_handle = tsdb_open(config);
    return g_default_handle != NULL ? ESP_OK : ESP_FAIL;
}

esp_err_t tsdb_close(void) {
    tsdb_t *db = g_default_handle;
    if (db == NULL) return ESP_OK;
    g_default_handle = NULL;
    esp_err_t ret = tsdb_close_h(db);
    if (ret != ESP_OK) g_default_handle = db;
    return ret;
}

bool tsdb_is_initialized(void) { return tsdb_is_initialized_h(g_default_handle); }
esp_err_t tsdb_get_stats(tsdb_stats_t *stats) { return tsdb_get_stats_h(g_default_handle, stats); }
esp_err_t tsdb_clear(void) { return tsdb_clear_h(g_default_handle); }

esp_err_t tsdb_delete(void) {
    tsdb_t *db = g_default_handle;
    if (db == NULL) return ESP_ERR_INVALID_STATE;
    g_default_handle = NULL;
    esp_err_t ret = tsdb_delete_h(db);
    // Refused (lock timeout, open query, not open): the handle is still
    // alive and still the default handle.
    if (ret != ESP_OK) g_default_handle = db;
    return ret;
}

esp_err_t tsdb_add_extra_params(const char **param_names, uint8_t count) {
    return tsdb_add_extra_params_h(g_default_handle, param_names, count);
}
esp_err_t tsdb_migrate_overflow(const char **new_names, uint8_t new_count) {
    return tsdb_migrate_overflow_h(g_default_handle, new_names, new_count);
}
esp_err_t tsdb_migrate_schema(const char **new_names, uint8_t new_count, const tsdb_migrate_opts_t *opts) {
    return tsdb_migrate_schema_h(g_default_handle, new_names, new_count, opts);
}
esp_err_t tsdb_resize(uint32_t new_max_records, const tsdb_migrate_opts_t *opts) {
    return tsdb_resize_h(g_default_handle, new_max_records, opts);
}
uint8_t tsdb_get_total_params(void) { return tsdb_get_total_params_h(g_default_handle); }
const char *tsdb_get_param_name(uint8_t index) { return tsdb_get_param_name_h(g_default_handle, index); }
bool tsdb_has_overflow(void) { return tsdb_has_overflow_h(g_default_handle); }
esp_err_t tsdb_rename_param(uint8_t index, const char *new_name) {
    return tsdb_rename_param_h(g_default_handle, index, new_name);
}
int tsdb_find_param(const char *name) { return tsdb_find_param_h(g_default_handle, name); }
