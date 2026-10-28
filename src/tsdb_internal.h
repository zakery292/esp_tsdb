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

// Kinds of segment-table entry. Entries are kept in time order by
// tsdb_seg_order(): everything that came from the adopted pre-3.0 file sorts
// by its legacy rank (0 = its oldest retained record), before every 3.0
// segment, which sorts by seq.
#define TSDB_SEG_NORMAL 0   // <path>.sNNNNNNNNNN, seq 1..
#define TSDB_SEG_LEGACY 1   // a rank interval still held by the 2.x file
#define TSDB_SEG_CONV   2   // <path>.cNNNNNNNNNN: records converted from it, N = rank

/**
 * @brief In-RAM segment table entry.
 */
typedef struct {
    uint32_t seq;               // NORMAL: file sequence; CONV: = rank; LEGACY: 0
    uint32_t records;           // valid physical records
    uint32_t first_ts;          // timestamp of physical record 0
    uint32_t last_ts;           // timestamp of the newest record
    uint32_t blocks_cap;
    uint32_t data_offset;
    uint32_t schema_gen;
    uint32_t lifetime_base;
    uint32_t rank;              // LEGACY / CONV: legacy rank of physical record 0
    uint16_t rpb;
    uint8_t  np;
    uint8_t  kind;              // TSDB_SEG_*
} tsdb_seg_t;

static inline uint64_t tsdb_seg_order(const tsdb_seg_t *s) {
    return s->kind == TSDB_SEG_NORMAL ? (1ull << 32) + s->seq : (uint64_t) s->rank;
}

// ============================================================================
// LEGACY (pre-3.0 single file) — tsdb_legacy.c, conversion — tsdb_convert.c
// ============================================================================

// Conversion of the adopted file into segments (tsdb_convert.c). Littlefs can
// only shrink a file from its end, cheaply: each round copies the live
// records held in the file's physical tail into new segments, syncs them,
// then truncates the tail away — one segment of extra space at most.
enum {
    TSDB_CONV_SCAN = 0,     // header unreadable: reconstructing it from the blocks
    TSDB_CONV_START,        // persist the adopted header (<path>.cv)
    TSDB_CONV_SIDECARS,     // then drop the 2.x sidecars (redundant from here on)
    TSDB_CONV_PLAN,         // pick the next tail cut and the ranks to copy before it
    TSDB_CONV_COPY,         // writing the converted segment(s)
    TSDB_CONV_TRUNC,        // cut the tail
    TSDB_CONV_FINAL,        // nothing live left: remove the file and its marker
};

#define TSDB_CONV_MAX_IV 8

typedef struct {
    uint8_t  phase;
    bool     stalled;                    // cannot proceed (no truncate support): the file stays as it is
    uint32_t chunk_cap;                  // legacy blocks per round when space is short (0 = a segment's worth)
    uint32_t backoff;                    // out of space: writes to wait before trying again (0 = go)
    uint32_t target;                     // length the file is cut to (PLAN..TRUNC)
    bool     final_cut;                  // target leaves no records: remove instead of truncating
    uint8_t  niv, cur;                   // rank intervals to copy, the one being written
    uint32_t iv[TSDB_CONV_MAX_IV][2];    // [first rank, end rank)
    tsdb_seg_t done[TSDB_CONV_MAX_IV];   // entries of the completed ones
    FILE    *out;                        // converted segment being written
    uint32_t written;                    // records of iv[cur] on disk
    uint32_t out_rpb, out_data_offset;
    // Deferred header reconstruction (SCAN)
    uint32_t scan_next, scan_blocks;
    uint32_t scan_oldest_ts, scan_newest_ts, scan_oldest_idx, scan_newest_idx, scan_total;
} tsdb_conv_t;

typedef struct {
    FILE *file;                 // read-only; NULL only transiently around a truncate
    char path[128];
    tsdb_header_t header;       // as adopted (marker, sidecars, reconstruction)
    bool unlimited;
    uint32_t available;         // retained records (ranks 0..available-1)
    uint32_t first_slot;        // ring slot of rank 0
    // Overflow extras (sequential region past the ring)
    uint8_t  extra_count;
    uint32_t overflow_data_offset;
    uint32_t first_overflow_record_idx;
    uint16_t overflow_record_size;
    char   (*extra_names)[TSDB_NAME_LEN];   // overflow column names (cached: the region is cut early)
    // Layout
    bool     v2_layout;         // pre-2.0.1 block layout: transcoded on every read
    uint8_t *v2_tmp;            // its scratch block
    uint32_t data_offset;       // first ring block
    uint32_t length;            // physical length now (shrinks as it is converted)
    bool     marker;            // the adopted header is persisted in <path>.cv
    bool     needs_scan;        // header unreadable and not yet reconstructed
    bool     reconstructed;     // header rebuilt from the blocks (names from the config)
    tsdb_conv_t conv;
} tsdb_legacy_t;

// Opens <path> if it is a pre-3.0 database, read-only and cheaply (nothing in
// it is read beyond its header, sidecars and conversion marker; a header
// that must be reconstructed is left for the conversion steps).
// ESP_ERR_NOT_FOUND if absent (orphaned sidecars / marker are removed);
// ESP_ERR_INVALID_RESPONSE if present but unusable (it is then removed so the
// next open is clean); any other error (out of memory, a file present but
// unreadable now) leaves every file exactly as it was — the caller runs
// without it and retries later.
esp_err_t tsdb_legacy_open(const char *path, const tsdb_config_t *config, tsdb_legacy_t **out);
void tsdb_legacy_close(tsdb_legacy_t *l);
// Removes the file, its sidecars (<path>.h0/.h1, <path>.d/), any .mig
// leftover and the conversion marker, in an order a power cut cannot turn
// into orphans or a stale adoption.
void tsdb_legacy_remove(const char *path);
uint8_t tsdb_legacy_columns(const tsdb_legacy_t *l);
// Column name (base, V4 extended or overflow) into out[TSDB_NAME_LEN].
void tsdb_legacy_column_name(tsdb_legacy_t *l, uint8_t idx, char *out);
// Rank r (0 = oldest retained) -> ring block and offset in that block.
void tsdb_legacy_locate(const tsdb_legacy_t *l, uint32_t r, uint32_t *block, uint16_t *off);
// Ring block into buf, in the 3.x block layout (V2 blocks are transcoded).
esp_err_t tsdb_legacy_read_block(tsdb_legacy_t *l, uint32_t block, void *buf);
// Longest run of consecutive ranks starting at r that share a block.
uint32_t tsdb_legacy_run(const tsdb_legacy_t *l, uint32_t r);
// Overflow extras for rank r (zeros when none / pre-overflow).
void tsdb_legacy_read_extras(tsdb_legacy_t *l, uint32_t r, int16_t *out);
// Overflow extras for n consecutive ranks from r, row-major [n][extra_count].
void tsdb_legacy_read_extras_n(tsdb_legacy_t *l, uint32_t r, uint32_t n, int16_t *out);
// Timestamp of rank r (one block read into buf).
uint32_t tsdb_legacy_ts_at(tsdb_legacy_t *l, uint32_t r, void *buf);
// The rank intervals every byte of which lies below `len` (at most two):
// what the file still holds once cut to that length.
uint8_t tsdb_legacy_present(const tsdb_legacy_t *l, uint32_t len, uint32_t iv[2][2]);
// Total writes the 2.x database had made before 3.0 (lifetime numbering).
uint32_t tsdb_legacy_lifetime_end(const tsdb_legacy_t *l);
// Persist the adopted header (and overflow names) in <path>.cv.
esp_err_t tsdb_legacy_write_marker(tsdb_legacy_t *l);
// One bounded slice of the header reconstruction scan; true when finished
// (the header, available and first_slot are then set).
bool tsdb_legacy_scan_step(tsdb_legacy_t *l, uint32_t max_blocks, void *buf);
// Remove sidecars (subdirectory slots first) — no-ops when absent.
void tsdb_legacy_remove_sidecars(const char *path);
// Remove the first sidecar still present (same order); false when none is.
bool tsdb_legacy_remove_one_sidecar(const char *path);
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

    tsdb_legacy_t *legacy;          // the adopted 2.x file while it exists (see tsdb_convert.c)
    // A 2.x file is present but could not be adopted at open for a transient
    // reason (out of memory): the handle runs without its history, and never
    // deletes or converts it, until a later attempt adopts it.
    bool legacy_retry;

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

// With the lock held: a query open on this handle means the caller is the
// query's own task (any other task would be blocked on the lock, which the
// query holds from init to close). Writes, conversion steps, clear, delete
// and schema changes close, truncate or unlink files the query may be reading
// through and shift the segment table under its cursor, so they refuse:
// returns true after logging and releasing the lock.
bool tsdb_refuse_if_query_open(tsdb_t *db, const char *what);

// ============================================================================
// INTERNAL FUNCTIONS
// ============================================================================

// --- tsdb_segment.c ---------------------------------------------------------
void tsdb_seg_path(const tsdb_t *db, uint32_t seq, char *out, size_t out_len);
// File of a NORMAL or CONV table entry.
void tsdb_entry_path(const tsdb_t *db, const tsdb_seg_t *seg, char *out, size_t out_len);
void tsdb_conv_path(const tsdb_t *db, uint32_t rank, char *out, size_t out_len);
// Scan the directory for segment files (.s and converted .c), validate each
// (header + tail block), fill db->segs. Empty/invalid segments are removed.
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
// Drop segs[0] from the table and unlink its file. false if it is the active
// segment (never evicted physically) or the table is empty. A LEGACY piece
// has no file of its own: it is dropped (the conversion skips its ranks and
// removes the 2.x file once no live piece is left) — unless `whole_legacy`,
// for the out-of-space paths, which remove the whole 2.x file at once.
bool tsdb_evict_oldest(tsdb_t *db, bool whole_legacy);
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

// --- tsdb_convert.c ---------------------------------------------------------
// After the segment scan: the 2.x file's present ranks, less those already in
// converted segments, become LEGACY table entries (cheap: a few block reads
// for their time bounds).
esp_err_t tsdb_conv_adopt(tsdb_t *db);
// One bounded step of adoption retry / reconstruction / conversion (lock held).
esp_err_t tsdb_conv_step(tsdb_t *db);
static inline bool tsdb_conv_pending(const tsdb_t *db) {
    return db->legacy != NULL || db->legacy_retry;
}
// Stop a conversion and remove the whole 2.x file (clear, out of space).
void tsdb_conv_drop_legacy(tsdb_t *db);
// Out of space elsewhere: give back the space of a round in progress (its
// copies are discarded; the 2.x file still holds them) and back off. true
// if there was one.
bool tsdb_conv_yield_space(tsdb_t *db);
// Space has plausibly changed (a write, a file evicted): let a backed-off
// conversion try again sooner.
void tsdb_conv_space_event(tsdb_t *db, bool freed);
// Close (and unlink) a converted segment being written.
void tsdb_conv_abort_chunk(tsdb_t *db);
// Scratch block for internal reads: the pool's read buffer when contiguous,
// else *heap (caller frees). NULL on out of memory.
void *tsdb_scratch_block(tsdb_t *db, void **heap);

// --- tsdb_core.c ------------------------------------------------------------
uint32_t tsdb_total_writes(const tsdb_t *db);
static inline uint32_t tsdb_retained(const tsdb_t *db) {
    return db->total_physical - db->head_skip;
}
// Timestamp of the oldest RETAINED record (one block read when head_skip > 0).
uint32_t tsdb_oldest_retained_ts(tsdb_t *db);

#endif // TSDB_INTERNAL_H
