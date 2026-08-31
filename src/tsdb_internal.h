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
// PER-INSTANCE STATE
// ============================================================================

/**
 * @brief Database handle internals.
 *
 * Forward-declared as `tsdb_t` (opaque) in esp_tsdb.h so multiple instances
 * can be opened simultaneously. Each handle carries its own file descriptor,
 * header cache, buffer pool, overflow state, and FreeRTOS mutex.
 *
 * v2 single-DB callers see a static `g_default_handle` of this type wrapped
 * by the legacy global API (tsdb_init/tsdb_write/etc).
 */
// ---------------------------------------------------------------------------
// Sidecar header
//
// The header is a small structure at file offset 0. Rewriting it in place is
// the single most expensive thing tsdb_write_h does — measured on an ESP32-S3
// with a 3 MB littlefs partition:
//
//   in-place rewrite at offset 0, 192 KB file    3684-3802 ms
//   rewrite a small separate file (fopen "wb")     56-80 ms
//   ping-pong between two small files            134-141 ms
//
// littlefs pays copy-on-write costs proportional to the data from the modified
// offset to end-of-file, so touching offset 0 of a large file is worst-case.
// A small standalone file has almost nothing after it.
//
// So the hot path writes a SIDECAR instead: two alternating slots, <db>.h0 and
// <db>.h1, each holding {magic, seq, crc32, header}. Alternating means a crash
// mid-write always leaves the other slot intact — unlike truncate-and-rewrite,
// which is faster still but has a window with no valid copy anywhere.
//
// On open, the newest valid slot wins over the in-file header — but only if it
// passes CRC, matches the file's geometry, and describes at least as many
// records. On close/sync the in-file header is written too, so a cleanly closed
// file stays self-contained and readable by an unmodified esp_tsdb.
//
// In situ this is 46-56 ms per write against 5124 ms for the same header on a
// 268 KB database. Crash recovery is verified on hardware: with the last clean
// close two commits behind, power was pulled with no shutdown handler and both
// commits came back, with no records lost or altered.
#define TSDB_SIDECAR_MAGIC 0x54534831u  /* "TSH1" */

typedef struct {
    uint32_t magic;
    uint32_t seq;         // monotonic; the higher of the two slots is newer
    uint32_t header_crc;  // crc32 over `header` only — detects a torn write
    tsdb_header_t header;
} tsdb_sidecar_t;

struct tsdb_s {
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

    // Adaptive capacity: once THIS database has shrunk max_records in response
    // to ENOSPC, don't keep shrinking on every subsequent write. Per-handle —
    // one instance filling the filesystem must not suppress adaptation on
    // another. Resets naturally when the handle is reopened (e.g. next boot).
    bool capacity_adapted;

    // True once a record has been written whose header state lives only in the
    // sidecar. Cleared when close/sync folds the header back into the file, so
    // those paths can skip the write entirely for an untouched database.
    bool in_file_header_stale;

    // Sidecar state (see TSDB_SIDECAR_MAGIC above). The sequence number
    // increments on every sidecar write and decides which slot is newer; the
    // slot alternates 0/1 so the previous one always survives a torn write.
    uint32_t sidecar_seq;
    uint8_t sidecar_slot;

    // Set for the duration of tsdb_migrate_schema_h(). Writers/queriers check
    // this BEFORE taking the mutex so they fail fast with
    // ESP_ERR_INVALID_STATE instead of stalling their full lock timeout while
    // the migration (seconds to a minute) holds the handle lock.
    volatile bool migrating;

    // Set for the duration of tsdb_close_h(). Latency-only optimisation:
    // callers that check it BEFORE taking the mutex are spared blocking
    // behind a teardown they'd only bail out of. It is NOT the safety
    // mechanism — already-blocked callers recover via the post-lock
    // is_open + generation re-check in TSDB_LOCK_OPEN_OR_RETURN.
    volatile bool closing;

    // Incarnation stamp. Preserved (and incremented) across the recycler's
    // memset in tsdb_acquire_handle — like the mutex itself — so a straggler
    // that blocked on the mutex during incarnation N can detect that the
    // handle it wakes up on has been reopened as incarnation N+1 (is_open is
    // true again by then, so is_open alone cannot catch this: the straggler
    // would silently write its stale record into the NEW database).
    uint32_t generation;

    // Optional free-space guard (copied from tsdb_config_t): consulted before
    // the data file grows by a new block. See tsdb_write_h.
    uint64_t (*free_space_cb)(void);
    uint32_t min_free_bytes;

    // Overflow state
    uint8_t  extra_param_count;
    uint32_t overflow_data_offset;      // overflow_offset + TSDB_OVERFLOW_HEADER_SIZE
    uint32_t first_overflow_record_idx;
    uint16_t overflow_record_size;

    // Per-handle serialization. Acquired by every public _h-suffixed call (and
    // the legacy global API via g_default_handle) so concurrent writers/queriers
    // on the same handle are safe; different handles are fully independent.
    //
    // RECURSIVE: the HTTP handler calling tsdb_clear()/tsdb_close() races the
    // inverter poller mid-tsdb_write() — without serialization, close()
    // invalidates db->file while a writer is still using it (crash/corruption).
    // The close→delete→init chain from migration paths re-enters the lock on the
    // same task, so a non-recursive mutex would self-deadlock.
    SemaphoreHandle_t mutex;
};

// Legacy single-DB handle backing the v2 global API. Allocated lazily on the
// first tsdb_init() call; freed by tsdb_close().
extern tsdb_t *g_default_handle;

// Per-handle lock helpers. Recursive mutex — same task may re-enter via
// close→delete→init or write→header chains without deadlock. NULL-tolerant so
// pre-init / pre-allocation paths (and tsdb_close_h(NULL)) are no-ops.
static inline bool tsdb_lock(tsdb_t *db, uint32_t timeout_ms) {
    if (db == NULL || db->mutex == NULL) return true;  // pre-init paths
    return xSemaphoreTakeRecursive(db->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static inline void tsdb_unlock(tsdb_t *db) {
    if (db != NULL && db->mutex != NULL) {
        xSemaphoreGiveRecursive(db->mutex);
    }
}

#define TSDB_LOCK_OR_RETURN(db, timeout_ms, errval) \
    do { if (!tsdb_lock((db), timeout_ms)) { \
        ESP_LOGE(TAG, "%s: lock timeout", __func__); \
        return (errval); \
    } } while (0)

// Take the handle lock AND re-verify the handle once inside. The pre-lock
// `is_open` test is only a fast-path optimisation: a closer can win the mutex
// first and tear the handle down while this task is blocked in xSemaphoreTake
// — so after acquiring, the state must be re-checked before touching
// db->file / pool. (The mutex itself is never deleted — closed handles are
// parked for reuse, see tsdb_retire_handle in tsdb_core.c.)
//
// The generation check catches the close-AND-reopen interleaving: if the
// handle was recycled into a new incarnation while this task was blocked,
// is_open is true again but the generation differs — without this, a stale
// writer would silently inject its old record into the freshly-opened
// database (e.g. one bogus pre-clear record after a clear-all).
#define TSDB_LOCK_OPEN_OR_RETURN(db, timeout_ms, errval) \
    do { uint32_t _tsdb_gen_ = (db)->generation; \
        TSDB_LOCK_OR_RETURN((db), (timeout_ms), (errval)); \
        if (!(db)->is_open || (db)->generation != _tsdb_gen_) { \
            tsdb_unlock(db); \
            ESP_LOGW(TAG, "%s: handle closed/reopened while waiting for lock", __func__); \
            return ESP_ERR_INVALID_STATE; \
        } } while (0)

// ============================================================================
// INTERNAL FUNCTIONS
// ============================================================================
// Note: struct tsdb_query_s is now in esp_tsdb.h (public API)

// Core operations (tsdb_core.c)
esp_err_t tsdb_read_header(FILE *file, tsdb_header_t *header);
esp_err_t tsdb_write_header(FILE *file, const tsdb_header_t *header);

// Sidecar header persistence — see TSDB_SIDECAR_MAGIC.
esp_err_t tsdb_sidecar_write(tsdb_t *db);
// Loads the newest valid sidecar slot into *out. ESP_ERR_NOT_FOUND if neither
// slot is present/valid, in which case the caller keeps the in-file header.
esp_err_t tsdb_sidecar_load(const char *filepath, tsdb_header_t *out, uint32_t *seq_out);
void tsdb_sidecar_remove(const char *filepath);

// Block operations (tsdb_write.c, tsdb_query.c)
esp_err_t tsdb_read_block(tsdb_t *db, uint32_t block_num, tsdb_block_t *block);
esp_err_t tsdb_write_block(tsdb_t *db, uint32_t block_num, const tsdb_block_t *block);
uint32_t tsdb_calc_block_offset(const tsdb_header_t *header, uint32_t block_num);

// Index operations (tsdb_index.c)
esp_err_t tsdb_find_block_for_timestamp(FILE *file,
                                        const tsdb_header_t *header,
                                        uint32_t timestamp,
                                        uint32_t *block_num);

// Schema-migration crash recovery (tsdb_migrate.c). Called by tsdb_open()
// before touching the main file: removes a stale `<path>.mig` leftover, or
// adopts a completed one if a crash hit the unlink->rename swap window.
void tsdb_migrate_recover(const char *filepath);

#endif // TSDB_INTERNAL_H
