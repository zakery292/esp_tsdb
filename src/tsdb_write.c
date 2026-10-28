/**
 * @file tsdb_write.c
 * @brief Append path: active segment management, write cache, eviction.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <unistd.h>
#include <errno.h>

static const char *TAG = "TSDB_WRITE";

// ---------------------------------------------------------------------------
// Write cache
// ---------------------------------------------------------------------------

static tsdb_block_t *write_cache_ptr(tsdb_t *db) {
    return (tsdb_block_t *) tsdb_get_buffer_ptr(&db->pool, db->write_cache_offset, TSDB_BLOCK_SIZE);
}

void tsdb_reopen_active(tsdb_t *db) {
    tsdb_write_cache_invalidate(db);
    if (db->active == NULL || db->nsegs == 0) return;
    fclose(db->active);
    char path[160];
    tsdb_seg_path(db, db->segs[db->nsegs - 1].seq, path, sizeof(path));
    db->active = fopen(path, "r+b");
    if (db->active == NULL) {
        ESP_LOGE(TAG, "Reopen of %s after a failed write failed — next write starts a new segment", path);
    }
}

esp_err_t tsdb_write_cache_flush(tsdb_t *db) {
    if (db == NULL || !db->cache_dirty) return ESP_OK;
    tsdb_block_t *blk = write_cache_ptr(db);
    if (blk == NULL || db->cached_block_num == UINT32_MAX || db->active == NULL || db->nsegs == 0) {
        db->cache_dirty = false;
        return ESP_OK;
    }
    const tsdb_seg_t *a = &db->segs[db->nsegs - 1];
    esp_err_t ret = tsdb_seg_write_block(db->active, a->data_offset, db->cached_block_num, blk);
    db->cache_dirty = false;
    if (ret != ESP_OK) {
        tsdb_reopen_active(db);
    } else {
        // The dirty block is the tail block: everything counted is on disk.
        db->flushed_seq = a->seq;
        db->flushed_records = a->records;
        db->flushed_last_ts = a->last_ts;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Space management
// ---------------------------------------------------------------------------

/**
 * @brief Free-space / ENOSPC response: unlink the oldest segment, then latch
 *        the capacity at "what is on disk now plus the room the file set is
 *        about to use" (`room` records), so logical eviction keeps the file
 *        set from growing past this point. Only ever lowered; tsdb_resize()
 *        or a reopen restores the config value.
 *
 * @return false when nothing can be evicted (only the active segment left).
 */
static bool make_room(tsdb_t *db, uint32_t room, const char *why) {
    // A conversion round in progress holds up to a segment of space for
    // copies the 2.x file still has: give that back first (it backs off),
    // before anything is evicted.
    if (tsdb_conv_yield_space(db)) return true;
    uint32_t before = db->total_physical;
    if (db->nsegs > 0 && db->segs[0].kind == TSDB_SEG_LEGACY) {
        ESP_LOGE(TAG, "%s: removing the adopted pre-3.0 database file to make room", why);
    }
    if (!tsdb_evict_oldest(db, true)) {
        ESP_LOGW(TAG, "%s and nothing left to evict", why);
        return false;
    }
    // Cap at the footprint we had BEFORE evicting: in steady-state turnover
    // that is "what is on disk plus one segment"; after evicting a large
    // legacy file it keeps the whole former capacity instead of one segment.
    (void) room;
    uint32_t cap = before;
    if (db->max_records == 0 || cap < db->max_records) {
        db->max_records = cap;
        ESP_LOGW(TAG, "%s: capacity capped at %lu records", why, (unsigned long) cap);
    }
    db->capacity_adapted = true;
    tsdb_apply_capacity(db);
    return true;
}

static bool below_reserve(tsdb_t *db) {
    if (db->free_space_cb == NULL || db->min_free_bytes == 0) return false;
    uint64_t free_now = db->free_space_cb();
    if (free_now >= db->min_free_bytes) return false;
    ESP_LOGW(TAG, "Free space %llu below reserve %lu", (unsigned long long) free_now,
             (unsigned long) db->min_free_bytes);
    return true;
}

/**
 * @brief Ensure db->active is a segment with the current schema and room for
 *        one more record, creating a new one if needed.
 */
static esp_err_t ensure_active(tsdb_t *db, uint32_t ts) {
    const tsdb_seg_t *a = db->nsegs ? &db->segs[db->nsegs - 1] : NULL;
    bool usable = db->active != NULL && a != NULL && a->kind == TSDB_SEG_NORMAL &&
                  a->schema_gen == db->schema.gen &&
                  a->records < a->blocks_cap * (uint32_t) a->rpb;
    if (usable) return ESP_OK;

    if (db->cache_dirty) {
        esp_err_t fr = tsdb_write_cache_flush(db);
        if (fr != ESP_OK) return fr;
    }
    tsdb_write_cache_invalidate(db);
    if (db->active) { fclose(db->active); db->active = NULL; }

    uint16_t rpb = (TSDB_BLOCK_SIZE - 8) / (4 + 2u * db->schema.count);
    uint32_t room = db->seg_blocks * (uint32_t) rpb;

    // Proactive guard: a new segment is a new file. Below the reserve, turn
    // the file set over (evict one, create one) instead of growing. If there
    // is nothing to evict yet, go ahead — the guard is advisory, ENOSPC below
    // is the hard stop.
    if (below_reserve(db)) {
        make_room(db, room, "Free-space reserve breached");
    }

    for (int attempt = 0; attempt < 64; attempt++) {
        esp_err_t ret = tsdb_seg_create(db, ts);
        if (ret != ESP_ERR_NO_MEM) return ret;
        if (!make_room(db, room, "Filesystem full creating a segment")) return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_NO_MEM;
}

// ---------------------------------------------------------------------------
// Append one record (lock held)
// ---------------------------------------------------------------------------

static esp_err_t tsdb_write_locked(tsdb_t *db, uint32_t timestamp, const int16_t *values,
                                   bool defer_flush) {
    esp_err_t ret = ensure_active(db, timestamp);
    if (ret != ESP_OK) return ret;

    tsdb_seg_t *a = &db->segs[db->nsegs - 1];
    uint16_t rpb = a->rpb;
    uint32_t block_num = a->records / rpb;
    uint16_t off = (uint16_t)(a->records % rpb);
    bool new_block = (off == 0);

    // Proactive free-space guard at block boundaries (the file grows here),
    // only until the capacity is latched: after that the active segment's
    // remaining room is already inside the cap, and the file set turns over
    // at segment granularity (checked on creation).
    if (new_block && a->records > 0 && !db->capacity_adapted && below_reserve(db)) {
        uint32_t room = a->blocks_cap * (uint32_t) rpb - a->records;
        make_room(db, room, "Free-space reserve breached");
        a = &db->segs[db->nsegs - 1];               // table may have shifted
    }

    tsdb_block_t temp_block;
    tsdb_block_t *block = write_cache_ptr(db);
    bool cacheable = (block != NULL);
    if (!cacheable) block = &temp_block;
    uint8_t *raw = (uint8_t *) block;

    bool hit = cacheable && db->cached_block_num == block_num;
    if (!hit) {
        if (cacheable && db->cache_dirty) {
            esp_err_t fr = tsdb_write_cache_flush(db);
            if (fr != ESP_OK) return fr;
        }
        if (new_block || tsdb_seg_read_block(db->active, a->data_offset, block_num, block) != ESP_OK ||
            TSDB_BLOCK_MAGIC(raw) != TSDB_BLOCK_MAGIC_VALUE) {
            memset(block, 0, TSDB_BLOCK_SIZE);
            TSDB_BLOCK_MAGIC(raw) = TSDB_BLOCK_MAGIC_VALUE;
            TSDB_BLOCK_COUNT(raw) = 0;
        }
        if (cacheable) db->cached_block_num = block_num;
    }

    TSDB_BLOCK_TS(raw, off) = timestamp;
    for (uint8_t i = 0; i < a->np; i++) {
        TSDB_BLOCK_PARAM(raw, rpb, i, off) = values[i];
    }
    TSDB_BLOCK_COUNT(raw) = off + 1;

    // Flush policy: single writes always; batch writes defer within a block,
    // but a NEW block (the write that grows the file) goes out immediately so
    // ENOSPC is handled here, once, instead of surfacing from a later flush.
    bool flush_now = !defer_flush || !cacheable || new_block;
    if (flush_now) {
        for (int attempt = 0; attempt < 64; attempt++) {
            ret = tsdb_seg_write_block(db->active, a->data_offset, block_num, block);
            if (ret == ESP_OK) break;
            // Never write through a handle that failed (littlefs: an errored
            // file is not safe to use); the reopened one has the last synced
            // state, and the tail block's slot in it is unchanged.
            tsdb_reopen_active(db);
            if (db->active == NULL) break;
            if (ret != ESP_ERR_NO_MEM) break;
            // Filesystem full: free the oldest segment and retry.
            if (db->nsegs <= 1 ||
                !make_room(db, a->blocks_cap * (uint32_t) rpb - a->records,
                           "Filesystem full appending a block")) {
                ret = ESP_ERR_NO_MEM;
                break;
            }
            a = &db->segs[db->nsegs - 1];
        }
        if (ret != ESP_OK) {
            tsdb_write_cache_invalidate(db);
            // Undo the in-RAM record so the block image matches the file.
            TSDB_BLOCK_COUNT(raw) = off;
            return ret;
        }
        if (cacheable) db->cached_block_num = block_num;
        db->cache_dirty = false;
    } else {
        db->cache_dirty = true;
    }

    a->records++;
    a->last_ts = timestamp;
    if (a->records == 1) a->first_ts = timestamp;
    db->total_physical++;
    tsdb_apply_capacity(db);
    tsdb_conv_space_event(db, false);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

// While a pre-3.0 file is being converted (or awaits a retried adoption),
// every write also does one bounded step of it, so the conversion completes
// without the application calling tsdb_maintain(). The step takes the lock
// separately: a reader that queued behind the write gets it first (a mutex
// handed back does not preempt an equal-priority waiter by itself, hence the
// yield).
static void maintenance_after_write(tsdb_t *db) {
    taskYIELD();
    esp_err_t r = tsdb_maintain_h(db, NULL);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Maintenance step after a write: %d (retried at the next one)", r);
    }
}

esp_err_t tsdb_write_h(tsdb_t *db, uint32_t timestamp, const int16_t *values) {
    if (db == NULL || !db->is_open) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (values == NULL) return ESP_ERR_INVALID_ARG;
    if (db->closing) return ESP_ERR_INVALID_STATE;

    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = tsdb_write_locked(db, timestamp, values, false);
    bool maintain = tsdb_conv_pending(db);
    tsdb_unlock(db);
    if (maintain) maintenance_after_write(db);
    return ret;
}

esp_err_t tsdb_write_batch_h(tsdb_t *db, const uint32_t *timestamps,
                             const int16_t **values, uint32_t count) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (timestamps == NULL || values == NULL || count == 0) return ESP_ERR_INVALID_ARG;
    if (db->closing) return ESP_ERR_INVALID_STATE;

    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    uint32_t done = 0;
    // Last point at which everything counted in the active segment was on
    // disk (the write cache clean).
    uint32_t committed_seq = db->nsegs ? db->segs[db->nsegs - 1].seq : UINT32_MAX;
    uint32_t committed_records = db->nsegs ? db->segs[db->nsegs - 1].records : 0;
    uint32_t committed_last_ts = db->nsegs ? db->segs[db->nsegs - 1].last_ts : 0;
    db->flushed_seq = UINT32_MAX;

    for (uint32_t i = 0; i < count; i++) {
        if (values[i] == NULL) { ret = ESP_ERR_INVALID_ARG; break; }
        ret = tsdb_write_locked(db, timestamps[i], values[i], true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Batch: record %lu failed", (unsigned long) i);
            break;
        }
        done++;
        if (!db->cache_dirty) {
            committed_seq = db->segs[db->nsegs - 1].seq;
            committed_records = db->segs[db->nsegs - 1].records;
            committed_last_ts = db->segs[db->nsegs - 1].last_ts;
        }
    }

    if (db->cache_dirty) {
        esp_err_t fr = tsdb_write_cache_flush(db);
        if (ret == ESP_OK) ret = fr;
    }
    // A deferred block can also have been flushed by the failing record
    // itself (moving to a new segment flushes the full one before creating
    // the next, and the creation failed): those records are on disk.
    if (db->flushed_seq == committed_seq && db->flushed_records > committed_records) {
        committed_records = db->flushed_records;
        committed_last_ts = db->flushed_last_ts;
    }
    if (ret != ESP_OK && db->nsegs > 0 && db->segs[db->nsegs - 1].seq == committed_seq &&
        db->segs[db->nsegs - 1].records > committed_records) {
        // A deferred block that failed to flush (here or mid-batch, when the
        // batch moved to the next block) took its records with it: forget
        // them, so the table only counts what reached disk. Records of it
        // that did land are overwritten by the next write.
        tsdb_seg_t *a = &db->segs[db->nsegs - 1];
        uint32_t lost = a->records - committed_records;
        ESP_LOGE(TAG, "Batch: dropping %lu records that did not reach disk", (unsigned long) lost);
        a->records = committed_records;
        a->last_ts = committed_last_ts;
        db->total_physical -= lost;
        tsdb_write_cache_invalidate(db);
        tsdb_apply_capacity(db);
    }

    bool maintain = tsdb_conv_pending(db);
    tsdb_unlock(db);
    if (maintain) maintenance_after_write(db);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Batch write complete: %lu records", (unsigned long) done);
    return ret;
}

esp_err_t tsdb_write(uint32_t timestamp, const int16_t *values) {
    return tsdb_write_h(g_default_handle, timestamp, values);
}

esp_err_t tsdb_write_batch(const uint32_t *timestamps, const int16_t **values, uint32_t count) {
    return tsdb_write_batch_h(g_default_handle, timestamps, values, count);
}
