/**
 * @file tsdb_internal.h
 * @brief Internal structures and functions for ESP TSDB
 */

#ifndef TSDB_INTERNAL_H
#define TSDB_INTERNAL_H

#include "esp_tsdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdio.h>

// ============================================================================
// FILE FORMAT STRUCTURES (internal only)
// ============================================================================
// Note: tsdb_header_t and tsdb_block_t are now in esp_tsdb.h (public API)

/**
 * @brief Sparse time index entry (internal only)
 */
typedef struct {
    uint32_t timestamp;             // First timestamp in block range
    uint32_t block_number;          // Physical block number
} __attribute__((packed)) tsdb_index_entry_t;

// ============================================================================
// BUFFER POOL MANAGEMENT
// ============================================================================

#define TSDB_MAX_PAGES 128          // Max pages for paged allocation

/**
 * @brief Buffer pool (paged or contiguous)
 */
typedef struct {
    void *pages[TSDB_MAX_PAGES];    // Page pointers
    uint8_t num_pages;              // Number of allocated pages
    size_t page_size;               // Size of each page
    size_t total_size;              // Total allocated size
    bool is_paged;                  // true = paged, false = contiguous
} tsdb_buffer_pool_t;

// Buffer pool functions (implemented in tsdb_buffer.c)
esp_err_t tsdb_alloc_buffer_pool(tsdb_buffer_pool_t *pool,
                                 size_t total_size,
                                 bool use_paged,
                                 size_t page_size,
                                 tsdb_alloc_strategy_t strategy);
void tsdb_free_buffer_pool(tsdb_buffer_pool_t *pool);
void* tsdb_get_buffer_ptr(tsdb_buffer_pool_t *pool, size_t offset, size_t size);
void tsdb_buffer_read(tsdb_buffer_pool_t *pool, size_t offset, void *dest, size_t size);
void tsdb_buffer_write(tsdb_buffer_pool_t *pool, size_t offset, const void *src, size_t size);

// ============================================================================
// GLOBAL STATE
// ============================================================================

/**
 * @brief Internal state
 */
typedef struct {
    FILE *file;
    tsdb_header_t header;
    char filepath[128];
    bool is_open;

    // Buffer pool
    tsdb_buffer_pool_t pool;

    // Logical buffer regions (offsets into pool)
    size_t read_buffer_offset;      // Offset for block read buffer
    size_t write_cache_offset;      // Offset for block write cache
    size_t query_buffer_offset;     // Offset for query iterator
    size_t stream_buffer_offset;    // Offset for streaming/temp data
    size_t stream_buffer_size;      // Size of stream buffer

    // Write cache state
    uint32_t cached_block_num;
    bool cache_dirty;

    // Overflow state
    uint8_t  extra_param_count;
    uint32_t overflow_data_offset;      // overflow_offset + TSDB_OVERFLOW_HEADER_SIZE
    uint32_t first_overflow_record_idx;
    uint16_t overflow_record_size;

    // Global mutex serialising every public mutating + reading API. The HTTP
    // handler that calls tsdb_clear()/tsdb_close() races the inverter poller
    // mid-tsdb_write() — without this lock, close() invalidates g_state.file
    // while a writer is still using it (crash / corruption). Recursive so the
    // close→delete→init chain from migration paths doesn't self-deadlock.
    SemaphoreHandle_t lock;
} tsdb_state_t;

// Global state (defined in tsdb_core.c)
extern tsdb_state_t g_state;

// Lock helpers (defined in tsdb_core.c). Recursive mutex — same task may
// re-enter via close→delete→init or write→header chains without deadlock.
static inline bool tsdb_lock(uint32_t timeout_ms) {
    if (g_state.lock == NULL) return true;  // pre-init paths
    return xSemaphoreTakeRecursive(g_state.lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static inline void tsdb_unlock(void) {
    if (g_state.lock != NULL) {
        xSemaphoreGiveRecursive(g_state.lock);
    }
}

#define TSDB_LOCK_OR_RETURN(timeout_ms, errval) \
    do { if (!tsdb_lock(timeout_ms)) { \
        ESP_LOGE(TAG, "%s: lock timeout", __func__); \
        return (errval); \
    } } while (0)

// ============================================================================
// INTERNAL FUNCTIONS
// ============================================================================
// Note: struct tsdb_query_s is now in esp_tsdb.h (public API)

// Core operations (tsdb_core.c)
esp_err_t tsdb_read_header(FILE *file, tsdb_header_t *header);
esp_err_t tsdb_write_header(FILE *file, const tsdb_header_t *header);

// Block operations (tsdb_write.c, tsdb_query.c)
esp_err_t tsdb_read_block(FILE *file, uint32_t block_num, tsdb_block_t *block);
esp_err_t tsdb_write_block(FILE *file, uint32_t block_num, const tsdb_block_t *block);
uint32_t tsdb_calc_block_offset(const tsdb_header_t *header, uint32_t block_num);

// Index operations (tsdb_index.c)
esp_err_t tsdb_find_block_for_timestamp(FILE *file,
                                        const tsdb_header_t *header,
                                        uint32_t timestamp,
                                        uint32_t *block_num);

#endif // TSDB_INTERNAL_H
