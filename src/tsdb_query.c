/**
 * @file tsdb_query.c
 * @brief Query and aggregation operations with streaming
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "TSDB_QUERY";

// ============================================================================
// RING GEOMETRY HELPERS
// ============================================================================

/**
 * @brief Physical ring slot of logical position k (0 = oldest retained).
 */
static inline uint32_t ring_slot(const tsdb_query_t *q, uint32_t k) {
    return (q->header.max_records == 0) ? k :
           ((q->header.oldest_record_idx + k) % q->header.max_records);
}

/**
 * @brief Point the iterator cursor at logical position k.
 */
static void cursor_set(tsdb_query_t *q, uint32_t k) {
    uint16_t rpb = q->header.records_per_block;
    uint32_t slot = ring_slot(q, k);
    uint32_t blk = rpb ? (slot / rpb) : 0;
    if (blk != q->current_block_num) {
        q->block_loaded = false;
    }
    q->records_scanned = k;
    q->current_record_idx = slot;
    q->current_block_num = blk;
    q->offset_in_block = rpb ? (slot % rpb) : 0;
}

/**
 * @brief Binary-search the ring for the first retained record with
 *        ts >= target, in logical (time-ascending) order.
 *
 * The ring walk from oldest_record_idx is time-ascending by construction, so
 * logical positions are binary-searchable even after the ring wraps (unlike
 * the sparse index, whose entries are slot-ordered post-wrap). O(log n) block
 * reads: on a year-full database a "last 24h" window costs ~17 reads instead
 * of a ~3400-block scan.
 *
 * Relies on the same timestamp monotonicity the iterator's early-exit
 * (ts > end_time) already assumes.
 *
 * @param pos Out: logical position in [0, available] of the first record with
 *            ts >= target (== available when every record is older).
 * @return ESP_OK, or ESP_FAIL on any probe anomaly (unreadable block, zero
 *         timestamp) — the caller must fall back to a linear walk.
 */
static esp_err_t tsdb_seek_pos(tsdb_query_t *q, uint32_t available,
                               uint32_t target, uint32_t *pos) {
    uint16_t rpb = q->header.records_per_block;
    uint32_t lo = 0, hi = available;   // invariant: ts(lo-1) < target <= ts(hi)

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t slot = ring_slot(q, mid);

        if (q->current_block_num != slot / rpb || !q->block_loaded) {
            if (tsdb_read_block(q->db, slot / rpb, q->block_buffer) != ESP_OK) {
                q->block_loaded = false;
                return ESP_FAIL;   // truncation/corruption
            }
            q->current_block_num = slot / rpb;
            q->block_loaded = true;
        }
        uint32_t ts = TSDB_BLOCK_TS((uint8_t *)q->block_buffer, slot % rpb);
        if (ts == 0) {
            return ESP_FAIL;       // unexpected hole
        }

        if (ts < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *pos = lo;
    return ESP_OK;
}

/**
 * @brief Logical position one past the last record with ts <= end_time.
 *        Returns ESP_FAIL on probe anomaly (see tsdb_seek_pos).
 */
static esp_err_t tsdb_seek_end(tsdb_query_t *q, uint32_t available,
                               uint32_t end_time, uint32_t *pos) {
    if (end_time == UINT32_MAX) {
        *pos = available;
        return ESP_OK;
    }
    return tsdb_seek_pos(q, available, end_time + 1, pos);
}

// ============================================================================
// QUERY OPERATIONS
// ============================================================================

esp_err_t tsdb_query_init_h(tsdb_t *db,
                            tsdb_query_t *query,
                            uint32_t start_time,
                            uint32_t end_time,
                            const uint8_t *param_indices,
                            uint8_t num_params_to_fetch) {
    if (db == NULL || !db->is_open || query == NULL) {
        ESP_LOGE(TAG, "Invalid state or NULL query");
        return ESP_ERR_INVALID_STATE;
    }

    // Fail fast during a schema migration or close instead of stalling on
    // the mutex.
    if (db->migrating || db->closing) {
        return ESP_ERR_INVALID_STATE;
    }

    // Take the handle lock for the entire query lifecycle. Released by
    // tsdb_query_close(). CRITICAL: callers MUST call tsdb_query_close()
    // even on early-exit / error paths or the lock leaks. The mutex is
    // recursive so the same task can also do writes.
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);

    if (start_time > end_time) {
        ESP_LOGE(TAG, "Invalid time range: start > end");
        tsdb_unlock(db);
        return ESP_ERR_INVALID_ARG;
    }

    memset(query, 0, sizeof(tsdb_query_t));

    // Bind handle to the query iterator. tsdb_query_next() / _close() use
    // this to access the underlying file + overflow state without a separate
    // handle parameter (preserves the v2 API shape).
    query->db = db;

    // Copy header
    memcpy(&query->header, &db->header, sizeof(tsdb_header_t));

    // Set query parameters
    query->start_time = start_time;
    query->end_time = end_time;
    query->file = db->file;
    query->stride = 1;

    // Setup parameter indices
    if (param_indices == NULL) {
        // Fetch all parameters (base + extra)
        uint8_t total = query->header.num_params + db->extra_param_count;
        query->num_params_to_fetch = total;
        for (uint8_t i = 0; i < total; i++) {
            query->param_indices[i] = i;
        }
    } else {
        query->num_params_to_fetch = num_params_to_fetch;
        if (num_params_to_fetch > TSDB_MAX_PARAMS) {
            ESP_LOGE(TAG, "Too many parameters requested: %d", num_params_to_fetch);
            tsdb_unlock(db);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(query->param_indices, param_indices, num_params_to_fetch);
    }

    // Does any requested column live in the legacy overflow region? Decided
    // once here so the per-record path can skip the extras read entirely.
    query->needs_extras = false;
    for (uint8_t i = 0; i < query->num_params_to_fetch; i++) {
        if (query->param_indices[i] >= query->header.num_params) {
            query->needs_extras = true;
            break;
        }
    }

    // Try to use query buffer from pool
    query->block_buffer = (tsdb_block_t*)tsdb_get_buffer_ptr(&db->pool,
                                                              db->query_buffer_offset,
                                                              sizeof(tsdb_block_t));

    if (query->block_buffer == NULL) {
        // Paged mode and spans pages - allocate separately
        query->block_buffer = heap_caps_malloc(sizeof(tsdb_block_t), MALLOC_CAP_8BIT);
        if (query->block_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate query block buffer");
            tsdb_unlock(db);
            return ESP_ERR_NO_MEM;
        }
        query->owns_buffer = true;
        ESP_LOGD(TAG, "Allocated separate query buffer at %p", query->block_buffer);
    } else {
        query->owns_buffer = false;
        ESP_LOGD(TAG, "Using query buffer from pool at %p", query->block_buffer);
    }

    // Walk the ring in logical (time-ascending) order. Records are written at
    // slot = total_records % max_records, and on eviction oldest_record_idx
    // advances, so reading slots oldest_record_idx, +1, +2, ... (mod
    // max_records) yields strictly ascending timestamps.
    //
    // We do NOT use the sparse index to seek to start_time: once the ring has
    // wrapped, index entries are ordered by physical slot (the slot they were
    // last written at), not by timestamp, so a binary search on them is
    // invalid and lands in the wrong place. A logical walk with an early-exit
    // once ts > end_time is correct for every range and is bounded by
    // `available` record reads; the binary seek below then jumps the walk's
    // starting point to the window in O(log n) block reads.
    bool unlimited = (query->header.max_records == 0);
    uint32_t available_records = unlimited ? query->header.total_records :
                                 ((query->header.total_records < query->header.max_records) ?
                                  query->header.total_records : query->header.max_records);

    uint16_t rpb = query->header.records_per_block;
    query->end_record_idx = available_records;     // total records to scan
    query->current_block_num = UINT32_MAX;         // nothing loaded yet
    query->block_loaded = false;
    cursor_set(query, 0);

    // Whole-window early-outs: the header tracks the retained time bounds, so
    // a window entirely outside them (a chart asking for "the future", or a
    // range the ring has already evicted) costs zero block reads.
    if (available_records > 0 && query->header.newest_timestamp > 0 &&
        (start_time > query->header.newest_timestamp ||
         end_time < query->header.oldest_timestamp)) {
        query->end_record_idx = 0;
        ESP_LOGD(TAG, "Query window [%lu, %lu] outside retained [%lu, %lu]",
                 (unsigned long)start_time, (unsigned long)end_time,
                 (unsigned long)query->header.oldest_timestamp,
                 (unsigned long)query->header.newest_timestamp);
        return ESP_OK;
    }

    // O(log n) start seek: jump the cursor to the first record with
    // ts >= start_time instead of linearly scanning from the oldest.
    // (records_scanned advances by the skipped count so query_next's
    // absolute-record-index math for overflow lookups stays correct.)
    if (available_records > 0 && rpb > 0 &&
        start_time > query->header.oldest_timestamp) {
        uint32_t k = 0;
        if (tsdb_seek_pos(query, available_records, start_time, &k) != ESP_OK) {
            k = 0;      // probe anomaly: plain full walk from the oldest
        }
        cursor_set(query, k);
    }

    ESP_LOGD(TAG, "Query init: time=[%lu, %lu], params=%d, scan %lu records from pos %lu",
             (unsigned long)start_time, (unsigned long)end_time,
             query->num_params_to_fetch,
             (unsigned long)available_records, (unsigned long)query->records_scanned);

    return ESP_OK;
}

esp_err_t tsdb_query_set_stride(tsdb_query_t *query, uint32_t stride) {
    if (query == NULL || query->db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    query->stride = stride == 0 ? 1 : stride;
    return ESP_OK;
}

/**
 * @brief Read the extras row (legacy overflow region) for one absolute record
 *        index. One seek + one read per record instead of one per column.
 */
static void read_extras_row(tsdb_query_t *query, uint32_t abs_record_idx,
                            int16_t *extras) {
    tsdb_t *db = query->db;
    if (db->extra_param_count == 0 ||
        abs_record_idx < db->first_overflow_record_idx) {
        memset(extras, 0, db->extra_param_count * sizeof(int16_t));
        return;
    }
    uint32_t overflow_idx = abs_record_idx - db->first_overflow_record_idx;
    uint32_t ovf_offset = db->overflow_data_offset +
                          (overflow_idx * db->overflow_record_size);
    fseek(query->file, ovf_offset, SEEK_SET);
    if (fread(extras, sizeof(int16_t), db->extra_param_count, query->file)
            != db->extra_param_count) {
        memset(extras, 0, db->extra_param_count * sizeof(int16_t));
    }
}

/**
 * @brief Copy the requested columns of one record out of a loaded block.
 */
static void emit_record(tsdb_query_t *query, const uint8_t *raw, uint16_t off,
                        uint32_t abs_record_idx, int16_t *values) {
    uint16_t rpb = query->header.records_per_block;
    uint8_t np = query->header.num_params;
    int16_t extras[TSDB_MAX_EXTRA_PARAMS];
    if (query->needs_extras) {
        read_extras_row(query, abs_record_idx, extras);
    }
    for (uint8_t i = 0; i < query->num_params_to_fetch; i++) {
        uint8_t param_idx = query->param_indices[i];
        if (param_idx < np) {
            values[i] = TSDB_BLOCK_PARAM(raw, rpb, param_idx, off);
        } else if (query->needs_extras &&
                   param_idx < (np + query->db->extra_param_count)) {
            values[i] = extras[param_idx - np];
        } else {
            values[i] = 0;  // invalid index
        }
    }
}

esp_err_t tsdb_query_next(tsdb_query_t *query,
                          uint32_t *timestamp,
                          int16_t *values) {
    if (query == NULL || query->db == NULL || timestamp == NULL || values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t available = query->end_record_idx;   // total records to scan (fixed)

    // Walk the ring one slot at a time in logical (time-ascending) order,
    // wrapping the physical slot/block modulo max_records. Stop once we've
    // visited every retained record or passed end_time.
    while (query->records_scanned < available) {
        if (!query->block_loaded) {
            if (tsdb_read_block(query->db, query->current_block_num,
                                query->block_buffer) != ESP_OK) {
                // Every slot inside the ring has a backing block; a read
                // failure here means truncation/corruption -- stop cleanly.
                ESP_LOGD(TAG, "Block %lu unreadable mid-ring",
                         (unsigned long)query->current_block_num);
                return ESP_ERR_NOT_FOUND;
            }
            query->block_loaded = true;
        }

        uint8_t *qraw = (uint8_t *)query->block_buffer;
        uint16_t cur_off = query->offset_in_block;
        uint32_t ts = TSDB_BLOCK_TS(qraw, cur_off);

        // Absolute record index (in total_records space) of the slot we are
        // about to read; the k-th oldest retained record maps to
        // (total_records - available) + k. Needed for overflow lookups.
        uint32_t abs_record_idx = (query->header.total_records - available) +
                                  query->records_scanned;

        if (ts == 0) {
            cursor_set(query, query->records_scanned + 1);   // uninitialized slot
            continue;
        }
        if (ts > query->end_time) {
            query->records_scanned = available;              // ascending -> done
            return ESP_ERR_NOT_FOUND;
        }
        if (ts < query->start_time) {
            cursor_set(query, query->records_scanned + 1);   // before the window
            continue;
        }

        // In range -> emit this record, then advance by the stride.
        *timestamp = ts;
        emit_record(query, qraw, cur_off, abs_record_idx, values);

        uint32_t next = query->records_scanned + query->stride;
        if (next < query->records_scanned || next > available) next = available;
        cursor_set(query, next);

        ESP_LOGD(TAG, "Found record: ts=%lu", (unsigned long)ts);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

void tsdb_query_close(tsdb_query_t *query) {
    if (query == NULL) {
        return;
    }

    // Capture the handle before we wipe the struct -- tsdb_unlock() needs it.
    tsdb_t *db = query->db;

    // Free buffer if we allocated it separately
    if (query->owns_buffer && query->block_buffer != NULL) {
        free(query->block_buffer);
        query->block_buffer = NULL;
        ESP_LOGD(TAG, "Freed separate query buffer");
    }

    memset(query, 0, sizeof(tsdb_query_t));

    // Release the handle lock taken by tsdb_query_init_h(). The mutex is
    // recursive so the same task can also do writes.
    if (db != NULL) {
        tsdb_unlock(db);
    }
}

/**
 * @brief Window bounds as logical positions: [*first, *last_plus_one).
 *        O(log n) via two binary seeks. ESP_FAIL if a probe hit an anomaly
 *        (caller falls back to scanning).
 */
static esp_err_t window_positions(tsdb_query_t *query, uint32_t *first,
                                  uint32_t *last_plus_one) {
    uint32_t available = query->end_record_idx;
    if (available == 0) {
        *first = *last_plus_one = 0;
        return ESP_OK;
    }
    uint32_t a = 0, b = 0;
    if (query->start_time > query->header.oldest_timestamp) {
        if (tsdb_seek_pos(query, available, query->start_time, &a) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (tsdb_seek_end(query, available, query->end_time, &b) != ESP_OK) {
        return ESP_FAIL;
    }
    if (b < a) b = a;
    *first = a;
    *last_plus_one = b;
    return ESP_OK;
}

esp_err_t tsdb_query_count_h(tsdb_t *db,
                             uint32_t start_time,
                             uint32_t end_time,
                             uint32_t *count) {
    if (db == NULL || !db->is_open || count == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *count = 0;

    // Count without iterating: two O(log n) seeks bound the window. Requires
    // no zero-timestamp holes among the retained records (the seek reports
    // one as an anomaly, in which case we fall back to the walk).
    tsdb_query_t query;
    uint8_t one[1] = {0};
    esp_err_t ret = tsdb_query_init_h(db, &query, start_time, end_time, one, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t a, b;
    if (window_positions(&query, &a, &b) == ESP_OK) {
        *count = b - a;
    } else {
        uint32_t ts;
        int16_t value;
        while (tsdb_query_next(&query, &ts, &value) == ESP_OK) {
            (*count)++;
        }
    }

    tsdb_query_close(&query);

    ESP_LOGI(TAG, "Counted %lu records in range [%lu, %lu]",
             (unsigned long)*count, (unsigned long)start_time, (unsigned long)end_time);

    return ESP_OK;
}

// ============================================================================
// AGGREGATION OPERATIONS
// ============================================================================

/**
 * @brief Per-column accumulator shared by the single and multi paths.
 */
typedef struct {
    int64_t sum;
    int32_t min_val;
    int32_t max_val;
    int16_t first_val;
    int16_t last_val;
} agg_acc_t;

static inline void acc_init(agg_acc_t *a) {
    a->sum = 0;
    a->min_val = INT32_MAX;
    a->max_val = INT32_MIN;
    a->first_val = 0;
    a->last_val = 0;
}

static inline void acc_push(agg_acc_t *a, int16_t v, bool first) {
    if (first) a->first_val = v;
    a->last_val = v;
    a->sum += v;
    if (v < a->min_val) a->min_val = v;
    if (v > a->max_val) a->max_val = v;
}

static int32_t acc_result(const agg_acc_t *a, tsdb_agg_type_t t, uint32_t count) {
    switch (t) {
        case TSDB_AGG_SUM:   return (int32_t)a->sum;
        case TSDB_AGG_AVG:   return (int32_t)(a->sum / (int64_t)count);
        case TSDB_AGG_MIN:   return a->min_val;
        case TSDB_AGG_MAX:   return a->max_val;
        case TSDB_AGG_COUNT: return (int32_t)count;
        case TSDB_AGG_FIRST: return a->first_val;
        case TSDB_AGG_LAST:  return a->last_val;
        default:             return 0;
    }
}

/**
 * @brief Block-level aggregation scan over the query window.
 *
 * Instead of pulling every record through tsdb_query_next (a function call,
 * cursor update and column gather per record), this walks each loaded block
 * directly: one bounds check per record and one 16-bit load per accumulated
 * column. Columns are base-only — legacy overflow extras still go through
 * the per-record path (they live in a separate region of the file).
 *
 * @param cols Base column indices to accumulate; acc[i] receives cols[i].
 * @return Number of in-window records.
 */
static uint32_t agg_scan_blocks(tsdb_query_t *q, const uint8_t *cols,
                                uint8_t ncols, agg_acc_t *acc) {
    uint32_t available = q->end_record_idx;
    uint16_t rpb = q->header.records_per_block;
    bool unlimited = (q->header.max_records == 0);
    uint32_t count = 0;

    while (q->records_scanned < available) {
        if (!q->block_loaded) {
            if (tsdb_read_block(q->db, q->current_block_num, q->block_buffer) != ESP_OK) {
                break;
            }
            q->block_loaded = true;
        }
        const uint8_t *raw = (const uint8_t *)q->block_buffer;

        // Run of consecutive slots inside this block: bounded by the block
        // end, the retained-record count, and (ring mode) the ring end.
        uint32_t run = rpb - q->offset_in_block;
        uint32_t left = available - q->records_scanned;
        if (run > left) run = left;
        if (!unlimited) {
            uint32_t to_ring_end = q->header.max_records - q->current_record_idx;
            if (run > to_ring_end) run = to_ring_end;
        }

        uint16_t off = q->offset_in_block;
        for (uint32_t i = 0; i < run; i++, off++) {
            uint32_t ts = TSDB_BLOCK_TS(raw, off);
            if (ts == 0 || ts < q->start_time) continue;
            if (ts > q->end_time) {
                q->records_scanned = available;        // ascending -> done
                return count;
            }
            for (uint8_t c = 0; c < ncols; c++) {
                acc_push(&acc[c], TSDB_BLOCK_PARAM(raw, rpb, cols[c], off), count == 0);
            }
            count++;
        }
        cursor_set(q, q->records_scanned + run);
    }
    return count;
}

/**
 * @brief Per-record aggregation path (needed when overflow extras are
 *        requested). Same results as agg_scan_blocks.
 */
static uint32_t agg_scan_records(tsdb_query_t *q, uint8_t ncols, agg_acc_t *acc) {
    uint32_t ts;
    int16_t values[TSDB_MAX_PARAMS];
    uint32_t count = 0;
    while (tsdb_query_next(q, &ts, values) == ESP_OK) {
        for (uint8_t c = 0; c < ncols; c++) {
            acc_push(&acc[c], values[c], count == 0);
        }
        count++;
    }
    return count;
}

esp_err_t tsdb_aggregate_h(tsdb_t *db,
                           uint32_t start_time,
                           uint32_t end_time,
                           uint8_t param_index,
                           tsdb_agg_type_t agg_type,
                           int32_t *result) {
    if (db == NULL || !db->is_open || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (param_index >= (db->header.num_params + db->extra_param_count)) {
        ESP_LOGE(TAG, "Invalid parameter index: %d", param_index);
        return ESP_ERR_INVALID_ARG;
    }
    if (agg_type < TSDB_AGG_SUM || agg_type > TSDB_AGG_LAST) {
        ESP_LOGE(TAG, "Unknown aggregation type: %d", agg_type);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t params[] = {param_index};
    tsdb_query_t query;
    esp_err_t ret = tsdb_query_init_h(db, &query, start_time, end_time, params, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    agg_acc_t acc;
    acc_init(&acc);
    uint32_t count = 0;

    // COUNT / FIRST / LAST need no scan: two binary seeks bound the window,
    // then at most one record is read. O(log n) regardless of window size.
    if (agg_type == TSDB_AGG_COUNT || agg_type == TSDB_AGG_FIRST ||
        agg_type == TSDB_AGG_LAST) {
        uint32_t a, b;
        if (window_positions(&query, &a, &b) == ESP_OK) {
            count = b - a;
            if (count > 0 && agg_type != TSDB_AGG_COUNT) {
                uint32_t ts;
                int16_t v;
                cursor_set(&query, agg_type == TSDB_AGG_FIRST ? a : b - 1);
                if (tsdb_query_next(&query, &ts, &v) == ESP_OK) {
                    acc_push(&acc, v, true);
                } else {
                    count = 0;      // should not happen; report empty
                }
            }
            goto done;
        }
        // Probe anomaly: fall through to the scan.
    }

    if (query.needs_extras) {
        count = agg_scan_records(&query, 1, &acc);
    } else {
        count = agg_scan_blocks(&query, params, 1, &acc);
    }

done:
    tsdb_query_close(&query);

    if (count == 0) {
        *result = 0;
        ESP_LOGW(TAG, "No records found in range");
        return ESP_OK;
    }
    *result = acc_result(&acc, agg_type, count);

    ESP_LOGI(TAG, "Aggregation complete: type=%d, result=%ld, count=%lu",
             agg_type, (long)*result, (unsigned long)count);

    return ESP_OK;
}

esp_err_t tsdb_aggregate_multi_h(tsdb_t *db,
                                  uint32_t start_time,
                                  uint32_t end_time,
                                  tsdb_agg_request_t *requests,
                                  uint8_t num_requests,
                                  uint32_t *record_count) {
    if (db == NULL || !db->is_open || requests == NULL || num_requests == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (num_requests > TSDB_MAX_PARAMS) {
        ESP_LOGE(TAG, "Too many aggregation requests: %d (max %d)",
                 num_requests, TSDB_MAX_PARAMS);
        return ESP_ERR_INVALID_ARG;
    }

    // Collect unique param indices needed
    uint8_t unique_params[TSDB_MAX_PARAMS];
    uint8_t num_unique = 0;
    uint8_t req_to_query_idx[TSDB_MAX_PARAMS];
    for (uint8_t i = 0; i < num_requests; i++) {
        if (requests[i].param_index >= (db->header.num_params + db->extra_param_count)) {
            ESP_LOGE(TAG, "Invalid parameter index: %d", requests[i].param_index);
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t j;
        for (j = 0; j < num_unique; j++) {
            if (unique_params[j] == requests[i].param_index) break;
        }
        if (j == num_unique) {
            unique_params[num_unique++] = requests[i].param_index;
        }
        req_to_query_idx[i] = j;
    }

    // Open single query for all needed params
    tsdb_query_t query;
    esp_err_t ret = tsdb_query_init_h(db, &query, start_time, end_time, unique_params, num_unique);
    if (ret != ESP_OK) {
        return ret;
    }

    // One accumulator per UNIQUE column; requests map onto them.
    agg_acc_t acc[TSDB_MAX_PARAMS];
    for (uint8_t i = 0; i < num_unique; i++) {
        acc_init(&acc[i]);
    }

    uint32_t count;
    if (query.needs_extras) {
        count = agg_scan_records(&query, num_unique, acc);
    } else {
        count = agg_scan_blocks(&query, unique_params, num_unique, acc);
    }

    tsdb_query_close(&query);

    // Compute results
    for (uint8_t i = 0; i < num_requests; i++) {
        requests[i].result = (count == 0) ? 0 :
            acc_result(&acc[req_to_query_idx[i]], requests[i].agg_type, count);
    }

    if (record_count) {
        *record_count = count;
    }

    ESP_LOGI(TAG, "Multi-aggregate complete: %d requests, %lu records scanned",
             num_requests, (unsigned long)count);

    return ESP_OK;
}

// ============================================================================
// LEGACY GLOBAL API
// ============================================================================

esp_err_t tsdb_query_init(tsdb_query_t *query,
                          uint32_t start_time,
                          uint32_t end_time,
                          const uint8_t *param_indices,
                          uint8_t num_params_to_fetch) {
    return tsdb_query_init_h(g_default_handle, query, start_time, end_time,
                             param_indices, num_params_to_fetch);
}

esp_err_t tsdb_query_count(uint32_t start_time,
                           uint32_t end_time,
                           uint32_t *count) {
    return tsdb_query_count_h(g_default_handle, start_time, end_time, count);
}

esp_err_t tsdb_aggregate(uint32_t start_time,
                         uint32_t end_time,
                         uint8_t param_index,
                         tsdb_agg_type_t agg_type,
                         int32_t *result) {
    return tsdb_aggregate_h(g_default_handle, start_time, end_time,
                            param_index, agg_type, result);
}

esp_err_t tsdb_aggregate_multi(uint32_t start_time,
                                uint32_t end_time,
                                tsdb_agg_request_t *requests,
                                uint8_t num_requests,
                                uint32_t *record_count) {
    return tsdb_aggregate_multi_h(g_default_handle, start_time, end_time,
                                   requests, num_requests, record_count);
}
