# ESP Time-Series Database (esp_tsdb)

Lightweight, data-agnostic time-series database component for ESP32: append-only segment files, columnar blocks, by-name schema evolution, streaming queries and built-in aggregations, with configurable memory allocation.

## Features

- **Data-Agnostic Engine**: No domain knowledge — just N columns of `int16_t` + timestamps
- **Append-Only Segments (v3.0+)**: Writes only ever append to the newest small file; old data is evicted by deleting whole files. No in-place rewrites, so LittleFS write cost stays flat for the life of the device
- **Exact Capacity**: `max_records` is honoured to the record; eviction is logical at the head and physical per segment
- **Schema Evolution Without Rewrites (v3.0+)**: Change, add, drop, reorder or rename columns at any time; every segment carries its own column list and readers map by name
- **Zero-Copy Upgrade**: A 1.x/2.x single-file database is adopted in place as the oldest segment
- **Crash-Safe by Construction**: No header to keep in sync — a segment's record count is derived from its content on open
- **Chart-Friendly Queries**: O(log n) seeks, counts and FIRST/LAST; stride sampling; block-level aggregation
- **Real Batch Writes**: One block write per block touched
- **Configurable Memory Allocation**: Buffer pool in internal RAM or PSRAM, contiguous or paged
- **Multi-Instance, Thread-Safe**: Handle-based API; every handle owns a FreeRTOS mutex

## Quick Start

### 1. Include in Your Project

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
    .param_names = param_names,                        // the schema — match by name
    .max_records = TSDB_CALC_MAX_RECORDS(2000000, 5),  // ~2MB of records
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

`COUNT`, `FIRST` and `LAST` (and `tsdb_query_count()`) are O(log n): two binary
seeks bound the window and no records are scanned. `SUM/AVG/MIN/MAX` scan the
window block by block.

### 6. Charts: stride sampling

A plot wants a few hundred points, not every sample. `tsdb_query_set_stride()`
returns every N-th in-range record, and once N is at or above
`records_per_block` the iterator reads roughly one block per returned point:

```c
uint32_t span = 365 * 86400;                 // a year at 5-min = ~105k records
uint32_t stride = (span / 300) / 500 + 1;    // ~500 points
tsdb_query_init(&query, now - span, now, cols, 3);
tsdb_query_set_stride(&query, stride);
while (tsdb_query_next(&query, &ts, values) == ESP_OK) { /* ~500 rows */ }
tsdb_query_close(&query);
```

### 7. Backfill: batch writes

`tsdb_write_batch()` takes the lock once and keeps the current block in RAM
until the batch moves on to the next block: one block write per block touched
instead of one per record. Use it for imports and for draining a RAM buffer
you fill while flash is busy.

## Changing Columns

The config's `param_names` **is** the schema. Every segment file records the
column list it was written with, and readers map each segment's columns onto
the current schema **by name**. So changing the schema never touches data:

- Ship a firmware with a different `param_names` list → open just works.
  Columns present in both keep their history, missing names read 0 for old
  records, dropped names are no longer visible, reorders are honoured.
- `tsdb_migrate_schema(names, n, NULL)` does the same at runtime, instantly.
- `tsdb_add_extra_params(names, n)` appends columns at runtime; they persist in
  the segment headers, so the config does not need to know about them.
- `tsdb_resize(n, NULL)` changes the capacity instantly (shrink keeps the
  newest records).

A **rename** cannot be told apart from drop + add by name matching, so rename in
place first; the old name is kept as an alias so earlier segments still map:

```c
tsdb_rename_param(3, "tbat");           // "pcharge" -> "tbat", history kept
int idx = tsdb_find_param("tbat");      // 3
```

Each of these starts a new segment immediately, so the change is on disk even
if the device reboots before the next write.

The only refusal left at open is a column-count mismatch with `param_names`
set to NULL — positional mapping is then ambiguous. Keep names in the config.

## Upgrading From 1.x / 2.x

Nothing is required; one call is recommended. A single-file database at the
configured path is adopted on the first open, cheaply: 2.5's recovery logic
(sidecar adoption — including 2.4.4's `<path>.d/` slots —, orphaned `.mig`,
the V2 block layout, which is now transcoded as it is read) runs, nothing is
written, and every record is readable at once. New records go to segment
files next to it.

**Conversion.** The old file is then moved into the segment format, in place,
without losing a record: each round copies the live records held in the
file's last ~one segment of blocks into `<path>.cNNNNNNNNNN` segments, closes
them, and truncates that tail away (littlefs shrinks a file from its end with
one metadata commit). Extra space at any moment is about one segment. Queries
see one history throughout, in time order, the not-yet-converted part read
from the shrinking file. The adopted header is persisted in `<path>.cv` before
the first cut; a power cut at any point loses nothing and duplicates nothing
(the host harness cuts power before every mutating operation of the
conversion). Converted records count toward `max_records` and are evicted
segment by segment like any others. A header that has to be reconstructed is
rebuilt by the same steps, once.

The work runs in bounded steps, never in `tsdb_open`: every `tsdb_write()`
does one after its own lock hold, and `tsdb_maintain()` does one on request.
On the OpenEVSE energy logger's full 2.5 MB database (16 MB build) that is 689
steps, ~51 s of flash time in all by the host cost model; a step is typically
~74 ms (p95 134 ms) and at most ~620 ms (a littlefs compaction landing on its
commit — plain writes hit the same, up to ~740 ms); free space stays above
210 KB. With writes alone it completes after ~680 records; to finish in about a minute,
call it from the writer task while it reports more work, with a short delay
between calls so readers get the lock, and stop on any error (the next
write's own step retries it):

```c
bool more = true;
while (more) {
    if (tsdb_maintain(&more) != ESP_OK) break;   // error: stop; writes retry
    vTaskDelay(pdMS_TO_TICKS(20));
}
```

Short of space, a round that meets ENOSPC is discarded (the old file still has
its records) and the next one is half the size, down to one 4 KB flash block;
after that the conversion pauses — `more` reads false — for 16 writes or
until a file is evicted, and a write that meets ENOSPC first takes back the
space of a round in progress. Nothing is retried in a loop. On the 16 MB
layout the conversion completes with the history intact from ~150 KB of free
space (the firmware leaves ~360 KB); with less, records go by the existing
out-of-space policy (oldest first; as a last resort the old file).

A full 4 MB-build database (32 KB) converts in ~20 steps and leaves 16–20 KB
of the 128 KB partition free.

**Do not roll back to 2.x once 3.0 has run.** 2.x does not know segment files,
and once the conversion has started the old file is truncated and its sidecars
are gone while its header still describes the whole ring. Measured with
v2.4.4 on a partly converted file: after a clean close its first write
zero-fills the file back out to its full size — on a full unit that fills the
filesystem (0 KB free, and every other file's writes fail); after an unclean
shutdown it restarts the ring at slot 0 and rewrites the rest of the file on
every write. If you must roll back, delete `<path>*` (the file, its `.c*` /
`.s*` segments and `.cv`) first, losing the history.

**One upgrade caveat — devices already short of space.** If the free-space
guard (`free_space_cb` / `min_free_bytes`) is configured and the filesystem is
*already* below the reserve when 3.0 first writes (typical for a device whose
2.x ring had been capped by that guard), the first segment cannot be created
without breaching the reserve, so the engine makes room the only way it can:
it deletes what is left of the adopted 2.x file and logs an ERROR. The same happens on a hard ENOSPC. If that history matters,
free space (or raise the reserve only after the upgrade) before deploying 3.0.
`host_test/test_legacy.c` pins this behaviour.

## Multiple Databases (Handle-Based API, v2.1+)

Every global function has an `_h`-suffixed counterpart taking a `tsdb_t *` handle as its first argument, so several independent databases can be open at once. The legacy global API is a thin wrapper over an internal default handle.

```c
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

## Durability

Every write ends with an `fsync` of the active segment, and on open a
segment's record count comes from its content (full blocks from the file size,
the tail block validated by magic, count and ascending timestamps), not from a
counter that has to be kept in sync. A power cut therefore loses at most the
records of the block being written at that moment and never leaves the
database inconsistent. `tsdb_sync_h()` (close + reopen of the active segment)
remains available for belt-and-braces before a planned reboot.

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

### Why segments: LittleFS and in-place writes

LittleFS stores a file as a backward-linked skip-list of blocks. Modifying a
block in the **middle** of a file rewrites that block *and every block after
it* — ~20 ms per KB that follows, measured in 2.4.0 (which moved the header
out of the file for exactly this reason). The 2.x ring buffer wrote block 0,
1, 2… in place once it wrapped, so a full 2 MB database would have cost tens of
seconds per write — past the task watchdog.

In 3.0 the only in-place write is the tail block of the newest segment (a
small file with nothing after that block), and eviction is `unlink`. Write cost
is flat for the life of the device, on LittleFS and FAT alike.

### What a write costs now

| | 2.5 (ring + sidecar) | 3.0 (segments) |
|---|---|---|
| Single write | block read-back, block write, fsync, sidecar create + write + fsync + close | block write, fsync |
| Host trace, 500 single writes | 1001 fsync, 506 opens | 501 fsync, 39 opens |
| Host trace, 500-record batch | 503 fsync | 34 fsync |

Pre-3.0 on-device figures (ESP32-C6, LittleFS, 14 columns, full production
load) were ~290 ms per write, dominated by the sidecar and the flash program.
Re-run the built-in benchmark (`tsdb_run_benchmark()`) on your hardware for
current numbers.

### Reads

- Query start, count, FIRST/LAST: O(log n) — a binary search over the in-RAM
  segment table, then over one segment's blocks.
- Full scan: one 1 KB block read per `records_per_block` records, plus one
  file open per segment crossed.
- Boot: one open and two small reads per segment file (default ~32 segments).

## Memory Usage

- **Buffer pool**: three 1 KB block buffers (read, write cache, query) — 3 KB
  minimum; the rest of `buffer_pool_size` is currently unused
- **Handle**: ~3 KB (schema names + aliases for up to 64 columns) + 40 bytes
  per segment
- **Stack**: ~1.3 KB per query (`tsdb_query_t` + a block when the pool buffer
  spans pages)

## Storage Calculations

```c
// Records per block depends on the number of columns
// Block size = 1024 bytes, overhead = 8 bytes
// Per record = 4 bytes (timestamp) + (num_params * 2 bytes)

// Example: 10 columns
// Per record = 4 + (10 * 2) = 24 bytes
// Records per block = (1024 - 8) / 24 = 42 records/block

// Each segment adds a 1 KB header (2 KB above 24 columns).
// Default segment size = capacity / 32, at least 6 and at most ~256 blocks,
// rounded so the file (header + data) is 4n - 1 KB and fills n 4 KB flash
// blocks; override with tsdb_config_t.segment_bytes.
```

```c
uint32_t max_records = TSDB_CALC_MAX_RECORDS(2000000, 10);  // 2MB budget, 10 params
```

Leave a few percent of headroom: at capacity the on-disk footprint is the
records plus one header per segment plus up to one partially filled block.

## File Format (v3)

```
<path>.s0000000001         segment 1 (oldest)
<path>.s0000000002         segment 2
...
<path>.s00000000NN         newest segment (held open for appends)
<path>                     optional: an adopted 1.x/2.x single-file database
                           (read-only, shrinking from its end as it is converted)
<path>.cNNNNNNNNNN         segments converted from it (N = record rank in it;
                           they sort before every .s segment)
<path>.cv                  conversion marker: the adopted 2.x header

Segment file:
┌─────────────────────────────────────────────────┐
│ Header (64 bytes, write-once)                   │
│   magic "TSEG", version, sequence               │
│   num_params, records_per_block, blocks_cap     │
│   data_offset, schema generation                │
│   lifetime_base (records written before it)     │
│   created_ts                                    │
├─────────────────────────────────────────────────┤
│ Column names   [num_params][20]                 │
│ Column aliases [num_params][20]  (rename)       │
│ (zero padding to data_offset: 1 KB, or 2 KB     │
│  above 24 columns)                              │
├─────────────────────────────────────────────────┤
│ Data blocks (1024 bytes each, time-ordered)     │
│   magic "BLKT", record_count, timestamps[],     │
│   then one int16 column array per parameter     │
└─────────────────────────────────────────────────┘
```

## API Reference

See `include/esp_tsdb.h` for full API documentation.

### Core Functions

- `tsdb_init()` / `tsdb_open()` - Initialize or open a database (adopts a 1.x/2.x file)
- `tsdb_close()` / `tsdb_close_h()` - Close
- `tsdb_write()` - Write single record (base + extra values)
- `tsdb_write_batch()` - Write multiple records (one block write per block touched)

### Schema Functions

- `tsdb_migrate_schema()` - Replace the column list (instant, by-name mapping)
- `tsdb_add_extra_params()` / `tsdb_migrate_overflow()` - Append / replace runtime extra columns
- `tsdb_rename_param()` / `tsdb_find_param()` - Rename in place (history kept) / look up by name
- `tsdb_get_total_params()` / `tsdb_get_param_name()` / `tsdb_has_overflow()` - Introspection
- `tsdb_resize()` - Change capacity (instant)

### Query Functions

- `tsdb_query_init()` - Initialize query iterator
- `tsdb_query_set_stride()` - Return every N-th in-range record
- `tsdb_query_next()` - Get next record
- `tsdb_query_close()` - Close query (releases the handle lock — always call it)
- `tsdb_query_count()` - Count records in range, O(log n)

### Aggregation Functions

- `tsdb_aggregate()` - `SUM`, `AVG`, `MIN`, `MAX`, `COUNT`, `FIRST`, `LAST`
- `tsdb_aggregate_multi()` - Several aggregations in one pass

### Utility Functions

- `tsdb_get_stats()` - Statistics (retained records, capacity, time bounds, lifetime writes/evictions, storage bytes, segment count)
- `tsdb_clear()` - Delete all data (keeps schema and capacity)
- `tsdb_delete()` - Delete the database files and close
- `tsdb_remove(path)` - Delete every file of a database that is not open (no handle needed)
- `tsdb_disk_usage(path, &bytes)` - On-disk size of a database without opening it
- `tsdb_sync_h()` - Close + reopen the active segment

Every function above has an `_h` variant taking a `tsdb_t *` handle.

## Host Tests

`host_test/` builds the engine with gcc against FreeRTOS/ESP shims:

```sh
cd host_test
for s in build.sh build_*.sh; do bash $s; done
for t in test_wrap test_multi test_schema test_sidecar test_resize test_freecap test_v25 test_legacy test_segments; do ./$t | tail -1; done
./run_compat.sh      # databases written by v2.4.3 / v2.4.4 (clean and power cut) opened by this tree
./run_lfs_upgrade.sh # on real littlefs: upgrades and conversion of 2.x databases (power cuts at every step), a 2.2x-capacity run, failure reporting
./test_lfs_cost      # per-write littlefs cost (reads, programs, erases, commits)
./make_fixtures.sh   # regenerate the 2.5.0 legacy fixtures (needs git history)
```

`build_lfs_*.sh` fetch littlefs v2.11.3 (the version esp_littlefs ships) into
`.littlefs/` unless `LFS_DIR` points at a checkout; `run_compat.sh` and
`run_lfs_upgrade.sh` build the 2.x releases from git history.

## License

MIT License
