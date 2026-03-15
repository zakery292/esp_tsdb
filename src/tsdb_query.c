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
// QUERY OPERATIONS
// ============================================================================

esp_err_t tsdb_query_init(tsdb_query_t *query,
                          uint32_t start_time,
                          uint32_t end_time,
                          const uint8_t *param_indices,
                          uint8_t num_params_to_fetch) {
    if (!g_state.is_open || query == NULL) {
        ESP_LOGE(TAG, "Invalid state or NULL query");
        return ESP_ERR_INVALID_STATE;
    }

    if (start_time > end_time) {
        ESP_LOGE(TAG, "Invalid time range: start > end");
        return ESP_ERR_INVALID_ARG;
    }

    memset(query, 0, sizeof(tsdb_query_t));

    // Copy header
    memcpy(&query->header, &g_state.header, sizeof(tsdb_header_t));

    // Set query parameters
    query->start_time = start_time;
    query->end_time = end_time;
    query->file = g_state.file;

    // Setup parameter indices
    if (param_indices == NULL) {
        // Fetch all parameters (base + extra)
        uint8_t total = query->header.num_params + g_state.extra_param_count;
        query->num_params_to_fetch = total;
        for (uint8_t i = 0; i < total; i++) {
            query->param_indices[i] = i;
        }
    } else {
        query->num_params_to_fetch = num_params_to_fetch;
        if (num_params_to_fetch > TSDB_MAX_PARAMS) {
            ESP_LOGE(TAG, "Too many parameters requested: %d", num_params_to_fetch);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(query->param_indices, param_indices, num_params_to_fetch);
    }

    // Try to use query buffer from pool
    query->block_buffer = (tsdb_block_t*)tsdb_get_buffer_ptr(&g_state.pool,
                                                              g_state.query_buffer_offset,
                                                              sizeof(tsdb_block_t));

    if (query->block_buffer == NULL) {
        // Paged mode and spans pages - allocate separately
        query->block_buffer = heap_caps_malloc(sizeof(tsdb_block_t), MALLOC_CAP_8BIT);
        if (query->block_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate query block buffer");
            return ESP_ERR_NO_MEM;
        }
        query->owns_buffer = true;
        ESP_LOGD(TAG, "Allocated separate query buffer at %p", query->block_buffer);
    } else {
        query->owns_buffer = false;
        ESP_LOGD(TAG, "Using query buffer from pool at %p", query->block_buffer);
    }

    // Find starting block using sparse index
    uint32_t start_block = 0;
    if (tsdb_find_block_for_timestamp(g_state.file, &query->header,
                                      start_time, &start_block) == ESP_OK) {
        query->current_block_num = start_block;
        ESP_LOGD(TAG, "Starting block: %lu", (unsigned long)start_block);
    }

    // Calculate record index range
    bool unlimited = (query->header.max_records == 0);
    uint32_t available_records = unlimited ? query->header.total_records :
                                 ((query->header.total_records < query->header.max_records) ?
                                  query->header.total_records : query->header.max_records);

    query->current_record_idx = query->header.oldest_record_idx;
    query->end_record_idx = unlimited ? available_records :
                           ((query->header.oldest_record_idx + available_records) %
                            query->header.max_records);

    query->offset_in_block = 0;
    query->block_loaded = false;

    ESP_LOGI(TAG, "Query initialized: time=[%lu, %lu], params=%d, records=%lu-%lu",
             (unsigned long)start_time, (unsigned long)end_time,
             query->num_params_to_fetch,
             (unsigned long)query->current_record_idx,
             (unsigned long)query->end_record_idx);

    return ESP_OK;
}

esp_err_t tsdb_query_next(tsdb_query_t *query,
                          uint32_t *timestamp,
                          int16_t *values) {
    if (query == NULL || timestamp == NULL || values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Main query loop
    while (true) {
        // Load block if needed
        if (!query->block_loaded) {
            esp_err_t ret = tsdb_read_block(query->file, query->current_block_num,
                                           query->block_buffer);
            if (ret != ESP_OK) {
                ESP_LOGD(TAG, "Block %lu not found or empty",
                         (unsigned long)query->current_block_num);
                return ESP_ERR_NOT_FOUND;
            }

            query->block_loaded = true;
            query->offset_in_block = 0;

            ESP_LOGD(TAG, "Loaded block %lu: %d records",
                     (unsigned long)query->current_block_num,
                     TSDB_BLOCK_COUNT((uint8_t *)query->block_buffer));
        }

        // Check if we've exhausted this block
        if (query->offset_in_block >= TSDB_BLOCK_COUNT((uint8_t *)query->block_buffer)) {
            // Move to next block
            query->current_block_num++;
            query->block_loaded = false;
            // Note: current_record_idx already incremented per-record in the loop below

            // Check if we've scanned all available records
            uint32_t total_scanned = query->current_record_idx - query->header.oldest_record_idx;
            bool q_unlimited = (query->header.max_records == 0);
            uint32_t available = q_unlimited ? query->header.total_records :
                                ((query->header.total_records < query->header.max_records) ?
                                 query->header.total_records : query->header.max_records);

            if (total_scanned >= available) {
                ESP_LOGD(TAG, "Scanned all available records");
                return ESP_ERR_NOT_FOUND;
            }

            continue;
        }

        // Read timestamp from current position
        uint8_t *qraw = (uint8_t *)query->block_buffer;
        uint16_t qrpb = query->header.records_per_block;
        uint32_t ts = TSDB_BLOCK_TS(qraw, query->offset_in_block);

        // Advance position
        query->offset_in_block++;
        query->current_record_idx++;

        // Check if timestamp is valid (non-zero)
        if (ts == 0) {
            continue;  // Skip uninitialized records
        }

        // Check if beyond end time (optimization: stop early)
        if (ts > query->end_time) {
            ESP_LOGD(TAG, "Reached end time");
            return ESP_ERR_NOT_FOUND;
        }

        // Check if in range
        if (ts >= query->start_time && ts <= query->end_time) {
            *timestamp = ts;

            // Extract requested parameters (columnar read for base, overflow for extra)
            bool need_overflow = false;
            for (uint8_t i = 0; i < query->num_params_to_fetch; i++) {
                if (query->param_indices[i] >= query->header.num_params) {
                    need_overflow = true;
                    break;
                }
            }

            // Calculate absolute record index for overflow lookups
            uint32_t abs_record_idx = 0;
            if (need_overflow && g_state.extra_param_count > 0) {
                bool ovf_unlimited = (query->header.max_records == 0);
                uint32_t available = ovf_unlimited ? query->header.total_records :
                                     ((query->header.total_records < query->header.max_records) ?
                                      query->header.total_records : query->header.max_records);
                abs_record_idx = (query->header.total_records - available) +
                                 (query->current_record_idx - 1 - query->header.oldest_record_idx);
            }

            for (uint8_t i = 0; i < query->num_params_to_fetch; i++) {
                uint8_t param_idx = query->param_indices[i];
                if (param_idx < query->header.num_params) {
                    values[i] = TSDB_BLOCK_PARAM(qraw, qrpb, param_idx, query->offset_in_block - 1);
                } else if (g_state.extra_param_count > 0 &&
                           param_idx < (query->header.num_params + g_state.extra_param_count) &&
                           abs_record_idx >= g_state.first_overflow_record_idx) {
                    uint32_t overflow_idx = abs_record_idx - g_state.first_overflow_record_idx;
                    uint8_t extra_idx = param_idx - query->header.num_params;
                    uint32_t ovf_offset = g_state.overflow_data_offset +
                                         (overflow_idx * g_state.overflow_record_size) +
                                         (extra_idx * sizeof(int16_t));

                    long saved_pos = ftell(query->file);
                    fseek(query->file, ovf_offset, SEEK_SET);
                    int16_t extra_val = 0;
                    if (fread(&extra_val, sizeof(int16_t), 1, query->file) == 1) {
                        values[i] = extra_val;
                    } else {
                        values[i] = 0;
                    }
                    fseek(query->file, saved_pos, SEEK_SET);
                } else {
                    values[i] = 0;  // Pre-overflow or invalid index
                }
            }

            ESP_LOGD(TAG, "Found record: ts=%lu", (unsigned long)ts);
            return ESP_OK;
        }

        // Skip record (before start_time)
    }
}

void tsdb_query_close(tsdb_query_t *query) {
    if (query == NULL) {
        return;
    }

    // Free buffer if we allocated it separately
    if (query->owns_buffer && query->block_buffer != NULL) {
        free(query->block_buffer);
        query->block_buffer = NULL;
        ESP_LOGD(TAG, "Freed separate query buffer");
    }

    memset(query, 0, sizeof(tsdb_query_t));
}

esp_err_t tsdb_query_count(uint32_t start_time,
                           uint32_t end_time,
                           uint32_t *count) {
    if (!g_state.is_open || count == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *count = 0;

    // Initialize query
    tsdb_query_t query;
    esp_err_t ret = tsdb_query_init(&query, start_time, end_time, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    // Count records
    uint32_t ts;
    int16_t values[TSDB_MAX_PARAMS];

    while (tsdb_query_next(&query, &ts, values) == ESP_OK) {
        (*count)++;
    }

    tsdb_query_close(&query);

    ESP_LOGI(TAG, "Counted %lu records in range [%lu, %lu]",
             (unsigned long)*count, (unsigned long)start_time, (unsigned long)end_time);

    return ESP_OK;
}

// ============================================================================
// AGGREGATION OPERATIONS
// ============================================================================

esp_err_t tsdb_aggregate(uint32_t start_time,
                         uint32_t end_time,
                         uint8_t param_index,
                         tsdb_agg_type_t agg_type,
                         int32_t *result) {
    if (!g_state.is_open || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (param_index >= (g_state.header.num_params + g_state.extra_param_count)) {
        ESP_LOGE(TAG, "Invalid parameter index: %d", param_index);
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize query for single parameter
    uint8_t params[] = {param_index};
    tsdb_query_t query;
    esp_err_t ret = tsdb_query_init(&query, start_time, end_time, params, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    // Accumulators
    int64_t sum = 0;
    int32_t min_val = INT32_MAX;
    int32_t max_val = INT32_MIN;
    uint32_t count = 0;
    int16_t first_val = 0;
    int16_t last_val = 0;

    // Single-pass aggregation
    uint32_t ts;
    int16_t value;

    while (tsdb_query_next(&query, &ts, &value) == ESP_OK) {
        if (count == 0) {
            first_val = value;
        }
        last_val = value;

        sum += value;
        count++;

        if (value < min_val) min_val = value;
        if (value > max_val) max_val = value;
    }

    tsdb_query_close(&query);

    // Calculate result based on aggregation type
    if (count == 0) {
        *result = 0;
        ESP_LOGW(TAG, "No records found in range");
        return ESP_OK;
    }

    switch (agg_type) {
        case TSDB_AGG_SUM:
            *result = (int32_t)sum;
            break;
        case TSDB_AGG_AVG:
            *result = (int32_t)(sum / count);
            break;
        case TSDB_AGG_MIN:
            *result = min_val;
            break;
        case TSDB_AGG_MAX:
            *result = max_val;
            break;
        case TSDB_AGG_COUNT:
            *result = count;
            break;
        case TSDB_AGG_FIRST:
            *result = first_val;
            break;
        case TSDB_AGG_LAST:
            *result = last_val;
            break;
        default:
            ESP_LOGE(TAG, "Unknown aggregation type: %d", agg_type);
            return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Aggregation complete: type=%d, result=%ld, count=%lu",
             agg_type, (long)*result, (unsigned long)count);

    return ESP_OK;
}

esp_err_t tsdb_aggregate_multi(uint32_t start_time,
                                uint32_t end_time,
                                tsdb_agg_request_t *requests,
                                uint8_t num_requests,
                                uint32_t *record_count) {
    if (!g_state.is_open || requests == NULL || num_requests == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // Collect unique param indices needed
    uint8_t unique_params[TSDB_MAX_PARAMS];
    uint8_t num_unique = 0;
    for (uint8_t i = 0; i < num_requests; i++) {
        if (requests[i].param_index >= (g_state.header.num_params + g_state.extra_param_count)) {
            ESP_LOGE(TAG, "Invalid parameter index: %d", requests[i].param_index);
            return ESP_ERR_INVALID_ARG;
        }
        // Check if already in unique list
        bool found = false;
        for (uint8_t j = 0; j < num_unique; j++) {
            if (unique_params[j] == requests[i].param_index) {
                found = true;
                break;
            }
        }
        if (!found) {
            unique_params[num_unique++] = requests[i].param_index;
        }
    }

    // Open single query for all needed params
    tsdb_query_t query;
    esp_err_t ret = tsdb_query_init(&query, start_time, end_time, unique_params, num_unique);
    if (ret != ESP_OK) {
        return ret;
    }

    // Per-request accumulators
    typedef struct {
        int64_t sum;
        int32_t min_val;
        int32_t max_val;
        int16_t first_val;
        int16_t last_val;
    } agg_acc_t;

    agg_acc_t acc[TSDB_MAX_PARAMS];
    for (uint8_t i = 0; i < num_requests; i++) {
        acc[i].sum = 0;
        acc[i].min_val = INT32_MAX;
        acc[i].max_val = INT32_MIN;
        acc[i].first_val = 0;
        acc[i].last_val = 0;
    }

    // Build a mapping: for each request, which index in the query values array has its param?
    uint8_t req_to_query_idx[TSDB_MAX_PARAMS];
    for (uint8_t i = 0; i < num_requests; i++) {
        for (uint8_t j = 0; j < num_unique; j++) {
            if (unique_params[j] == requests[i].param_index) {
                req_to_query_idx[i] = j;
                break;
            }
        }
    }

    // Single pass
    uint32_t ts;
    int16_t values[TSDB_MAX_PARAMS];
    uint32_t count = 0;

    while (tsdb_query_next(&query, &ts, values) == ESP_OK) {
        for (uint8_t i = 0; i < num_requests; i++) {
            int16_t val = values[req_to_query_idx[i]];
            if (count == 0) {
                acc[i].first_val = val;
            }
            acc[i].last_val = val;
            acc[i].sum += val;
            if (val < acc[i].min_val) acc[i].min_val = val;
            if (val > acc[i].max_val) acc[i].max_val = val;
        }
        count++;
    }

    tsdb_query_close(&query);

    // Compute results
    for (uint8_t i = 0; i < num_requests; i++) {
        if (count == 0) {
            requests[i].result = 0;
            continue;
        }
        switch (requests[i].agg_type) {
            case TSDB_AGG_SUM:   requests[i].result = (int32_t)acc[i].sum; break;
            case TSDB_AGG_AVG:   requests[i].result = (int32_t)(acc[i].sum / count); break;
            case TSDB_AGG_MIN:   requests[i].result = acc[i].min_val; break;
            case TSDB_AGG_MAX:   requests[i].result = acc[i].max_val; break;
            case TSDB_AGG_COUNT: requests[i].result = (int32_t)count; break;
            case TSDB_AGG_FIRST: requests[i].result = acc[i].first_val; break;
            case TSDB_AGG_LAST:  requests[i].result = acc[i].last_val; break;
            default:             requests[i].result = 0; break;
        }
    }

    if (record_count) {
        *record_count = count;
    }

    ESP_LOGI(TAG, "Multi-aggregate complete: %d requests, %lu records scanned",
             num_requests, (unsigned long)count);

    return ESP_OK;
}
