/**
 * @file tsdb_write.c
 * @brief Write operations with LRU eviction and ring buffer
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "TSDB_WRITE";

// ============================================================================
// BLOCK I/O
// ============================================================================

/**
 * @brief Read a data block from file
 */
esp_err_t tsdb_read_block(tsdb_t *db, uint32_t block_num, tsdb_block_t *block) {
    if (db == NULL || db->file == NULL || block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t block_offset = tsdb_calc_block_offset(&db->header, block_num);

    fseek(db->file, block_offset, SEEK_SET);
    size_t read = fread(block, TSDB_BLOCK_SIZE, 1, db->file);

    if (read != 1) {
        ESP_LOGD(TAG, "Block %lu not found or uninitialized", (unsigned long)block_num);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/**
 * @brief Write a data block to file
 *
 * Returns ESP_ERR_NO_MEM specifically when the filesystem is full (ENOSPC),
 * so the caller can adapt capacity and retry via LRU eviction.
 */
esp_err_t tsdb_write_block(tsdb_t *db, uint32_t block_num, const tsdb_block_t *block) {
    if (db == NULL || db->file == NULL || block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t block_offset = tsdb_calc_block_offset(&db->header, block_num);

    fseek(db->file, block_offset, SEEK_SET);
    errno = 0;
    size_t written = fwrite(block, TSDB_BLOCK_SIZE, 1, db->file);
    int write_errno = errno;
    fflush(db->file);
    fsync(fileno(db->file));

    if (written != 1) {
        if (write_errno == ENOSPC) {
            ESP_LOGW(TAG, "Block %lu write: filesystem full (ENOSPC)",
                     (unsigned long)block_num);
            clearerr(db->file);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGE(TAG, "Failed to write block %lu (errno=%d)",
                 (unsigned long)block_num, write_errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Wrote block %lu at offset %lu",
             (unsigned long)block_num, (unsigned long)block_offset);

    return ESP_OK;
}

// ============================================================================
// WRITE CACHE
// ============================================================================

/**
 * @brief The pool's write-cache block, or NULL when the region spans pages
 *        (paged pool with page_size < 2 KB) — then every write goes through
 *        a stack temp and nothing is cached.
 */
static tsdb_block_t *write_cache_ptr(tsdb_t *db) {
    return (tsdb_block_t *)tsdb_get_buffer_ptr(&db->pool, db->write_cache_offset,
                                               sizeof(tsdb_block_t));
}

esp_err_t tsdb_write_cache_flush(tsdb_t *db) {
    if (db == NULL || !db->cache_dirty) {
        return ESP_OK;
    }
    tsdb_block_t *blk = write_cache_ptr(db);
    if (blk == NULL || db->cached_block_num == UINT32_MAX) {
        db->cache_dirty = false;            // nothing cacheable: cannot happen
        return ESP_OK;
    }
    esp_err_t ret = tsdb_write_block(db, db->cached_block_num, blk);
    db->cache_dirty = false;
    if (ret != ESP_OK) {
        tsdb_write_cache_invalidate(db);
    }
    return ret;
}

// ============================================================================
// WRITE OPERATIONS
// ============================================================================

/**
 * @brief Append one record. Caller holds the handle lock.
 *
 * @param defer_flush false: the block is written to disk before returning
 *        (tsdb_write_h). true: the block stays dirty in the write cache and
 *        is flushed when a later record lands in a different block, or by
 *        tsdb_write_cache_flush (tsdb_write_batch_h). A block that GROWS the
 *        file is always written immediately so ENOSPC surfaces through the
 *        adaptive-capacity path exactly as for single writes.
 * @param dirty_snap Batch mode: receives a copy of the header the moment the
 *        cache turns dirty, so a failed final flush can roll the header back
 *        to the last fully persisted state. NULL in single-record mode.
 *
 * The header is NOT persisted here — the caller writes the sidecar once
 * (per record for tsdb_write_h, per batch for tsdb_write_batch_h).
 */
static esp_err_t tsdb_write_locked(tsdb_t *db, uint32_t timestamp,
                                   const int16_t *values, bool defer_flush,
                                   tsdb_header_t *dirty_snap) {
    // PROACTIVE capacity cap (free-space guard, see esp_tsdb.h): if the file
    // is still in its growth phase and the NEXT record starts a new block —
    // the only point the data file grows — check the filesystem reserve and
    // switch to ring/LRU reuse BEFORE breaching it. Waiting for the hard
    // ENOSPC below means the filesystem is already at zero free, where
    // copy-on-write filesystems (littlefs) fail every OTHER file's writes
    // too (fault ring, settings audit, energy json on the dongle).
    //
    // The cap is persisted by the caller's sidecar write — never by an
    // in-place header rewrite, which on littlefs costs seconds on a large
    // file (see TSDB_SIDECAR_MAGIC) and, at the reserve boundary, could not
    // even be relied on to succeed.
    if (db->free_space_cb && db->min_free_bytes > 0 &&
        !db->capacity_adapted && db->header.max_records > 0 &&
        db->header.total_records > 0 &&
        db->header.total_records < db->header.max_records &&
        (db->header.total_records % db->header.records_per_block) == 0) {
        uint64_t free_now = db->free_space_cb();
        if (free_now < db->min_free_bytes) {
            uint32_t old_max = db->header.max_records;
            db->header.max_records = db->header.total_records;
            db->capacity_adapted = true;
            ESP_LOGW(TAG, "Free space %llu below reserve %lu: capping %lu -> %lu records (LRU from now on)",
                     (unsigned long long)free_now,
                     (unsigned long)db->min_free_bytes,
                     (unsigned long)old_max,
                     (unsigned long)db->header.max_records);
        }
    }

retry_write:
    {}  // label target — allow re-entry after adaptive shrink

    // Calculate record index (ring buffer or unlimited)
    bool unlimited = (db->header.max_records == 0);
    uint32_t record_idx = unlimited ? db->header.total_records :
                          (db->header.total_records % db->header.max_records);

    // Determine if this overwrites old data (LRU eviction) — never in unlimited
    // mode. The bookkeeping (evictions++, oldest_record_idx++) is applied only
    // once the record is in the block, so a failed write leaves the header
    // describing exactly what is on disk.
    bool is_eviction = (!unlimited && db->header.total_records >= db->header.max_records);

    // Calculate block number and offset within block
    uint32_t block_num = record_idx / db->header.records_per_block;
    uint16_t offset_in_block = record_idx % db->header.records_per_block;

    ESP_LOGD(TAG, "Writing record %lu: block=%lu, offset=%d",
             (unsigned long)db->header.total_records,
             (unsigned long)block_num, offset_in_block);

    // Block buffer: the pool's write cache when available, else a stack temp.
    tsdb_block_t temp_block;
    tsdb_block_t *block = write_cache_ptr(db);
    bool cacheable = (block != NULL);
    if (!cacheable) {
        block = &temp_block;
    }

    // Cache hit: the block we need is the one written last time (the common
    // case — records_per_block consecutive records share a block), so the
    // read-back is skipped. On littlefs every read is a skip-list walk, so
    // this removes one of the two block I/Os per steady-state write.
    bool hit = cacheable && (db->cached_block_num == block_num);
    uint8_t *raw_blk = (uint8_t *)block;
    if (!hit) {
        if (cacheable && db->cache_dirty) {
            // Batch mode moving to a new block: persist the previous one. If
            // that fails, the records it held never reached disk — roll the
            // header back to the last persisted state.
            esp_err_t fr = tsdb_write_cache_flush(db);
            if (fr != ESP_OK) {
                if (dirty_snap != NULL) db->header = *dirty_snap;
                return fr;
            }
        }
        esp_err_t rr = tsdb_read_block(db, block_num, block);
        if (rr != ESP_OK || TSDB_BLOCK_MAGIC(raw_blk) != 0x424C4B54) {
            ESP_LOGD(TAG, "Initializing new block %lu", (unsigned long)block_num);
            memset(block, 0, TSDB_BLOCK_SIZE);
            TSDB_BLOCK_MAGIC(raw_blk) = 0x424C4B54;  // "BLKT"
            TSDB_BLOCK_COUNT(raw_blk) = 0;
        }
        if (cacheable) {
            db->cached_block_num = block_num;
        }
    }

    // Write data in columnar format (runtime offsets for correct disk layout)
    uint16_t rpb = db->header.records_per_block;
    TSDB_BLOCK_TS(raw_blk, offset_in_block) = timestamp;
    for (uint8_t i = 0; i < db->header.num_params; i++) {
        TSDB_BLOCK_PARAM(raw_blk, rpb, i, offset_in_block) = values[i];
    }

    // Update block record count
    if (offset_in_block >= TSDB_BLOCK_COUNT(raw_blk)) {
        TSDB_BLOCK_COUNT(raw_blk) = offset_in_block + 1;
    }

    // Flush policy: single writes always; batch writes when the block is not
    // cacheable or when this record opens a NEW block during growth (the only
    // write that can extend the file, hence the only one that can ENOSPC in
    // a way the adaptive cap must see).
    bool grows_file = (offset_in_block == 0 && !is_eviction);
    bool flush_now = !defer_flush || !cacheable || grows_file;
    if (flush_now) {
        esp_err_t ret = tsdb_write_block(db, block_num, block);
        if (ret == ESP_ERR_NO_MEM && !is_eviction && !db->capacity_adapted &&
            db->header.total_records > 0) {
            // Filesystem full while still growing. Cap max_records to what
            // we've already written so future writes reuse existing blocks
            // via LRU instead of allocating new ones. The cap reaches disk
            // through the caller's sidecar write (small separate file, the
            // only kind of write that still has a chance at zero free).
            uint32_t old_max = db->header.max_records;
            db->header.max_records = db->header.total_records;
            db->capacity_adapted = true;
            tsdb_write_cache_invalidate(db);
            ESP_LOGW(TAG, "Adaptive capacity: %lu -> %lu records (LRU from now on)",
                     (unsigned long)old_max,
                     (unsigned long)db->header.max_records);
            goto retry_write;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write block");
            tsdb_write_cache_invalidate(db);
            return ret;
        }
        db->cache_dirty = false;
    } else {
        if (!db->cache_dirty && dirty_snap != NULL) {
            // Clean -> dirty: remember the last fully persisted header state
            // (this record's bookkeeping has not been applied yet).
            *dirty_snap = db->header;
        }
        db->cache_dirty = true;
    }

    // The record is in the block (on disk, or in the cache with a snapshot
    // to fall back on): apply the ring bookkeeping.
    if (is_eviction) {
        db->header.total_evictions++;
        db->header.oldest_record_idx = (db->header.oldest_record_idx + 1) %
                                           db->header.max_records;
        ESP_LOGD(TAG, "LRU eviction: oldest_idx=%lu",
                 (unsigned long)db->header.oldest_record_idx);
    }

    // Write overflow data if active
    if (db->extra_param_count > 0 &&
        db->header.total_records >= db->first_overflow_record_idx) {
        uint32_t overflow_idx = db->header.total_records - db->first_overflow_record_idx;
        uint32_t ovf_offset = db->overflow_data_offset +
                              (overflow_idx * db->overflow_record_size);

        fseek(db->file, ovf_offset, SEEK_SET);
        size_t ovf_written = fwrite(&values[db->header.num_params],
                                     sizeof(int16_t),
                                     db->extra_param_count,
                                     db->file);
        if (ovf_written != db->extra_param_count) {
            ESP_LOGE(TAG, "Failed to write overflow data");
            // Don't fail the whole write -- base data is already written
        }
    }

    // Update header
    db->header.total_records++;
    db->header.total_writes++;
    db->header.newest_record_idx = record_idx;
    db->header.newest_timestamp = timestamp;

    // Update oldest timestamp if needed
    if (is_eviction) {
        uint32_t oldest_block = db->header.oldest_record_idx / db->header.records_per_block;
        uint16_t oldest_offset = db->header.oldest_record_idx % db->header.records_per_block;

        if (oldest_block == block_num) {
            // The new oldest record shares the block we just wrote (true for
            // all but one write per block once the ring has wrapped) — it is
            // already in RAM, so no second block read.
            db->header.oldest_timestamp = TSDB_BLOCK_TS(raw_blk, oldest_offset);
        } else {
            tsdb_block_t *oldest_block_data = (tsdb_block_t*)tsdb_get_buffer_ptr(&db->pool,
                                                                                  db->read_buffer_offset,
                                                                                  sizeof(tsdb_block_t));
            tsdb_block_t temp_oldest;
            if (oldest_block_data == NULL) {
                oldest_block_data = &temp_oldest;
            }
            if (tsdb_read_block(db, oldest_block, oldest_block_data) == ESP_OK) {
                db->header.oldest_timestamp = TSDB_BLOCK_TS((uint8_t *)oldest_block_data, oldest_offset);
            }
        }
    } else if (db->header.total_records == 1) {
        db->header.oldest_timestamp = timestamp;
    }

    // Update sparse index if at stride boundary. Bounds-guarded: after a
    // tsdb_resize() grow, record_idx can exceed the capacity the index was
    // sized for at creation — an unguarded write here would land PAST the
    // preallocated index region, inside the first data blocks.
    if (record_idx % db->header.index_stride == 0 &&
        (record_idx / db->header.index_stride) < db->header.index_entries) {
        uint32_t index_entry_num = record_idx / db->header.index_stride;
        tsdb_index_entry_t entry = {
            .timestamp = timestamp,
            .block_number = block_num
        };

        uint32_t index_file_offset = db->header.index_offset +
                                     (index_entry_num * sizeof(tsdb_index_entry_t));

        fseek(db->file, index_file_offset, SEEK_SET);
        fwrite(&entry, sizeof(tsdb_index_entry_t), 1, db->file);
        fflush(db->file);

        ESP_LOGD(TAG, "Updated index entry %lu: timestamp=%lu, block=%lu",
                 (unsigned long)index_entry_num,
                 (unsigned long)timestamp,
                 (unsigned long)block_num);
    }

    ESP_LOGD(TAG, "Write complete: total_records=%lu, newest_ts=%lu",
             (unsigned long)db->header.total_records,
             (unsigned long)db->header.newest_timestamp);

    return ESP_OK;
}

/**
 * @brief Persist the header to the SIDECAR, not to offset 0 of this file.
 *
 * Rewriting offset 0 costs 3.7-5.1 s because littlefs copy-on-writes
 * everything from the modified offset to EOF. The sidecar is measured at
 * 46-56 ms for the same header on a 268 KB database, and stays durable on
 * every record. close/sync fold the header back in, so a cleanly closed
 * file remains self-contained. See TSDB_SIDECAR_MAGIC.
 */
static void persist_header(tsdb_t *db) {
    tsdb_sidecar_write(db);
    db->in_file_header_stale = true;
}

esp_err_t tsdb_write_h(tsdb_t *db, uint32_t timestamp, const int16_t *values) {
    if (db == NULL || !db->is_open) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (values == NULL) {
        ESP_LOGE(TAG, "NULL values pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Fail fast during a schema migration or close instead of stalling on the
    // mutex for the full lock timeout. Caller may retry on its next cadence.
    if (db->migrating || db->closing) {
        return ESP_ERR_INVALID_STATE;
    }

    // Take the handle lock for the whole write. The mutex is recursive, so a
    // task already holding it (e.g. during a query) can still write.
    // OPEN variant: a closer may have won the mutex while we were blocked —
    // the handle is parked (not freed) in that case, so we must re-check.
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);

    bool was_adapted = db->capacity_adapted;
    esp_err_t ret = tsdb_write_locked(db, timestamp, values, false, NULL);

    // Persist on success — and on failure too if a capacity cap was latched
    // along the way, so a stale on-disk max_records does not resume growth
    // into a full filesystem on the next boot.
    if (ret == ESP_OK || db->capacity_adapted != was_adapted) {
        persist_header(db);
    }

    tsdb_unlock(db);
    return ret;
}

esp_err_t tsdb_write_batch_h(tsdb_t *db,
                              const uint32_t *timestamps,
                              const int16_t **values,
                              uint32_t count) {
    if (db == NULL || !db->is_open) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timestamps == NULL || values == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (db->migrating || db->closing) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Batch writing %lu records", (unsigned long)count);

    // One lock, one sidecar write, and one block write per block touched.
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);

    tsdb_header_t snap = db->header;        // last persisted header state
    esp_err_t ret = ESP_OK;
    uint32_t done = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (values[i] == NULL) {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        ret = tsdb_write_locked(db, timestamps[i], values[i], true, &snap);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write record %lu in batch", (unsigned long)i);
            break;
        }
        done++;
    }

    // Flush whatever is still dirty. If THAT fails, the records in the
    // unflushed block never reached disk: roll the header back to the last
    // state that did, so the next writes simply redo those slots.
    if (db->cache_dirty) {
        esp_err_t fr = tsdb_write_cache_flush(db);
        if (fr != ESP_OK) {
            ESP_LOGE(TAG, "Batch final flush failed — rolling header back");
            db->header = snap;
            if (ret == ESP_OK) ret = fr;
        }
    }
    fflush(db->file);
    fsync(fileno(db->file));
    persist_header(db);

    tsdb_unlock(db);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Batch write complete: %lu records", (unsigned long)done);
    }
    return ret;
}

// ============================================================================
// LEGACY GLOBAL API
// ============================================================================

esp_err_t tsdb_write(uint32_t timestamp, const int16_t *values) {
    return tsdb_write_h(g_default_handle, timestamp, values);
}

esp_err_t tsdb_write_batch(const uint32_t *timestamps,
                            const int16_t **values,
                            uint32_t count) {
    return tsdb_write_batch_h(g_default_handle, timestamps, values, count);
}
