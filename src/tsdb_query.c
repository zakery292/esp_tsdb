/**
 * @file tsdb_query.c
 * @brief Queries and aggregations across segments.
 *
 * Records are addressed by LOGICAL position: 0 = oldest retained record,
 * total-1 = newest, spanning every segment in time order (the first
 * `head_skip` physical records of the oldest segment are invisible). Each
 * segment covers [seg_start, seg_start + records) of that space.
 *
 * Within a segment, blocks are time-ordered, so a window seek is a binary
 * search over the in-RAM segment table (by last timestamp) followed by a
 * binary search over that segment's records: O(log n) block reads.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

static const char *TAG = "TSDB_QUERY";

// ============================================================================
// SEGMENT CURSOR
// ============================================================================

static int64_t seg_logical_start(const tsdb_t *db, int i) {
    int64_t start = -(int64_t) db->head_skip;
    for (int j = 0; j < i; j++) start += db->segs[j].records;
    return start;
}

static int seg_for_pos(const tsdb_t *db, uint32_t pos) {
    int64_t start = -(int64_t) db->head_skip;
    for (int i = 0; i < db->nsegs; i++) {
        int64_t end = start + db->segs[i].records;
        if ((int64_t) pos < end) return i;
        start = end;
    }
    return -1;
}

static void q_leave(tsdb_query_t *q) {
    if (q->owns_file && q->seg_file) fclose(q->seg_file);
    q->seg_file = NULL;
    q->owns_file = false;
    q->seg_i = -1;
    q->block_loaded = false;
    q->cur_block = UINT32_MAX;
}

static esp_err_t q_enter(tsdb_query_t *q, int i) {
    tsdb_t *db = q->db;
    if (q->seg_i == i) return ESP_OK;
    q_leave(q);
    const tsdb_seg_t *seg = &db->segs[i];

    q->seg_i = i;
    q->seg_start = seg_logical_start(db, i);
    q->seg_records = seg->records;
    q->seg_rpb = seg->rpb;
    q->seg_np = seg->np;
    q->seg_data_offset = seg->data_offset;
    q->seg_legacy = (seg->kind == TSDB_SEG_LEGACY);
    q->seg_rank = seg->rank;
    q->needs_extras = false;

    // File handle: the legacy file and the active segment are read through
    // the handle's own FILE* (littlefs forbids a second handle on a file that
    // is open for writing); frozen and converted segments are opened
    // read-only. Nothing closes or truncates these while the query is open:
    // it holds the lock, and its own task's writes and maintenance steps are
    // refused until it closes.
    if (q->seg_legacy) {
        // Closed only if the reopen after a cut failed: try again here.
        if (db->legacy && db->legacy->file == NULL) db->legacy->file = fopen(db->legacy->path, "rb");
        q->seg_file = db->legacy ? db->legacy->file : NULL;
    } else if (i == db->nsegs - 1 && seg->kind == TSDB_SEG_NORMAL && db->active != NULL) {
        q->seg_file = db->active;
    } else {
        char path[160];
        tsdb_entry_path(db, seg, path, sizeof(path));
        q->seg_file = fopen(path, "rb");
        q->owns_file = (q->seg_file != NULL);
    }
    if (q->seg_file == NULL) {
        ESP_LOGE(TAG, "Cannot open segment %lu", (unsigned long) seg->seq);
        q->seg_i = -1;
        return ESP_FAIL;
    }

    // Column map: identity when the segment has the current schema.
    q->seg_identity = (seg->kind == TSDB_SEG_NORMAL && seg->schema_gen == db->schema.gen &&
                       seg->np == db->schema.count);
    if (q->seg_identity) {
        for (uint8_t r = 0; r < q->num_params_to_fetch; r++) {
            uint8_t c = q->param_indices[r];
            q->col_map[r] = (c < seg->np) ? (int8_t) c : -1;
        }
        return ESP_OK;
    }

    char (*names)[TSDB_NAME_LEN] = calloc(seg->np, TSDB_NAME_LEN);
    int8_t *full = malloc(TSDB_MAX_PARAMS);
    if (names == NULL || full == NULL) {
        free(names); free(full);
        q_leave(q);
        return ESP_ERR_NO_MEM;
    }
    if (q->seg_legacy) {
        for (uint8_t j = 0; j < seg->np; j++) tsdb_legacy_column_name(db->legacy, j, names[j]);
    } else if (tsdb_seg_read_schema(db, seg, q->seg_file, names, NULL, NULL) != ESP_OK) {
        free(names); free(full);
        q_leave(q);
        return ESP_FAIL;
    }
    tsdb_map_columns(&db->schema, seg->np, names, full);
    for (uint8_t r = 0; r < q->num_params_to_fetch; r++) {
        uint8_t c = q->param_indices[r];
        q->col_map[r] = (c < db->schema.count) ? full[c] : -1;
        if (q->seg_legacy && q->col_map[r] >= (int8_t) db->legacy->header.num_params) {
            q->needs_extras = true;
        }
    }
    free(names); free(full);
    return ESP_OK;
}

static esp_err_t q_enter_pos(tsdb_query_t *q, uint32_t pos) {
    if (q->seg_i >= 0 && (int64_t) pos >= q->seg_start &&
        (int64_t) pos < q->seg_start + q->seg_records) {
        return ESP_OK;
    }
    int i = seg_for_pos(q->db, pos);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    return q_enter(q, i);
}

// Locate physical record `phys` of the current segment: block + offset.
static inline void q_locate(const tsdb_query_t *q, uint32_t phys, uint32_t *block, uint16_t *off) {
    if (q->seg_legacy) {
        tsdb_legacy_locate(q->db->legacy, q->seg_rank + phys, block, off);
    } else {
        *block = phys / q->seg_rpb;
        *off = (uint16_t)(phys % q->seg_rpb);
    }
}

static esp_err_t q_load(tsdb_query_t *q, uint32_t block) {
    if (q->block_loaded && q->cur_block == block) return ESP_OK;
    esp_err_t ret;
    if (q->seg_legacy) {
        ret = tsdb_legacy_read_block(q->db->legacy, block, q->block_buffer);
    } else {
        ret = tsdb_seg_read_block(q->seg_file, q->seg_data_offset, block, q->block_buffer);
    }
    q->block_loaded = (ret == ESP_OK);
    q->cur_block = ret == ESP_OK ? block : UINT32_MAX;
    return ret;
}

// Timestamp at logical position pos (block read as needed).
static esp_err_t q_ts_at(tsdb_query_t *q, uint32_t pos, uint32_t *ts) {
    esp_err_t ret = q_enter_pos(q, pos);
    if (ret != ESP_OK) return ret;
    uint32_t block; uint16_t off;
    q_locate(q, (uint32_t)(pos - q->seg_start), &block, &off);
    ret = q_load(q, block);
    if (ret != ESP_OK) return ret;
    *ts = TSDB_BLOCK_TS((uint8_t *) q->block_buffer, off);
    return ESP_OK;
}

// Longest run of consecutive logical positions from pos that share a block.
static uint32_t q_run(const tsdb_query_t *q, uint32_t pos) {
    uint32_t phys = (uint32_t)(pos - q->seg_start);
    uint32_t run;
    if (q->seg_legacy) {
        run = tsdb_legacy_run(q->db->legacy, q->seg_rank + phys);
    } else {
        run = q->seg_rpb - (phys % q->seg_rpb);
    }
    uint32_t left = q->seg_records - phys;
    if (run > left) run = left;
    uint32_t left_total = q->total - pos;
    return run < left_total ? run : left_total;
}

static void q_emit(tsdb_query_t *q, uint32_t phys, uint16_t off, int16_t *values) {
    const uint8_t *raw = (const uint8_t *) q->block_buffer;
    int16_t extras[TSDB_MAX_EXTRA_PARAMS];
    uint8_t legacy_np = 0;
    if (q->needs_extras) {
        legacy_np = q->db->legacy->header.num_params;
        tsdb_legacy_read_extras(q->db->legacy, q->seg_rank + phys, extras);
    }
    for (uint8_t r = 0; r < q->num_params_to_fetch; r++) {
        int8_t c = q->col_map[r];
        if (c < 0) {
            values[r] = 0;
        } else if (q->needs_extras && c >= (int8_t) legacy_np) {
            values[r] = extras[c - legacy_np];
        } else {
            values[r] = TSDB_BLOCK_PARAM(raw, q->seg_rpb, c, off);
        }
    }
}

// ============================================================================
// SEEK
// ============================================================================

/**
 * @brief First logical position with ts >= target (== total if none).
 *        ESP_FAIL on a probe anomaly (unreadable block / zero timestamp).
 */
static esp_err_t seek_pos(tsdb_query_t *q, uint32_t target, uint32_t *pos) {
    tsdb_t *db = q->db;
    if (q->total == 0) { *pos = 0; return ESP_OK; }

    // Segment whose last record is the first >= target.
    int lo_i = 0, hi_i = db->nsegs;          // segments are time-ordered
    while (lo_i < hi_i) {
        int mid = lo_i + (hi_i - lo_i) / 2;
        if (db->segs[mid].last_ts < target) lo_i = mid + 1; else hi_i = mid;
    }
    if (lo_i == db->nsegs) { *pos = q->total; return ESP_OK; }

    int64_t start = seg_logical_start(db, lo_i);
    uint32_t lo = start < 0 ? 0 : (uint32_t) start;
    uint32_t hi = (uint32_t)(start + db->segs[lo_i].records);
    if (hi > q->total) hi = q->total;
    // invariant: ts(lo-1) < target <= ts(hi)
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t ts;
        if (q_ts_at(q, mid, &ts) != ESP_OK || ts == 0) return ESP_FAIL;
        if (ts < target) lo = mid + 1; else hi = mid;
    }
    *pos = lo;
    return ESP_OK;
}

static esp_err_t window_positions(tsdb_query_t *q, uint32_t *first, uint32_t *last_plus_one) {
    if (q->total == 0) { *first = *last_plus_one = 0; return ESP_OK; }
    uint32_t a = 0, b = q->total;
    if (seek_pos(q, q->start_time, &a) != ESP_OK) return ESP_FAIL;
    if (q->end_time != UINT32_MAX && seek_pos(q, q->end_time + 1, &b) != ESP_OK) return ESP_FAIL;
    if (b < a) b = a;
    *first = a;
    *last_plus_one = b;
    return ESP_OK;
}

// ============================================================================
// QUERY API
// ============================================================================

esp_err_t tsdb_query_init_h(tsdb_t *db, tsdb_query_t *query, uint32_t start_time,
                            uint32_t end_time, const uint8_t *param_indices,
                            uint8_t num_params_to_fetch) {
    if (db == NULL || !db->is_open || query == NULL) return ESP_ERR_INVALID_STATE;
    if (db->closing) return ESP_ERR_INVALID_STATE;

    // The handle lock is held for the whole query lifetime; released by
    // tsdb_query_close(). Callers MUST close even on early exits.
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);

    if (start_time > end_time) {
        tsdb_unlock(db);
        return ESP_ERR_INVALID_ARG;
    }

    memset(query, 0, sizeof(*query));
    query->db = db;
    query->start_time = start_time;
    query->end_time = end_time;
    query->stride = 1;
    query->seg_i = -1;
    query->cur_block = UINT32_MAX;

    if (param_indices == NULL) {
        query->num_params_to_fetch = db->schema.count;
        for (uint8_t i = 0; i < db->schema.count; i++) query->param_indices[i] = i;
    } else {
        if (num_params_to_fetch > TSDB_MAX_PARAMS) {
            tsdb_unlock(db);
            return ESP_ERR_INVALID_ARG;
        }
        query->num_params_to_fetch = num_params_to_fetch;
        memcpy(query->param_indices, param_indices, num_params_to_fetch);
    }

    query->block_buffer = (tsdb_block_t *) tsdb_get_buffer_ptr(&db->pool, db->query_buffer_offset,
                                                               TSDB_BLOCK_SIZE);
    if (query->block_buffer == NULL) {
        query->block_buffer = heap_caps_malloc(TSDB_BLOCK_SIZE, MALLOC_CAP_8BIT);
        if (query->block_buffer == NULL) {
            tsdb_unlock(db);
            return ESP_ERR_NO_MEM;
        }
        query->owns_buffer = true;
    }

    // Unflushed batch records live only in the write cache; make them visible.
    if (db->cache_dirty) tsdb_write_cache_flush(db);

    query->total = tsdb_retained(db);
    query->pos = 0;
    db->open_queries++;

    // Whole-window early-outs: segs[0].first_ts is a lower bound on the
    // oldest retained timestamp, the newest segment's last_ts is exact.
    if (query->total > 0 &&
        (start_time > db->segs[db->nsegs - 1].last_ts || end_time < db->segs[0].first_ts)) {
        query->total = 0;
        return ESP_OK;
    }

    if (query->total > 0 && start_time > db->segs[0].first_ts) {
        uint32_t k = 0;
        if (seek_pos(query, start_time, &k) != ESP_OK) k = 0;
        query->pos = k;
    }
    return ESP_OK;
}

esp_err_t tsdb_query_set_stride(tsdb_query_t *query, uint32_t stride) {
    if (query == NULL || query->db == NULL) return ESP_ERR_INVALID_ARG;
    query->stride = stride == 0 ? 1 : stride;
    return ESP_OK;
}

esp_err_t tsdb_query_next(tsdb_query_t *query, uint32_t *timestamp, int16_t *values) {
    if (query == NULL || query->db == NULL || timestamp == NULL || values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    while (query->pos < query->total) {
        if (q_enter_pos(query, query->pos) != ESP_OK) return ESP_ERR_NOT_FOUND;
        uint32_t phys = (uint32_t)(query->pos - query->seg_start);
        uint32_t block; uint16_t off;
        q_locate(query, phys, &block, &off);
        if (q_load(query, block) != ESP_OK) return ESP_ERR_NOT_FOUND;

        uint32_t ts = TSDB_BLOCK_TS((uint8_t *) query->block_buffer, off);
        if (ts == 0 || ts < query->start_time) { query->pos++; continue; }
        if (ts > query->end_time) { query->pos = query->total; return ESP_ERR_NOT_FOUND; }

        *timestamp = ts;
        q_emit(query, phys, off, values);
        uint32_t next = query->pos + query->stride;
        query->pos = (next < query->pos || next > query->total) ? query->total : next;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void tsdb_query_close(tsdb_query_t *query) {
    if (query == NULL) return;
    tsdb_t *db = query->db;
    if (db && db->open_queries > 0) db->open_queries--;
    q_leave(query);
    if (query->owns_buffer && query->block_buffer) free(query->block_buffer);
    memset(query, 0, sizeof(*query));
    if (db) tsdb_unlock(db);
}

esp_err_t tsdb_query_count_h(tsdb_t *db, uint32_t start_time, uint32_t end_time, uint32_t *count) {
    if (db == NULL || !db->is_open || count == NULL) return ESP_ERR_INVALID_STATE;
    *count = 0;
    tsdb_query_t q;
    uint8_t one[1] = {0};
    esp_err_t ret = tsdb_query_init_h(db, &q, start_time, end_time, one, 1);
    if (ret != ESP_OK) return ret;
    uint32_t a, b;
    if (window_positions(&q, &a, &b) == ESP_OK) {
        *count = b - a;
    } else {
        uint32_t ts; int16_t v;
        q.pos = 0;
        while (tsdb_query_next(&q, &ts, &v) == ESP_OK) (*count)++;
    }
    tsdb_query_close(&q);
    return ESP_OK;
}

// ============================================================================
// AGGREGATION
// ============================================================================

typedef struct {
    int64_t sum;
    int32_t min_val, max_val;
    int16_t first_val, last_val;
} agg_acc_t;

static inline void acc_init(agg_acc_t *a) {
    a->sum = 0; a->min_val = INT32_MAX; a->max_val = INT32_MIN; a->first_val = a->last_val = 0;
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
        case TSDB_AGG_SUM:   return (int32_t) a->sum;
        case TSDB_AGG_AVG:   return (int32_t)(a->sum / (int64_t) count);
        case TSDB_AGG_MIN:   return a->min_val;
        case TSDB_AGG_MAX:   return a->max_val;
        case TSDB_AGG_COUNT: return (int32_t) count;
        case TSDB_AGG_FIRST: return a->first_val;
        case TSDB_AGG_LAST:  return a->last_val;
        default:             return 0;
    }
}

/**
 * @brief Block-level scan: one bounds check per record and one 16-bit load
 *        per accumulated column, straight off each loaded block. Falls back
 *        to per-record emission for segments that need legacy extras.
 */
static uint32_t agg_scan(tsdb_query_t *q, uint8_t ncols, agg_acc_t *acc) {
    uint32_t count = 0;
    int16_t vals[TSDB_MAX_PARAMS];
    while (q->pos < q->total) {
        if (q_enter_pos(q, q->pos) != ESP_OK) break;
        uint32_t phys = (uint32_t)(q->pos - q->seg_start);
        uint32_t block; uint16_t off;
        q_locate(q, phys, &block, &off);
        if (q_load(q, block) != ESP_OK) break;
        const uint8_t *raw = (const uint8_t *) q->block_buffer;
        uint32_t run = q_run(q, q->pos);
        if (run == 0) { q->pos++; continue; }

        for (uint32_t i = 0; i < run; i++) {
            uint32_t ts = TSDB_BLOCK_TS(raw, off + i);
            if (ts == 0 || ts < q->start_time) continue;
            if (ts > q->end_time) { q->pos = q->total; return count; }
            if (q->needs_extras) {
                q_emit(q, phys + i, (uint16_t)(off + i), vals);
                for (uint8_t c = 0; c < ncols; c++) acc_push(&acc[c], vals[c], count == 0);
            } else {
                for (uint8_t c = 0; c < ncols; c++) {
                    int8_t col = q->col_map[c];
                    int16_t v = col < 0 ? 0 : TSDB_BLOCK_PARAM(raw, q->seg_rpb, col, off + i);
                    acc_push(&acc[c], v, count == 0);
                }
            }
            count++;
        }
        q->pos += run;
    }
    return count;
}

esp_err_t tsdb_aggregate_h(tsdb_t *db, uint32_t start_time, uint32_t end_time,
                           uint8_t param_index, tsdb_agg_type_t agg_type, int32_t *result) {
    if (db == NULL || !db->is_open || result == NULL) return ESP_ERR_INVALID_STATE;
    if (param_index >= db->schema.count) return ESP_ERR_INVALID_ARG;
    if (agg_type < TSDB_AGG_SUM || agg_type > TSDB_AGG_LAST) return ESP_ERR_INVALID_ARG;

    uint8_t params[] = {param_index};
    tsdb_query_t q;
    esp_err_t ret = tsdb_query_init_h(db, &q, start_time, end_time, params, 1);
    if (ret != ESP_OK) return ret;

    agg_acc_t acc;
    acc_init(&acc);
    uint32_t count = 0;

    if (agg_type == TSDB_AGG_COUNT || agg_type == TSDB_AGG_FIRST || agg_type == TSDB_AGG_LAST) {
        uint32_t a, b;
        if (window_positions(&q, &a, &b) == ESP_OK) {
            count = b - a;
            if (count > 0 && agg_type != TSDB_AGG_COUNT) {
                uint32_t ts; int16_t v;
                q.pos = (agg_type == TSDB_AGG_FIRST) ? a : b - 1;
                if (tsdb_query_next(&q, &ts, &v) == ESP_OK) acc_push(&acc, v, true);
                else count = 0;
            }
            goto done;
        }
        q.pos = 0;   // anomaly: scan
    }
    count = agg_scan(&q, 1, &acc);

done:
    tsdb_query_close(&q);
    if (count == 0) { *result = 0; return ESP_OK; }
    *result = acc_result(&acc, agg_type, count);
    return ESP_OK;
}

esp_err_t tsdb_aggregate_multi_h(tsdb_t *db, uint32_t start_time, uint32_t end_time,
                                 tsdb_agg_request_t *requests, uint8_t num_requests,
                                 uint32_t *record_count) {
    if (db == NULL || !db->is_open || requests == NULL || num_requests == 0) return ESP_ERR_INVALID_STATE;
    if (num_requests > TSDB_MAX_PARAMS) return ESP_ERR_INVALID_ARG;

    uint8_t unique[TSDB_MAX_PARAMS], nu = 0, req_to[TSDB_MAX_PARAMS];
    for (uint8_t i = 0; i < num_requests; i++) {
        if (requests[i].param_index >= db->schema.count) return ESP_ERR_INVALID_ARG;
        uint8_t j;
        for (j = 0; j < nu; j++) if (unique[j] == requests[i].param_index) break;
        if (j == nu) unique[nu++] = requests[i].param_index;
        req_to[i] = j;
    }

    agg_acc_t *acc = malloc(sizeof(agg_acc_t) * nu);   // up to 64 × ~24 B: heap, not stack
    if (acc == NULL) return ESP_ERR_NO_MEM;

    tsdb_query_t q;
    esp_err_t ret = tsdb_query_init_h(db, &q, start_time, end_time, unique, nu);
    if (ret != ESP_OK) { free(acc); return ret; }

    for (uint8_t i = 0; i < nu; i++) acc_init(&acc[i]);
    uint32_t count = agg_scan(&q, nu, acc);
    tsdb_query_close(&q);

    for (uint8_t i = 0; i < num_requests; i++) {
        requests[i].result = count == 0 ? 0 : acc_result(&acc[req_to[i]], requests[i].agg_type, count);
    }
    free(acc);
    if (record_count) *record_count = count;
    return ESP_OK;
}

// ============================================================================
// LEGACY GLOBAL API
// ============================================================================

esp_err_t tsdb_query_init(tsdb_query_t *query, uint32_t start_time, uint32_t end_time,
                          const uint8_t *param_indices, uint8_t num_params_to_fetch) {
    return tsdb_query_init_h(g_default_handle, query, start_time, end_time, param_indices, num_params_to_fetch);
}
esp_err_t tsdb_query_count(uint32_t start_time, uint32_t end_time, uint32_t *count) {
    return tsdb_query_count_h(g_default_handle, start_time, end_time, count);
}
esp_err_t tsdb_aggregate(uint32_t start_time, uint32_t end_time, uint8_t param_index,
                         tsdb_agg_type_t agg_type, int32_t *result) {
    return tsdb_aggregate_h(g_default_handle, start_time, end_time, param_index, agg_type, result);
}
esp_err_t tsdb_aggregate_multi(uint32_t start_time, uint32_t end_time, tsdb_agg_request_t *requests,
                               uint8_t num_requests, uint32_t *record_count) {
    return tsdb_aggregate_multi_h(g_default_handle, start_time, end_time, requests, num_requests, record_count);
}
