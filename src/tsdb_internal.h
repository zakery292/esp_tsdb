/**
 * @file tsdb_internal.h
 * @brief Internal structures and functions for ESP TSDB (v3 segment format)
 *
 * FORMAT OVERVIEW (v3)
 *
 * A database is a set of append-only SEGMENT files next to the configured
 * path: `<path>.s0000000001`, `<path>.s0000000002`, ... Each segment is a
 * write-once header (schema: column names + aliases, geometry, lifetime
 * counters) followed by 1 KB columnar data blocks in time order. Records are
 * only ever appended to the newest segment; when it reaches its block
 * capacity, or the schema changes, the next write starts a new one.
 *
 * Eviction is logical at the head and physical at segment granularity: with
 * `total_physical` records across all segments and a capacity of
 * `max_records`, the oldest `head_skip = total_physical - max_records`
 * records of the oldest segment are invisible; once a whole segment is
 * skipped it is unlinked. Capacity is therefore exact, nothing needs to be
 * persisted to track it, and no block is ever rewritten in place except the
 * tail block of the newest segment.
 *
 * WHY: littlefs stores a file as a backward-linked skip-list of blocks, so a
 * write in the middle of a file rewrites every block after it (~20 ms/KB,
 * measured in 2.4.0). The v1/v2 ring buffer wrote block 0, 1, 2... in place
 * once it wrapped — tens of seconds per write on a 2 MB file. Appending to
 * the tail of a small file and unlinking old files are both cheap.
 *
 * A pre-3.0 single-file database at `<path>` is adopted read-only as the
 * oldest segment (sequence 0) without copying — see tsdb_legacy.c.
 */

#ifndef TSDB_INTERNAL_H
#define TSDB_INTERNAL_H

#include "esp_tsdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif

#define TSDB_BLOCK_MAGIC_VALUE 0x424C4B54u   /* "BLKT" */

// ============================================================================
// BUFFER POOL MANAGEMENT
// ============================================================================

#define TSDB_MAX_PAGES 128          // Max pages for paged allocation

typedef struct {
    void *pages[TSDB_MAX_PAGES];    // Page pointers
    uint8_t num_pages;              // Number of allocated pages
    size_t page_size;               // Size of each page
    size_t total_size;              // Total allocated size
    bool is_paged;                  // true = paged, false = contiguous
} tsdb_buffer_pool_t;

esp_err_t tsdb_alloc_buffer_pool(tsdb_buffer_pool_t *pool, size_t total_size,
                                 bool use_paged, size_t page_size,
                                 tsdb_alloc_strategy_t strategy);
void tsdb_free_buffer_pool(tsdb_buffer_pool_t *pool);
void* tsdb_get_buffer_ptr(tsdb_buffer_pool_t *pool, size_t offset, size_t size);
void tsdb_buffer_read(tsdb_buffer_pool_t *pool, size_t offset, void *dest, size_t size);
void tsdb_buffer_write(tsdb_buffer_pool_t *pool, size_t offset, const void *src, size_t size);

// ============================================================================
// SCHEMA
// ============================================================================

/**
 * @brief The current logical column set. Columns [0, base_count) come from
 *        the config; [base_count, count) are runtime extras
 *        (tsdb_add_extra_params / tsdb_migrate_overflow). `aliases[i]` is the
 *        previous name of column i after a tsdb_rename_param (one hop), so
 *        segments written under the old name still map to it.
 */
typedef struct {
    uint8_t  count;
    uint8_t  base_count;
    uint32_t gen;                   // bumps on every change; segments record it
    char names[TSDB_MAX_PARAMS][TSDB_NAME_LEN];
    char aliases[TSDB_MAX_PARAMS][TSDB_NAME_LEN];
} tsdb_schema_t;

// ============================================================================
// SEGMENT FILES
// ============================================================================

/**
 * @brief On-disk segment header (64 bytes, write-once at creation), followed
 *        by names[num_params][TSDB_NAME_LEN] and aliases[num_params][TSDB_NAME_LEN];
 *        data blocks start at `data_offset` (the whole prefix rounded up to
 *        TSDB_BLOCK_SIZE).
 */
typedef struct {
    uint32_t magic;             // TSDB_SEG_MAGIC
    uint32_t version;           // TSDB_SEG_VERSION
    uint32_t seq;               // 1..; 0 is reserved for the legacy file
    uint8_t  num_params;        // columns in this segment
    uint8_t  base_params;       // of which from the config (rest = extras)
    uint16_t records_per_block;
    uint32_t blocks_cap;        // capacity in data blocks
    uint32_t data_offset;       // file offset of block 0
    uint32_t schema_gen;        // tsdb_schema_t.gen at creation
    uint32_t lifetime_base;     // records written to the DB before this segment
    uint32_t created_ts;        // timestamp of the first record
    uint8_t  reserved[28];
} __attribute__((packed)) tsdb_seg_header_t;

#define TSDB_SEG_HEADER_SIZE 64

/**
 * @brief In-RAM segment table entry. seq 0 = the adopted legacy file.
 */
typedef struct {
    uint32_t seq;
    uint32_t records;           // valid physical records
    uint32_t first_ts;          // timestamp of physical record 0
    uint32_t last_ts;           // timestamp of the newest record
    uint32_t blocks_cap;
    uint32_t data_offset;
    uint32_t schema_gen;
    uint32_t lifetime_base;
    uint16_t rpb;
    uint8_t  np;
} tsdb_seg_t;

// ============================================================================
// LEGACY (pre-3.0 single file) — tsdb_legacy.c
// ============================================================================

typedef struct {
    FILE *file;
    char path[128];
    tsdb_header_t header;       // as adopted (sidecar-aware, reconstructed if needed)
    bool unlimited;
    uint32_t available;         // retained records
    uint32_t first_slot;        // ring slot of the oldest retained record
    // Overflow extras (sequential region past the ring)
    uint8_t  extra_count;
    uint32_t overflow_data_offset;
    uint32_t first_overflow_record_idx;
    uint16_t overflow_record_size;
} tsdb_legacy_t;

// Opens <path> if it is a pre-3.0 database, read-only. ESP_ERR_NOT_FOUND if
// absent; ESP_ERR_INVALID_RESPONSE if present but unusable (it is then
// removed so the next open is clean); any other error (I/O failure while
// converting a V2 file, out of memory) is the caller's to report — the file
// is left in place. Performs the pre-3.0 recovery steps (sidecar adoption
// incl. 2.4.4's <path>.d/ slots, header reconstruction, V2->V3 block layout
// migration, .mig adoption).
esp_err_t tsdb_legacy_open(const char *path, const tsdb_config_t *config, tsdb_legacy_t **out);
void tsdb_legacy_close(tsdb_legacy_t *l);
// Unlinks the file, its sidecars (<path>.h0/.h1, <path>.d/) and any .mig leftover.
void tsdb_legacy_remove(const char *path);
uint8_t tsdb_legacy_columns(const tsdb_legacy_t *l);
// Column name (base, V4 extended or overflow) into out[TSDB_NAME_LEN].
void tsdb_legacy_column_name(tsdb_legacy_t *l, uint8_t idx, char *out);
// Physical record k (0 = oldest retained) -> block number and offset in block.
void tsdb_legacy_locate(const tsdb_legacy_t *l, uint32_t k, uint32_t *block, uint16_t *off);
uint32_t tsdb_legacy_block_offset(const tsdb_legacy_t *l, uint32_t block);
// Longest run of consecutive physical records starting at k that share a block.
uint32_t tsdb_legacy_run(const tsdb_legacy_t *l, uint32_t k);
// Overflow extras for physical record k (zeros when none / pre-overflow).
void tsdb_legacy_read_extras(tsdb_legacy_t *l, uint32_t k, int16_t *out);
long tsdb_legacy_file_size(const tsdb_legacy_t *l);

// ============================================================================
// PER-INSTANCE STATE
// ============================================================================

struct tsdb_s {
    char filepath[128];
    bool is_open;

    // Buffer pool and its logical regions
    tsdb_buffer_pool_t pool;
    size_t read_buffer_offset;      // eviction / stats block reads
    size_t write_cache_offset;      // block being appended to
    size_t query_buffer_offset;     // query iterator

    // Write cache: the tail block of the active segment stays here so the
    // next record landing in it skips the read-back. cache_dirty is only true
    // inside tsdb_write_batch_h (deferred flush).
    uint32_t cached_block_num;      // block index within the active segment; UINT32_MAX = none
    bool cache_dirty;
    // Last successful flush of a deferred block: the newest segment then had
    // flushed_records records, all on disk (tsdb_write_batch_h's rollback).
    uint32_t flushed_seq;
    uint32_t flushed_records;
    uint32_t flushed_last_ts;

    // Schema and capacity
    tsdb_schema_t schema;
    uint32_t cfg_max_records;       // from config (0 = unlimited)
    uint32_t max_records;           // effective (latched lower by ENOSPC / the free-space guard)
    bool capacity_adapted;
    uint32_t seg_blocks;            // data blocks per new segment (config / auto)
    uint64_t (*free_space_cb)(void);
    uint32_t min_free_bytes;

    // Segment table, ascending by seq (index 0 may be the legacy file, seq 0)
    tsdb_seg_t *segs;
    uint16_t nsegs;
    uint16_t segs_cap;
    uint32_t total_physical;        // sum of segs[i].records
    uint32_t head_skip;             // logically evicted records at the head of segs[0]
    uint32_t next_seq;

    // Newest segment, held open for appends (NULL when none yet / legacy only).
    // Queries read it through THIS handle — littlefs does not support a second
    // handle on a file that is open for writing.
    FILE *active;

    tsdb_legacy_t *legacy;          // non-NULL while segs[0] is the legacy file

    // Queries open on this handle (same task, recursive mutex). Schema
    // changes, clear and delete close/unlink files a query may be reading
    // through, so they refuse while this is non-zero.
    uint16_t open_queries;

    // Lifecycle / concurrency (see tsdb_core.c)
    volatile bool migrating;        // kept for API compatibility; schema ops are instant now
    volatile bool closing;
    uint32_t generation;
    SemaphoreHandle_t mutex;
};

extern tsdb_t *g_default_handle;

static inline bool tsdb_lock(tsdb_t *db, uint32_t timeout_ms) {
    if (db == NULL || db->mutex == NULL) return true;
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

// Take the handle lock AND re-verify the handle once inside (a closer may have
// torn the handle down while we were blocked; the mutex itself is never
// deleted — closed handles are parked for reuse). The generation check
// catches a close-and-reopen interleaving.
#define TSDB_LOCK_OPEN_OR_RETURN(db, timeout_ms, errval) \
    do { uint32_t _tsdb_gen_ = (db)->generation; \
        TSDB_LOCK_OR_RETURN((db), (timeout_ms), (errval)); \
        if (!(db)->is_open || (db)->generation != _tsdb_gen_) { \
            tsdb_unlock(db); \
            ESP_LOGW(TAG, "%s: handle closed/reopened while waiting for lock", __func__); \
            return (errval); \
        } } while (0)

// ============================================================================
// INTERNAL FUNCTIONS
// ============================================================================

// --- tsdb_segment.c ---------------------------------------------------------
void tsdb_seg_path(const tsdb_t *db, uint32_t seq, char *out, size_t out_len);
// Scan the directory for segment files, validate each (header + tail block),
// fill db->segs (after any legacy entry). Empty/invalid segments are removed.
esp_err_t tsdb_seg_scan(tsdb_t *db);
// Read a segment's column names/aliases (NULL-able outputs). Uses `f` when
// given (the active segment's handle), else opens the file read-only.
esp_err_t tsdb_seg_read_schema(tsdb_t *db, const tsdb_seg_t *seg, FILE *f,
                               char names[][TSDB_NAME_LEN], char aliases[][TSDB_NAME_LEN],
                               uint8_t *base_params);
// Create the next segment with the current schema; db->active becomes its
// FILE* and the table gains its entry. ESP_ERR_NO_MEM on ENOSPC.
esp_err_t tsdb_seg_create(tsdb_t *db, uint32_t first_ts);
// Table maintenance
esp_err_t tsdb_seg_table_push(tsdb_t *db, const tsdb_seg_t *seg);
void tsdb_seg_table_pop_front(tsdb_t *db);
// Unlink segs[0] (legacy or segment) and drop it from the table. false if it
// is the active segment (never evicted physically) or the table is empty.
bool tsdb_evict_oldest(tsdb_t *db);
// Recompute head_skip from total_physical/max_records, unlinking fully
// skipped segments.
void tsdb_apply_capacity(tsdb_t *db);
// Unlink every segment file and the legacy file; empties the table.
void tsdb_remove_all_files(tsdb_t *db);
// Block I/O on a segment file. ESP_ERR_NO_MEM on ENOSPC.
esp_err_t tsdb_seg_read_block(FILE *f, uint32_t data_offset, uint32_t block, void *buf);
esp_err_t tsdb_seg_write_block(FILE *f, uint32_t data_offset, uint32_t block, const void *buf);
// fflush + fsync with the result reported (ESP_ERR_NO_MEM on ENOSPC).
esp_err_t tsdb_file_sync(FILE *f);
// Close and reopen the active segment after a failed write: littlefs leaves
// a file handle that failed mid-write unusable ("not safe to do anything if
// our file errored"); the reopened handle sees the last synced state.
// db->active is NULL if the reopen fails (the next write starts a segment).
void tsdb_reopen_active(tsdb_t *db);
// Column mapping: for each current logical column c, the index of the
// matching column in a segment with the given names (NULL = all empty), or -1.
void tsdb_map_columns(const tsdb_schema_t *cur, uint8_t seg_np,
                      char seg_names[][TSDB_NAME_LEN], int8_t *map /* [cur->count] */);

// --- tsdb_write.c -----------------------------------------------------------
static inline void tsdb_write_cache_invalidate(tsdb_t *db) {
    db->cached_block_num = UINT32_MAX;
    db->cache_dirty = false;
}
esp_err_t tsdb_write_cache_flush(tsdb_t *db);

// --- tsdb_core.c ------------------------------------------------------------
uint32_t tsdb_total_writes(const tsdb_t *db);
static inline uint32_t tsdb_retained(const tsdb_t *db) {
    return db->total_physical - db->head_skip;
}
// Timestamp of the oldest RETAINED record (one block read when head_skip > 0).
uint32_t tsdb_oldest_retained_ts(tsdb_t *db);

#endif // TSDB_INTERNAL_H
