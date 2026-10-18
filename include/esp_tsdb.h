/**
 * @file esp_tsdb.h
 * @brief ESP Time-Series Database Library
 *
 * Lightweight time-series database for ESP32 with configurable memory allocation,
 * columnar storage, sparse indexing, and streaming queries.
 */

#ifndef ESP_TSDB_H
#define ESP_TSDB_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

#define TSDB_MAX_PARAMS 64          // Maximum parameters supported
#define TSDB_MAGIC 0x45545344       // "ETSD" (ESP Time-Series DB) — pre-3.0 single file
#define TSDB_VERSION 3              // Pre-3.0 file format version (<=16 base params)
#define TSDB_VERSION_WIDE 4         // Pre-3.0 V4: 17-64 base params (extended names region)
#define TSDB_BLOCK_SIZE 1024        // Block size in bytes
#define TSDB_NAME_LEN 20            // Column name storage (19 chars + NUL) in v3 segments
#define TSDB_SEG_MAGIC 0x47455354   // "TSEG" — v3 segment file
#define TSDB_SEG_VERSION 1

// V4 wide-schema layout: base param names 16..63 live in an extension region
// between the fixed 584-byte header and the index (which starts at 2048).
// Only files with num_params > 16 use V4; smaller files stay V3 for
// backward compatibility. Block geometry is runtime-computed either way.
#define TSDB_V4_EXT_NAMES_OFFSET 584   // sizeof(tsdb_header_t)
#define TSDB_V4_EXT_NAME_LEN     20    // names 16..63, 20 bytes each (like overflow)
#define TSDB_V4_INDEX_OFFSET     2048  // 584 + 48*20 = 1544, rounded up
#define TSDB_OVERFLOW_MAGIC 0x4F564652  // "OVFR"
#define TSDB_MAX_EXTRA_PARAMS 48
#define TSDB_OVERFLOW_HEADER_SIZE 1024

// ============================================================================
// OPAQUE DATABASE HANDLE
// ============================================================================

/**
 * @brief Opaque database handle. Created by tsdb_open(), released by
 *        tsdb_close_h(). Multiple handles may be open simultaneously, each
 *        backed by its own file, buffer pool, and per-instance mutex.
 *
 * The legacy global API (tsdb_init / tsdb_write / tsdb_query_init / ...) is
 * preserved for backward compatibility and operates on a single internal
 * default handle. New multi-instance code should use the _h-suffixed
 * variants below.
 */
typedef struct tsdb_s tsdb_t;

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief Memory allocation strategy
 */
typedef enum {
    TSDB_ALLOC_INTERNAL_RAM,        // Use internal RAM (legacy/V1 units)
    TSDB_ALLOC_PSRAM,               // Use PSRAM (V2+ units with external RAM)
    TSDB_ALLOC_AUTO                 // Auto-detect best strategy
} tsdb_alloc_strategy_t;

/**
 * @brief Schema policy at open. Kept for source compatibility with 2.5.
 *
 * Since 3.0 nothing is ever rewritten: every segment carries its own column
 * list and columns are matched BY NAME at read time. So both values behave
 * the same — the config's `param_names` IS the schema, old data maps onto it
 * by name (reorders honoured, missing names read 0, dropped names are simply
 * not visible), and a changed list just starts a new segment on the next
 * write. The only remaining refusal is a count mismatch when `param_names`
 * is NULL, because positional mapping is then ambiguous.
 *
 * A RENAMED column cannot be told apart from drop+add by name matching —
 * call tsdb_rename_param() before changing the name in the config and the
 * history follows the column.
 */
typedef enum {
    TSDB_SCHEMA_STRICT = 0,
    TSDB_SCHEMA_MIGRATE = 1,
} tsdb_schema_policy_t;

struct tsdb_migrate_opts_s;

/**
 * @brief Database configuration
 */
typedef struct {
    const char *filepath;           // Full path (e.g., "/littlefs/data.tsdb")

    // Data structure
    uint8_t num_params;             // Number of parameters to store (1-16)
    const char **param_names;       // Parameter names (optional, can be NULL)

    // Capacity
    uint32_t max_records;           // Maximum records before LRU eviction (0 = unlimited)
    uint32_t index_stride;          // Unused since 3.0 (kept for source compatibility)

    // Memory allocation
    size_t buffer_pool_size;        // Buffer pool size in bytes (e.g., 10KB or 256KB)
    tsdb_alloc_strategy_t alloc_strategy;  // Where to allocate buffers
    bool use_paged_allocation;      // true = use paged buffers (fragmented heap)
    size_t page_size;               // Page size if using paged allocation (default: 2048)

    // Free-space guard (optional, NULL/0 = disabled — fully backward
    // compatible). When set, each time the data file is about to GROW by a
    // new block the engine calls free_space_cb(); if the result is below
    // min_free_bytes it caps max_records at the current record count and
    // switches to ring/LRU reuse immediately — instead of waiting for a hard
    // ENOSPC, by which time the filesystem is at zero bytes free and every
    // OTHER file on the medium is failing its writes too (copy-on-write
    // filesystems like littlefs need free blocks even for in-place updates).
    // The callback should return UINT64_MAX when free space cannot be
    // determined, so a failed probe never triggers a spurious cap.
    uint64_t (*free_space_cb)(void);
    uint32_t min_free_bytes;

    // See tsdb_schema_policy_t — both values reconcile by name since 3.0.
    tsdb_schema_policy_t schema_policy;
    // Unused since 3.0 (no rewrite ever happens); kept for source compatibility.
    const struct tsdb_migrate_opts_s *migrate_opts;

    // v3: data blocks per segment file, as bytes (rounded down to whole 1 KB
    // blocks). 0 = auto: the capacity split into ~32 segments, clamped to
    // [1 KB, 256 KB] per segment. Smaller segments evict in finer steps and
    // cost one more file; larger ones mean fewer files. Capacity is exact
    // either way (eviction is logical at the head, physical per segment).
    uint32_t segment_bytes;
} tsdb_config_t;

/**
 * @brief Helper macro to calculate max records for a storage size
 *
 * v3 note: records are packed into 1 KB blocks at (1016 / (4 + 2*num_params))
 * per block, and each segment carries a 1 KB header, so the on-disk footprint
 * at capacity is a few percent above records * record_size. Leave headroom.
 *
 * @param storage_bytes Total file size budget
 * @param num_params Number of parameters per record
 */
#define TSDB_CALC_MAX_RECORDS(storage_bytes, num_params) \
    (((storage_bytes) - 2048) / (4 + ((num_params) * 2)))

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize or open existing time-series database
 *
 * @param config Configuration structure
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if config is invalid
 *         ESP_ERR_NO_MEM if buffer allocation fails
 *         ESP_FAIL on file I/O errors
 */
esp_err_t tsdb_init(const tsdb_config_t *config);

/**
 * @brief Close database and flush buffers
 *
 * @return ESP_OK on success
 */
esp_err_t tsdb_close(void);

/**
 * @brief Check if database is initialized
 *
 * @return true if initialized
 */
bool tsdb_is_initialized(void);

// ============================================================================
// WRITE OPERATIONS
// ============================================================================

/**
 * @brief Write a single record
 *
 * @param timestamp Unix timestamp (seconds)
 * @param values Array of parameter values (size must match num_params)
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if not initialized
 *         ESP_ERR_INVALID_ARG if values is NULL
 */
esp_err_t tsdb_write(uint32_t timestamp, const int16_t *values);

/**
 * @brief Write multiple records in batch (more efficient)
 *
 * Takes the handle lock once and keeps the current block in RAM so
 * consecutive records that land in the same block cost ONE block write
 * instead of one per record. A crash mid-batch loses at most the records of
 * the block that had not been flushed yet; what reached disk is consistent.
 *
 * Timestamps must be ascending (same rule as tsdb_write).
 *
 * @param timestamps Array of timestamps
 * @param values 2D array [count][num_params]
 * @param count Number of records
 * @return ESP_OK on success; on error, records before the failing one are
 *         persisted.
 */
esp_err_t tsdb_write_batch(const uint32_t *timestamps,
                            const int16_t **values,
                            uint32_t count);

// ============================================================================
// INTERNAL STRUCTURES (exposed for stack allocation)
// ============================================================================

/**
 * @brief Pre-3.0 single-file header (584 bytes at file offset 0). Kept so
 *        existing databases are adopted read-only by 3.0.
 */
typedef struct {
    uint32_t magic;                 // Validation magic number (TSDB_MAGIC)
    uint32_t version;               // File format version

    // Parameter configuration
    uint8_t num_params;             // Number of parameters stored
    uint8_t param_size;             // Bytes per parameter (2 = int16_t)
    uint16_t record_size;           // Total bytes per record
    uint16_t records_per_block;     // Calculated from block size

    // Capacity and ring buffer state
    uint32_t max_records;           // Maximum records before LRU eviction
    uint32_t total_records;         // Total records ever written
    uint32_t oldest_record_idx;     // Index of oldest valid record
    uint32_t newest_record_idx;     // Index of newest record

    // Time bounds (for quick validation)
    uint32_t oldest_timestamp;      // Timestamp of oldest record
    uint32_t newest_timestamp;      // Timestamp of newest record

    // Index metadata
    uint32_t index_offset;          // Byte offset to sparse index
    uint32_t index_entries;         // Number of index entries
    uint32_t index_stride;          // Records between index entries

    // Statistics
    uint32_t total_writes;          // Lifetime write count
    uint32_t total_evictions;       // Lifetime LRU evictions

    // Parameter metadata (optional, for validation)
    char param_names[16][32];       // Human-readable parameter names

    uint8_t  base_params;               // Original num_params (drives block geometry)
    uint8_t  extra_param_count;          // Number of overflow params (0 = none)
    uint16_t overflow_record_size;       // extra_param_count * sizeof(int16_t)
    uint32_t overflow_offset;            // File offset of overflow region (0 = none)
    uint32_t first_overflow_record_idx;  // total_records when overflow was created
    uint8_t  reserved2[2];              // 2 bytes remaining (was 16 bytes reserved)
} __attribute__((packed)) tsdb_header_t;

/**
 * @brief Overflow header structure (1024 bytes, at overflow_offset)
 */
typedef struct {
    uint32_t magic;                    // 0x4F564652 "OVFR"
    uint8_t  param_count;             // Number of extra params
    uint8_t  reserved[3];
    uint32_t first_record_idx;        // Same as header.first_overflow_record_idx
    char     param_names[48][20];     // Up to 48 extra param names (960 bytes)
    uint8_t  padding[52];             // Pad to 1024 bytes total
} __attribute__((packed)) tsdb_overflow_header_t;

/**
 * @brief Data block (1024 bytes, columnar storage)
 */
typedef struct {
    uint32_t block_magic;           // 0x424C4B54 "BLKT"
    uint16_t record_count;          // Valid records in this block
    uint16_t flags;                 // Reserved flags

    // Columnar layout for selective reading
    uint32_t timestamps[38];        // Max records per block (for 10 params)

    // Parameters stored column-wise
    // Actual size determined by num_params at runtime
    int16_t params[16][38];

    // Note: Actual usable records_per_block calculated at init based on num_params
    // This structure is sized for worst case (max params)
} __attribute__((packed)) tsdb_block_t;

/**
 * @brief Runtime block access macros
 *
 * The tsdb_block_t struct is used as a memory buffer only.
 * These macros calculate correct byte offsets for the on-disk
 * columnar layout based on actual records_per_block.
 */
#define TSDB_BLOCK_HEADER_SIZE 8  // magic(4) + record_count(2) + flags(2)

// Access block fields from raw buffer
#define TSDB_BLOCK_MAGIC(buf)         (*(uint32_t*)(buf))
#define TSDB_BLOCK_COUNT(buf)         (*(uint16_t*)((uint8_t*)(buf) + 4))
#define TSDB_BLOCK_FLAGS(buf)         (*(uint16_t*)((uint8_t*)(buf) + 6))
#define TSDB_BLOCK_TS(buf, rec)       (*(uint32_t*)((uint8_t*)(buf) + 8 + (rec) * 4))
#define TSDB_BLOCK_PARAM(buf, rpb, param_idx, rec) \
    (*(int16_t*)((uint8_t*)(buf) + 8 + (rpb) * 4 + (param_idx) * (rpb) * 2 + (rec) * 2))

// ============================================================================
// QUERY OPERATIONS
// ============================================================================

/**
 * @brief Query handle for iterating through results
 *
 * Note: Callers should allocate this on the stack
 */
struct tsdb_query_s {
    tsdb_t *db;                     // Database handle this query iterates over

    // Query parameters
    uint32_t start_time;
    uint32_t end_time;
    uint8_t param_indices[TSDB_MAX_PARAMS];
    uint8_t num_params_to_fetch;
    uint32_t stride;                // every stride-th in-range record (1 = all)

    // Logical cursor: position in [0, total) over all retained records,
    // oldest first.
    uint32_t pos;
    uint32_t total;

    // Current segment (internal)
    int seg_i;                      // index into the handle's segment table, -1 = none
    int64_t seg_start;              // logical position of the segment's physical record 0
    uint32_t seg_records;
    uint32_t seg_data_offset;
    uint16_t seg_rpb;
    uint8_t seg_np;
    bool seg_legacy;                // segment is the adopted pre-3.0 file
    bool seg_identity;              // column map is the identity
    FILE *seg_file;
    bool owns_file;
    int8_t col_map[TSDB_MAX_PARAMS];   // requested column i -> segment column, -1 = absent
    bool needs_extras;              // legacy overflow extras requested

    // Block buffer (points into the handle's buffer pool or heap)
    tsdb_block_t *block_buffer;
    uint32_t cur_block;
    bool block_loaded;
    bool owns_buffer;
};

typedef struct tsdb_query_s tsdb_query_t;

/**
 * @brief Initialize a query for time range
 *
 * @param query Query handle (allocated by caller)
 * @param start_time Start timestamp (inclusive)
 * @param end_time End timestamp (inclusive)
 * @param param_indices Which parameters to fetch (NULL = all)
 * @param num_params_to_fetch Number of parameters to fetch
 * @return ESP_OK on success
 */
esp_err_t tsdb_query_init(tsdb_query_t *query,
                          uint32_t start_time,
                          uint32_t end_time,
                          const uint8_t *param_indices,
                          uint8_t num_params_to_fetch);

/**
 * @brief Fetch next record from query
 *
 * @param query Query handle
 * @param timestamp Output timestamp
 * @param values Output values (size must match num_params_to_fetch)
 * @return ESP_OK on success
 *         ESP_ERR_NOT_FOUND when no more records
 */
esp_err_t tsdb_query_next(tsdb_query_t *query,
                          uint32_t *timestamp,
                          int16_t *values);

/**
 * @brief Return only every stride-th in-range record from the query (v2.5+).
 *
 * For charts: a year of 5-minute samples is ~105k records, but a plot only
 * needs a few hundred points. With stride = 200 the iterator reads only the
 * blocks that hold the sampled records — roughly one block read per point
 * once stride >= records_per_block — instead of every block in the window.
 * The first returned record is the first in-range one; subsequent records
 * are stride positions apart. Call after tsdb_query_init(); 0 or 1 = all.
 *
 * @param query Initialised query
 * @param stride Record step (1 = every record)
 * @return ESP_OK, ESP_ERR_INVALID_ARG if query is not initialised
 */
esp_err_t tsdb_query_set_stride(tsdb_query_t *query, uint32_t stride);

/**
 * @brief Get count of records in time range without iterating
 *
 * O(log n): two binary seeks bound the window, no records are read.
 * (Falls back to a scan only if a probe hits a zero-timestamp hole.)
 *
 * @param start_time Start timestamp
 * @param end_time End timestamp
 * @param count Output count
 * @return ESP_OK on success
 */
esp_err_t tsdb_query_count(uint32_t start_time,
                           uint32_t end_time,
                           uint32_t *count);

/**
 * @brief Close query and free resources
 *
 * @param query Query handle
 */
void tsdb_query_close(tsdb_query_t *query);

// ============================================================================
// AGGREGATION OPERATIONS
// ============================================================================

/**
 * @brief Aggregation types
 */
typedef enum {
    TSDB_AGG_SUM,
    TSDB_AGG_AVG,
    TSDB_AGG_MIN,
    TSDB_AGG_MAX,
    TSDB_AGG_COUNT,
    TSDB_AGG_FIRST,
    TSDB_AGG_LAST
} tsdb_agg_type_t;

/**
 * @brief Perform aggregation over time range
 *
 * COUNT, FIRST and LAST are O(log n) (binary seeks, at most one record
 * read). SUM/AVG/MIN/MAX scan the window block by block.
 *
 * @param start_time Start timestamp
 * @param end_time End timestamp
 * @param param_index Which parameter to aggregate
 * @param agg_type Aggregation operation
 * @param result Output result (int32 for overflow safety)
 * @return ESP_OK on success
 */
esp_err_t tsdb_aggregate(uint32_t start_time,
                         uint32_t end_time,
                         uint8_t param_index,
                         tsdb_agg_type_t agg_type,
                         int32_t *result);

/**
 * @brief Result for one parameter in a multi-aggregate query
 */
typedef struct {
    uint8_t param_index;            // Which parameter
    tsdb_agg_type_t agg_type;       // Which aggregation
    int32_t result;                  // Computed result
} tsdb_agg_request_t;

/**
 * @brief Perform multiple aggregations in a single pass
 *
 * @param start_time Start timestamp
 * @param end_time End timestamp
 * @param requests Array of aggregation requests (param_index and agg_type filled in, result filled out)
 * @param num_requests Number of requests
 * @param record_count Output: number of records scanned (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t tsdb_aggregate_multi(uint32_t start_time,
                                uint32_t end_time,
                                tsdb_agg_request_t *requests,
                                uint8_t num_requests,
                                uint32_t *record_count);

// ============================================================================
// STATISTICS & MAINTENANCE
// ============================================================================

/**
 * @brief Database statistics
 */
typedef struct {
    uint32_t total_records;         // Current record count
    uint32_t max_records;           // Capacity
    uint32_t oldest_timestamp;      // Oldest record time
    uint32_t newest_timestamp;      // Newest record time
    uint32_t total_writes;          // Lifetime writes
    uint32_t total_evictions;       // Lifetime LRU evictions
    uint32_t storage_bytes;         // File size on disk
    uint8_t num_params;             // Parameters per record
    uint8_t extra_params;           // Extra overflow parameters
    size_t buffer_pool_size;        // Allocated buffer size
    bool using_paged_allocation;    // true if using paged buffers
    uint16_t segment_count;         // v3: segment files (incl. an adopted legacy file)
} tsdb_stats_t;

/**
 * @brief Get database statistics
 *
 * @param stats Output statistics structure
 * @return ESP_OK on success
 */
esp_err_t tsdb_get_stats(tsdb_stats_t *stats);

/**
 * @brief Clear all data (keeps schema and capacity, resets to empty)
 *
 * Every segment file (and an adopted legacy file) is unlinked; the space goes
 * back to the filesystem. Flat cost.
 *
 * @return ESP_OK on success
 */
esp_err_t tsdb_clear(void);

/**
 * @brief Delete database file completely
 *
 * @return ESP_OK on success
 */
esp_err_t tsdb_delete(void);

/**
 * @brief Remove every file of a database that is NOT open (path-level)
 *
 * Unlinks the legacy 2.x file and its sidecars, an orphaned .mig, and every
 * segment file belonging to @p filepath. Use this where no handle exists yet
 * (e.g. discarding a database for a different schema before the first open).
 * Removing a database that is open elsewhere is undefined.
 *
 * @param filepath Database path as given in tsdb_config_t.filepath
 * @return ESP_OK (also when nothing existed), ESP_ERR_INVALID_ARG on NULL
 */
esp_err_t tsdb_remove(const char *filepath);

/**
 * @brief On-disk size of a database without opening it (path-level)
 *
 * Sums the legacy file, its sidecars and every segment file at @p filepath.
 * Lets a caller that sizes the capacity from free space add the database's
 * own bytes back in before tsdb_open(); 3.0 applies the configured
 * max_records at every open.
 *
 * @param filepath Database path
 * @param bytes    Out: total bytes (0 when no files exist)
 * @return ESP_OK, ESP_ERR_INVALID_ARG on NULL
 */
esp_err_t tsdb_disk_usage(const char *filepath, uint64_t *bytes);

// ============================================================================
// OVERFLOW PARAMETER API
// ============================================================================

/**
 * @brief Add extra parameters at runtime
 *
 * Since 3.0 extras are ordinary columns appended to the schema: the next
 * write starts a new segment with the wider column list, and earlier records
 * read 0 for them. Writes then expect base + extra values. The extras
 * persist in the segment headers, so they survive a reboot without the config
 * knowing about them. Instant; no region, no rewrite.
 *
 * @param param_names Array of parameter name strings
 * @param count Number of extra parameters (1-48)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if extras already exist
 */
esp_err_t tsdb_add_extra_params(const char **param_names, uint8_t count);

/**
 * @brief Replace the runtime extra columns
 *
 * Since 3.0: instant schema change — the next write starts a new segment.
 * Columns kept by name keep their history; new names read 0 for older
 * records; dropped names are no longer visible.
 *
 * @param new_names Array of new param name strings (NULL if new_count==0)
 * @param new_count Number of new extra parameters (0 = remove all extras)
 * @return ESP_OK on success
 */
esp_err_t tsdb_migrate_overflow(const char **new_names, uint8_t new_count);

// ============================================================================
// SCHEMA MIGRATION (full base-column rewrite, v2.2+)
// ============================================================================

/**
 * @brief Migration progress callback.
 *
 * Invoked from the migrating task at the start (done=0), roughly every 1% of
 * records (configurable via progress_every), and once at completion
 * (done == total). Total is the record count actually being copied (after any
 * trim). Keep it fast (it runs inside the migration, handle lock held) and do
 * NOT call any tsdb_* function from it.
 */
typedef void (*tsdb_migrate_progress_cb)(uint32_t done, uint32_t total, void *ctx);

/**
 * @brief Options for tsdb_migrate_schema().
 */
typedef struct tsdb_migrate_opts_s {
    /**
     * Filesystem free space in bytes, as known by the caller (e.g. from
     * esp_littlefs_info()). The migration writes a full new copy of the
     * database before deleting the old one, so this budget decides whether
     * it fits (a ~5% reserve is subtracted internally for FS metadata).
     *
     * 0 = don't check ("I'm on an SD card / space is plentiful, just go").
     */
    uint32_t free_space_bytes;

    /**
     * If the new file doesn't fit in free_space_bytes, permit dropping the
     * OLDEST records so the newest ones fit (the ring capacity is capped to
     * what was proven to fit). If false, the migration refuses with
     * ESP_ERR_NO_MEM instead.
     */
    bool allow_trim;

    /** Optional progress reporting (NULL = none). */
    tsdb_migrate_progress_cb progress;
    void *progress_ctx;

    /**
     * Records between progress callbacks. 0 = auto (~1% of the total,
     * minimum 1). The start (0) and completion (total) calls always fire.
     */
    uint32_t progress_every;
} tsdb_migrate_opts_t;

/**
 * @brief Change the column set — MySQL-style ALTER TABLE.
 *
 * Since 3.0 this is an instant, in-RAM schema change: the next write starts
 * a new segment with the new column list, and existing segments are read
 * through a by-name column map. Columns present in both keep their data,
 * names absent from new_names are no longer visible, brand-new names read 0
 * for existing records, reorders are honoured. Any runtime extras are folded
 * into the new list (name them to keep them). Writes after this call expect
 * new_count values. Nothing is rewritten, so `opts` is ignored and no
 * progress callback fires.
 *
 * @param new_names New column names, in order (1-64)
 * @param new_count Number of new columns
 * @param opts Ignored since 3.0 (kept for source compatibility)
 * @return ESP_OK on success (also when the schema already matches)
 */
esp_err_t tsdb_migrate_schema(const char **new_names, uint8_t new_count,
                              const tsdb_migrate_opts_t *opts);

/**
 * @brief Change the capacity (max_records) of a live database.
 *
 * Since 3.0 this is instant: growing just raises the limit; shrinking below
 * the retained count evicts the oldest records (logically at once, the files
 * are unlinked as whole segments fall out of the window). Also clears any
 * capacity cap latched by ENOSPC or the free-space guard.
 *
 * Storage reality check: growing only helps when the FILESYSTEM has room.
 * On a flash partition the database was sized to fill, a bigger max_records
 * just meets ENOSPC later, at which point the oldest segment is evicted and
 * the capacity re-capped.
 *
 * @param new_max_records New capacity; 0 = unlimited (no eviction).
 * @param opts Ignored since 3.0 (kept for source compatibility)
 * @return ESP_OK
 */
esp_err_t tsdb_resize(uint32_t new_max_records, const tsdb_migrate_opts_t *opts);

/**
 * @brief Get total parameter count (base + extra)
 */
uint8_t tsdb_get_total_params(void);

/**
 * @brief Get parameter name by index (base or extra)
 */
const char* tsdb_get_param_name(uint8_t index);

/**
 * @brief Check if overflow region is active
 */
bool tsdb_has_overflow(void);

/**
 * @brief Rename a column in place (v2.5+). No data is touched: the old name
 *        is kept as an alias so earlier segments still map to the column.
 *        Call this BEFORE changing the name in your config so the history
 *        follows the column (a config change alone looks like drop + add).
 *
 * @param index Column index (0 .. total params - 1)
 * @param new_name New name (19 chars max)
 * @return ESP_OK, ESP_ERR_INVALID_ARG on bad index/name
 */
esp_err_t tsdb_rename_param(uint8_t index, const char *new_name);

/**
 * @brief Find a column by name (base, then overflow extras). v2.5+
 *
 * @return Column index, or -1 if not found.
 */
int tsdb_find_param(const char *name);

// ============================================================================
// BENCHMARK
// ============================================================================

/**
 * @brief Benchmark results
 */
typedef struct {
    // Write performance
    uint32_t writes_count;
    uint32_t write_total_us;
    uint32_t write_min_us;
    uint32_t write_max_us;
    float    write_avg_us;

    // Full query (all params)
    uint32_t query_records;
    uint32_t query_total_us;
    float    query_per_record_us;

    // Single param query
    uint32_t single_param_records;
    uint32_t single_param_total_us;
    float    single_param_per_record_us;

    // Aggregation (single)
    uint32_t agg_records;
    uint32_t agg_total_us;

    // Multi-aggregation
    uint32_t multi_agg_records;
    uint32_t multi_agg_total_us;

    // Overflow write
    uint32_t overflow_writes_count;
    uint32_t overflow_write_total_us;
    float    overflow_write_avg_us;

    // Overflow query
    uint32_t overflow_query_records;
    uint32_t overflow_query_total_us;
    float    overflow_query_per_record_us;

    // Metadata
    uint32_t file_size_bytes;
    uint8_t  num_params;
    uint8_t  extra_params;
} tsdb_benchmark_results_t;

/**
 * @brief Run on-device benchmark
 *
 * Creates a temporary database, writes records, queries them, and measures
 * timing for all operations. Cleans up after itself.
 *
 * WARNING: Closes any currently open TSDB. Caller must re-init after.
 *
 * @param num_records Number of records to write (e.g., 500, 1000)
 * @param num_params Number of base params (1-16)
 * @param results Output benchmark results
 * @return ESP_OK on success
 */
esp_err_t tsdb_run_benchmark(uint32_t num_records, uint8_t num_params,
                              tsdb_benchmark_results_t *results);

// ============================================================================
// HANDLE-BASED API (multi-instance, v2.1+)
// ============================================================================
//
// Every legacy global function has an _h-suffixed counterpart that takes a
// tsdb_t handle as its first argument. Use these when you need more than
// one database open simultaneously (e.g., one for system metrics, one for
// per-panel telemetry). All functions are safe to call from multiple tasks
// against the same handle — each handle owns a FreeRTOS mutex internally.

/**
 * @brief Open a new database handle.
 *
 * Creates or opens the file at config->filepath, allocates the buffer pool,
 * and initialises the per-instance mutex. The returned handle is independent
 * of any other handle (including the default one used by the legacy API).
 *
 * @param config Configuration (same fields as tsdb_init).
 * @return Handle on success, NULL on failure (check the log for details).
 */
tsdb_t *tsdb_open(const tsdb_config_t *config);

/**
 * @brief Close a handle, flush buffers, free resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param db Handle returned from tsdb_open(). Must not be reused after.
 * @return ESP_OK on success.
 */
esp_err_t tsdb_close_h(tsdb_t *db);

/**
 * @brief Force a directory-entry commit by close+reopen of the active segment.
 *
 * Every write already fsyncs the active segment. On esp_littlefs the 2.4.0
 * power-pull test showed fsync alone is durable for an open file, but if you
 * want belt and braces before a planned reboot, this closes and reopens the
 * active segment file (one filesystem round-trip, ~30-50 ms). The handle and
 * all state survive.
 *
 * Returns ESP_ERR_INVALID_STATE if db is null or not open, ESP_FAIL if the
 * reopen fails (the handle is then closed).
 */
esp_err_t tsdb_sync_h(tsdb_t *db);

bool tsdb_is_initialized_h(const tsdb_t *db);

esp_err_t tsdb_write_h(tsdb_t *db, uint32_t timestamp, const int16_t *values);

esp_err_t tsdb_write_batch_h(tsdb_t *db,
                             const uint32_t *timestamps,
                             const int16_t **values,
                             uint32_t count);

esp_err_t tsdb_query_init_h(tsdb_t *db,
                            tsdb_query_t *query,
                            uint32_t start_time,
                            uint32_t end_time,
                            const uint8_t *param_indices,
                            uint8_t num_params_to_fetch);

// tsdb_query_next() and tsdb_query_close() use the handle stored inside the
// tsdb_query_t itself — no _h variant needed.

esp_err_t tsdb_query_count_h(tsdb_t *db,
                             uint32_t start_time,
                             uint32_t end_time,
                             uint32_t *count);

esp_err_t tsdb_aggregate_h(tsdb_t *db,
                           uint32_t start_time,
                           uint32_t end_time,
                           uint8_t param_index,
                           tsdb_agg_type_t agg_type,
                           int32_t *result);

esp_err_t tsdb_aggregate_multi_h(tsdb_t *db,
                                 uint32_t start_time,
                                 uint32_t end_time,
                                 tsdb_agg_request_t *requests,
                                 uint8_t num_requests,
                                 uint32_t *record_count);

esp_err_t tsdb_get_stats_h(tsdb_t *db, tsdb_stats_t *stats);

esp_err_t tsdb_clear_h(tsdb_t *db);

esp_err_t tsdb_delete_h(tsdb_t *db);

esp_err_t tsdb_add_extra_params_h(tsdb_t *db, const char **param_names, uint8_t count);

esp_err_t tsdb_migrate_overflow_h(tsdb_t *db, const char **new_names, uint8_t new_count);

esp_err_t tsdb_migrate_schema_h(tsdb_t *db, const char **new_names,
                                uint8_t new_count,
                                const tsdb_migrate_opts_t *opts);

esp_err_t tsdb_resize_h(tsdb_t *db, uint32_t new_max_records,
                        const tsdb_migrate_opts_t *opts);

uint8_t tsdb_get_total_params_h(const tsdb_t *db);

const char *tsdb_get_param_name_h(const tsdb_t *db, uint8_t index);

bool tsdb_has_overflow_h(const tsdb_t *db);

esp_err_t tsdb_rename_param_h(tsdb_t *db, uint8_t index, const char *new_name);

int tsdb_find_param_h(tsdb_t *db, const char *name);

#ifdef __cplusplus
}
#endif

#endif // ESP_TSDB_H
