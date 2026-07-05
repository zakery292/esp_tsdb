# ESP Time-Series Database (esp_tsdb)

Lightweight, data-agnostic time-series database component for ESP32 with configurable memory allocation, columnar storage, sparse indexing, streaming queries, and runtime-extensible parameters.

## Features

- **Data-Agnostic Engine**: No domain knowledge — just N columns of `int16_t` + timestamps
- **Runtime Extra Parameters**: Add overflow columns at runtime without recreating the database
- **Configurable Memory Allocation**: Choose buffer size and allocation strategy (internal RAM vs PSRAM)
- **Paged Buffer Support**: Allocate from fragmented heap on legacy units with limited contiguous memory
- **Ring Buffer with LRU Eviction**: Automatically overwrites oldest data when capacity is reached
- **Columnar Storage**: Efficient selective parameter reading
- **Sparse Time Index**: Fast timestamp-based lookups using binary search
- **Streaming Queries**: Zero-copy queries where possible, no result set accumulation
- **Aggregations**: Built-in SUM, AVG, MIN, MAX, COUNT, FIRST, LAST operations
- **Multi-Instance (v2.1+)**: Handle-based API — open several independent databases simultaneously
- **Thread-Safe (v2.1+)**: Every handle owns a FreeRTOS mutex; all operations are safe to call from multiple tasks
- **Backward Compatible**: V1 files open seamlessly under the V2 engine; the original global API is unchanged

## Quick Start

### 1. Include in Your Project

Add to your component's `CMakeLists.txt`:

```cmake
idf_component_register(
    ...
    REQUIRES esp_tsdb
)
```

### 2. Basic Usage

```c
#include "esp_tsdb.h"

const char *param_names[] = {"SOC", "Vbat", "PpvTotal", "Pload", "Pcharge"};

tsdb_config_t config = {
    .filepath = "/littlefs/solar.tsdb",
    .num_params = 5,
    .param_names = param_names,
    .max_records = TSDB_CALC_MAX_RECORDS(2000000, 5),  // ~2MB file
    .index_stride = 380,
    .buffer_pool_size = 10 * 1024,                     // 10KB buffer
    .alloc_strategy = TSDB_ALLOC_INTERNAL_RAM,
    .use_paged_allocation = true,
    .page_size = 2048
};

ESP_ERROR_CHECK(tsdb_init(&config));
```

### 3. Write Data

```c
int16_t values[] = {85, 4850, 2500, 1200, 800};
tsdb_write(time(NULL), values);
```

### 4. Query Data

```c
uint32_t now = time(NULL);
uint32_t day_ago = now - 86400;

tsdb_query_t query;
tsdb_query_init(&query, day_ago, now, NULL, 0);  // NULL = fetch all params

uint32_t timestamp;
int16_t values[5];

while (tsdb_query_next(&query, &timestamp, values) == ESP_OK) {
    printf("Time: %lu, SOC: %d, Vbat: %d\n", timestamp, values[0], values[1]);
}

tsdb_query_close(&query);
```

### 5. Aggregations

```c
uint32_t week_ago = time(NULL) - (7 * 86400);
int32_t total_kwh;

tsdb_aggregate(week_ago, now, 2, TSDB_AGG_SUM, &total_kwh);
printf("Total energy this week: %ld Wh\n", total_kwh);
```

## Runtime Extra Parameters (Overflow Region)

The database supports adding extra columns at runtime without recreating existing data. Base parameters define the block geometry; extra parameters are stored in an overflow region appended to the end of the file.

### How It Works

```
┌─────────────────────────────┐
│ Header (512 bytes)          │  ← base_params drives block geometry
│   overflow_offset ──────────│──┐
├─────────────────────────────┤  │
│ Sparse Time Index           │  │
├─────────────────────────────┤  │
│ Data Blocks (base params)   │  │  Ring buffer, columnar storage
│   ...                       │  │
├─────────────────────────────┤  │
│ Overflow Header (1024 bytes)│←─┘  Magic 0x4F564652, param names
├─────────────────────────────┤
│ Overflow Data (sequential)  │     int16_t[extra_count] per record
│   ...                       │     Indexed by (record_idx - first_overflow_record_idx)
└─────────────────────────────┘
```

### Adding Extra Parameters

```c
// Initialize with 5 base parameters
ESP_ERROR_CHECK(tsdb_init(&config));

// Later, add 3 extra parameters (creates overflow region at end of file)
const char *extras[] = {"Tbat", "Tinner", "GenPower"};
ESP_ERROR_CHECK(tsdb_add_extra_params(extras, 3));

// Now writes expect 8 values (5 base + 3 extra)
int16_t values[8] = {85, 4850, 2500, 1200, 800, 25, 40, 0};
tsdb_write(time(NULL), values);
```

### Querying Extra Parameters

```c
// Query all params (base + extra) — NULL fetches everything
tsdb_query_init(&query, start, end, NULL, 0);

// Or query specific extra params by index
uint8_t params[] = {0, 5, 6};  // SOC, Tbat, Tinner
tsdb_query_init(&query, start, end, params, 3);

// Records that predate the overflow return 0 for extra params
```

### Introspection

```c
uint8_t total = tsdb_get_total_params();      // base + extra
bool has_ovf = tsdb_has_overflow();            // true if overflow active
const char *name = tsdb_get_param_name(5);     // "Tbat" (reads from overflow header)
```

### Full Schema Migration (v2.2+)

`tsdb_migrate_schema()` changes the **base** column set itself — add, drop,
reorder, or promote overflow extras to first-class columns — via a streaming
rewrite into a new file with the new block geometry (constant ~2KB memory,
crash-safe, atomic swap):

```c
// Old schema: 3 base [soc, vbat, pload] + 2 overflow extras [tbat, gen].
// New schema: keep soc, promote both extras, add a fresh zero-filled column.
const char *new_cols[] = {"soc", "tbat", "gen", "site"};

static void on_progress(uint32_t done, uint32_t total, void *ctx) {
    // start (0), ~every 1% (configurable), completion (total). Runs on the
    // migrating task with the lock held: be quick, no tsdb_* calls.
}

tsdb_migrate_opts_t opts = {
    .free_space_bytes = fs_free,   // from esp_littlefs_info(); 0 = "on SD, just go"
    .allow_trim = true,            // drop oldest records if the copy won't fit
    .progress = on_progress,       // optional: drive a UI progress bar
};
tsdb_migrate_schema(new_cols, 4, &opts);
// Overflow region is folded away; vbat/pload dropped; "site" reads 0 for
// existing records. Writes/queries during the migration fail fast with
// ESP_ERR_INVALID_STATE — retry on the next cycle.
```

On littlefs the calling task performs flash-bus operations and must have an
internal-RAM stack (ESP32-S3 PSRAM-stack rule); SD-card databases can be
migrated from any task.

### Overflow Schema Migration

Extra parameters can be changed at any time using `tsdb_migrate_overflow()`:

```c
// Change from [Tbat, Tinner] to [Tbat, MaxCellVolt, GenPower]
const char *new_extras[] = {"Tbat", "MaxCellVolt", "GenPower"};
tsdb_migrate_overflow(new_extras, 3);
// Tbat data is preserved, MaxCellVolt/GenPower backfilled with zeros, Tinner dropped

// Remove all extras
tsdb_migrate_overflow(NULL, 0);
```

Migration is crash-safe: new data is written first, header updated last. If interrupted, the old layout remains intact and migration can be retried.

After migration, the old overflow region becomes dead space in the file. Call `tsdb_clear()` or `tsdb_delete()` to reclaim it if needed.

### Constraints

- Maximum 48 extra parameters (up to 64 total).
- Overflow data is sequential (not ring-buffered) — it grows as new records are written.
- Extra param names are stored in the overflow header (20 chars max each).
- Each migration leaves dead space from the previous overflow (~2 bytes x extras x records). Acceptable for occasional schema changes.

### Backward Compatibility

| File Version | Behavior |
|---|---|
| V1 (no overflow) | Opens normally. `base_params=0` treated as `base_params=num_params`. |
| V2, no overflow configured | `extra_param_count=0`, works identically to V1. |
| V2, with overflow | Extra params read from overflow region. Pre-overflow records return 0. |

## Multiple Databases (Handle-Based API, v2.1+)

Every global function has an `_h`-suffixed counterpart taking a `tsdb_t *` handle as its first argument, so several independent databases can be open at once (e.g. system metrics in one file, per-panel telemetry in another). The legacy global API still works and is now a thin wrapper over an internal default handle.

```c
tsdb_config_t sys_cfg  = { .filepath = "/littlefs/system.tsdb",  /* ... */ };
tsdb_config_t panel_cfg = { .filepath = "/littlefs/panels.tsdb", /* ... */ };

tsdb_t *sys_db   = tsdb_open(&sys_cfg);
tsdb_t *panel_db = tsdb_open(&panel_cfg);

tsdb_write_h(sys_db,   time(NULL), sys_values);
tsdb_write_h(panel_db, time(NULL), panel_values);

tsdb_query_t query;
tsdb_query_init_h(panel_db, &query, day_ago, now, NULL, 0);
while (tsdb_query_next(&query, &ts, values) == ESP_OK) { /* ... */ }
tsdb_query_close(&query);   // query_next/close use the handle stored in the query

tsdb_close_h(panel_db);
tsdb_close_h(sys_db);
```

Each handle owns a FreeRTOS mutex, so all operations — including on the legacy global API — are safe to call concurrently from multiple tasks.

### Durability on LittleFS: `tsdb_sync_h()`

On esp_littlefs, a file held open across many writes never commits its directory entry — only `fclose` publishes it, so an unexpected reboot can lose the whole file. `tsdb_sync_h(db)` forces the commit with a close+reopen cycle (~30-50 ms) while keeping the handle and all state valid. Call it after each write on slow snapshot cadences, or every N writes on fast ones.

## Streaming to HTTP/WebSocket

```c
esp_err_t history_handler(httpd_req_t *req) {
    tsdb_query_t query;
    tsdb_query_init(&query, start_time, end_time, NULL, 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    uint32_t ts;
    int16_t values[10];
    bool first = true;

    while (tsdb_query_next(&query, &ts, values) == ESP_OK) {
        char json[256];
        snprintf(json, sizeof(json),
            "%s{\"ts\":%u,\"soc\":%d,\"vbat\":%d}",
            first ? "" : ",", ts, values[0], values[1]);

        httpd_resp_send_chunk(req, json, strlen(json));
        first = false;
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, NULL, 0);

    tsdb_query_close(&query);
    return ESP_OK;
}
```

## Performance

Benchmarked on ESP32-C6 (single-core RISC-V, 160MHz) with 4MB NOR flash, LittleFS filesystem, 14 parameters, 10KB paged buffer pool. Measured **under full production load** (WiFi, 2x MQTT, WebSocket, inverter Modbus polling, TCP socket — all running concurrently).

| Operation | Latency | Throughput | Notes |
|---|---|---|---|
| **Write** | 290ms avg (99ms min, 750ms max) | ~3.5 writes/sec | Flash-bound: includes fflush + fsync for crash safety. Contends with other flash I/O under load. |
| **Full query (14 params)** | 64 us/record | ~15,600 records/sec | Columnar block read into RAM, then iterate. |
| **Single param query** | 60 us/record | ~16,600 records/sec | Same block I/O, fewer columns extracted. |
| **Aggregation (single)** | 30ms / 469 records | ~15,600 records/sec | Single-pass AVG over full dataset. |
| **Multi-agg (8 aggs)** | 30ms / 469 records | ~15,600 records/sec | 8 aggregations in one pass, same speed. |

**What this means in practice:**
- 1 week of 5-min data (2,016 records): **~130ms** to query
- 1 month (8,640 records): **~550ms**
- 1 year (105,120 records): **~6.7 seconds**

Write latency is dominated by LittleFS flash operations (sector erase/program). The 750ms max is a LittleFS block compaction. Under lighter load (fewer concurrent flash operations), writes drop to ~50-100ms. Read performance is largely RAM-bound and minimally affected by system load.

A built-in benchmark endpoint (`GET /api/history/benchmark?records=500&params=14`) runs on-device and returns JSON timing data.

## Memory Usage

### ESP32-C6 (4MB flash, no PSRAM)
- **Buffer pool**: 10KB (paged allocation from fragmented heap)
- **Stack usage**: ~500 bytes per query
- **Heap overhead**: ~50 bytes

### ESP32-S3 (16MB flash, 16MB PSRAM)
- **Buffer pool**: 256KB (contiguous PSRAM allocation)
- **Stack usage**: ~500 bytes per query
- **Heap overhead**: ~50 bytes

## Storage Calculations

```c
// Records per block depends on num_params (base only — extras don't affect blocks)
// Block size = 1024 bytes
// Overhead = 8 bytes (magic + count + flags)
// Per record = 4 bytes (timestamp) + (num_params * 2 bytes)

// Example: 10 base parameters
// Per record = 4 + (10 * 2) = 24 bytes
// Records per block = (1024 - 8) / 24 = 42 records/block

// Overflow adds: extra_count * 2 bytes per record (sequential, after all blocks)
// Example: 10 base + 5 extra, 75000 records, 50000 post-overflow
// Base data  = 75000 * 24 = 1.8 MB
// Overflow   = 50000 * 10 = 0.5 MB  (5 extras * 2 bytes * 50K records)
// Index      = (75000 / 380) * 8 = 1.5 KB
// Total      = ~2.3 MB
```

Use the helper macro for base storage:

```c
uint32_t max_records = TSDB_CALC_MAX_RECORDS(2000000, 10);  // 2MB storage, 10 params
```

## File Format

```
┌─────────────────────────────────────────────────┐
│ Header (512 bytes)                              │
│   - Magic (0x45545344), version (2)             │
│   - Base params, ring buffer state, time bounds │
│   - Overflow metadata (offset, extra count)     │
│   - Parameter names (base, up to 16)            │
├─────────────────────────────────────────────────┤
│ Sparse Time Index (variable size)              │
│   - Binary searchable timestamp->block mapping  │
│   - ~8 bytes per entry                          │
├─────────────────────────────────────────────────┤
│ Data Blocks (1024 bytes each)                  │
│   - Columnar storage for base params            │
│   - Ring buffer with LRU eviction              │
├─────────────────────────────────────────────────┤
│ Overflow Header (1024 bytes) [optional]        │
│   - Magic (0x4F564652), extra param names       │
│   - first_record_idx anchor                     │
├─────────────────────────────────────────────────┤
│ Overflow Data (sequential) [optional]          │
│   - int16_t[extra_count] per new record        │
└─────────────────────────────────────────────────┘
```

## API Reference

See `include/esp_tsdb.h` for full API documentation.

### Core Functions

- `tsdb_init()` - Initialize or open existing database
- `tsdb_close()` - Close and flush
- `tsdb_write()` - Write single record (base + extra values)
- `tsdb_write_batch()` - Write multiple records

### Overflow Functions

- `tsdb_add_extra_params()` - Add extra parameters (creates overflow region)
- `tsdb_get_total_params()` - Get total parameter count (base + extra)
- `tsdb_get_param_name()` - Get parameter name by index
- `tsdb_has_overflow()` - Check if overflow region is active

### Query Functions

- `tsdb_query_init()` - Initialize query iterator (fetches base + extra automatically)
- `tsdb_query_next()` - Get next record (returns 0 for extras on pre-overflow records)
- `tsdb_query_close()` - Close query
- `tsdb_query_count()` - Count records in range

### Aggregation Functions

- `tsdb_aggregate()` - Compute aggregation over time range (works on base + extra indices)
- `tsdb_aggregate_multi()` - Multiple aggregations in a single pass
  - `TSDB_AGG_SUM`, `TSDB_AGG_AVG`, `TSDB_AGG_MIN`, `TSDB_AGG_MAX`
  - `TSDB_AGG_COUNT`, `TSDB_AGG_FIRST`, `TSDB_AGG_LAST`

### Utility Functions

- `tsdb_get_stats()` - Get database statistics (includes `extra_params` count)
- `tsdb_clear()` - Clear all data (resets overflow index)
- `tsdb_delete()` - Delete database file

### Handle-Based API (v2.1+)

- `tsdb_open()` / `tsdb_close_h()` - Open/close an independent database instance
- `tsdb_sync_h()` - Force a LittleFS directory-entry commit (durability across reboots)
- `_h` variants of every function above: `tsdb_write_h()`, `tsdb_query_init_h()`, `tsdb_aggregate_h()`, `tsdb_get_stats_h()`, `tsdb_add_extra_params_h()`, `tsdb_migrate_overflow_h()`, etc.
- `tsdb_query_next()` / `tsdb_query_close()` need no `_h` variant — the query carries its handle

## License

MIT License
