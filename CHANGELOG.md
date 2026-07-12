# Changelog

## [2.3.0] - 2026-07-12
### Added
- **Free-space guard** (`tsdb_config_t.free_space_cb` + `min_free_bytes`, both optional/off by default): before the data file grows by a new block, the engine consults the callback and — if free space is below the reserve — caps `max_records` at the current record count and switches to ring/LRU reuse immediately, persisting the cap. Previously the adaptive cap only fired on a hard `ENOSPC`, i.e. when the filesystem was already at zero bytes free — where copy-on-write filesystems (littlefs) also fail every *other* file's in-place writes. The callback should return `UINT64_MAX` when free space cannot be determined, so a failed probe never latches a spurious cap. CAVEAT: the guard covers the base (ring) data region only — the legacy overflow-extras region (`tsdb_add_extra_params`) grows monotonically per write and is NOT ring-reused or guard-capped; databases using overflow extras can still grow past the reserve. (The modern path — first-class base columns via schema migration — is fully covered.)
- **Live ring-capacity resize** (`tsdb_resize()` / `_h`): change `max_records` on an open database. Growing a never-wrapped database is a header-only change (milliseconds); a wrapped ring, a shrink below the retained count (newest records win), or a database with overflow extras (folded into base columns) goes through the same-schema streaming-rewrite engine — inheriting the migration semantics: space budget, `allow_trim`, progress callback, `migrating` fail-fast, crash-safe `.mig` swap. Intended for expandable storage (SD cards): growing only helps when the filesystem actually has room — on a flash partition the database was sized to fill, a bigger capacity just meets ENOSPC later and adaptive capacity re-caps it.

### Fixed
- **Use-after-free closing a database under concurrent access.** `tsdb_close[_h]` deleted the handle mutex and freed the handle while another task — one that had passed its pre-lock `is_open` check — could still be blocked inside `xSemaphoreTake` on that exact mutex; deleting a semaphore under a blocked waiter frees the queue structure the waiter's TCB is linked into (observed live as a panic in a writer task racing a close). Closed handles are now parked in a one-slot recycler with their mutex alive and reused by the next `tsdb_open()`; every lock acquisition re-checks `is_open` after acquiring (`TSDB_LOCK_OPEN_OR_RETURN`), so stragglers wake on a valid mutex and bail cleanly. Additionally: `tsdb_close()`/`tsdb_delete()` unpublish the global handle *before* teardown (new callers fail fast), a close that times out leaves the database fully open and re-published instead of dangling, `tsdb_delete[_h]` no longer unlinks the file when the close failed, and writers/queriers fail fast on a new `closing` flag instead of queueing behind the teardown. A per-handle **generation stamp** (preserved across the recycler like the mutex) closes the close-AND-reopen interleaving: a straggler that blocked across a full close→reopen cycle sees `is_open` true again but a different generation, and bails instead of silently writing its stale record into the new database incarnation.
- Sparse-index writes are now bounds-checked against the preallocated index region. Previously unreachable (nothing could grow `max_records`), but after a `tsdb_resize()` grow, records beyond the creation-time capacity would have had their index entries written past the index region — into the first data blocks.

## [2.2.0] - 2026-07-06
### Added
- **Full schema migration** (`tsdb_migrate_schema()` / `_h`): change the base column set — the MySQL-ALTER-TABLE operation the block geometry previously made impossible. Streaming rewrite (one record in, one record out, ~2KB memory regardless of database size) into a sibling `.mig` file with the new geometry, then an atomic swap. Columns are matched by name against old base columns AND overflow extras — extras named in the new schema become first-class base columns and the overflow region is folded away entirely; dropped columns are discarded, new columns backfilled with 0. Space budget via `tsdb_migrate_opts_t`: caller passes known filesystem free space (`0` = "on SD, just go"), and `allow_trim` permits dropping the oldest records so the newest fit when space is tight. Runs at runtime under the handle lock with a `migrating` flag — concurrent writes/queries fail fast with `ESP_ERR_INVALID_STATE` instead of stalling, so on a periodic write cadence a migration completing within one interval loses no samples. Progress reporting via an optional callback (`(records_done, records_total)` at start, every ~1% by default, and at completion) for driving UI progress bars. Crash-safe: the original is never modified; an interrupted swap is recovered (stale `.mig` removed, orphaned complete `.mig` adopted) on the next `tsdb_open()`. Host regression suite in `host_test/test_schema.c`.
- **V4 wide-schema format: up to 64 first-class columns** (was 16). The 16-column limit was only ever the header's fixed `param_names[16][32]` array — block geometry is runtime-computed. Files created or migrated with >16 columns get a V4 header: names 17–64 live in an extension region (offset 584, 20 bytes each) with the sparse index moved to offset 2048. ≤16-column files remain V3 and fully backward-compatible. Combined with schema migration this replaces the overflow-region path at equal total capacity (the old 16 base + 48 overflow extras) but with every column first-class: ring-buffered, columnar, fast to query, no unbounded sequential growth. Caveat: firmware older than v2.2 cannot read V4 files (header sanity rejects >16 params) — a downgrade with a wide database recreates it empty. Names for columns beyond index 16 are capped at 19 characters.
- **O(log n) query start seek**: `tsdb_query_init` now binary-searches the ring's logical (time-ascending) positions to jump straight to the first record ≥ `start_time`, instead of linearly scanning from the oldest record. A "last 24 hours" chart query on a year-full database drops from ~3400 block reads to ~17. Falls back to the full walk on any probe anomaly; result sets are unchanged.

### Fixed
- **Header/index overlap**: `sizeof(tsdb_header_t)` is 584 bytes but the index has always been placed at offset 512, so every header write clobbered sparse-index entries 0–8 (silent — the index is not used by queries), and a small enough index put the first data block inside the header's footprint, corrupting block 0 on the next header write. New and migrated files place the index at offset 1024; the offset is stored per-file and honored everywhere, so existing 512-offset files keep working unchanged (header reconstruction now probes both bases).
- Mixed recursive/plain FreeRTOS mutex calls: the handle mutex is created with `xSemaphoreCreateRecursiveMutex()`, but `tsdb_get_stats_h`, `tsdb_clear_h`, `tsdb_sync_h`, `tsdb_add_extra_params_h` and `tsdb_migrate_overflow_h` locked it with plain `xSemaphoreTake`/`xSemaphoreGive` — undefined per the FreeRTOS API and corrupts the recursion count when the calling task already holds the lock (e.g. clearing during a query). All paths now go through the recursive lock helpers.
- Adaptive-capacity latch (`ENOSPC` shrink guard) was a file-scope static shared across all handles; with the v2.1 multi-instance API, the first database to adapt suppressed adaptation on every other open database. Now per-handle.
- Opening a file whose `num_params` differs from the config's was allowed with only a warning, but writes iterate the file's parameter count over the caller's values array — reading past the end of it when the file has more columns than the config, and silently dropping/mislabeling columns when it has fewer. `tsdb_open` now refuses to open on a mismatch; migrate or delete/recreate the file.

## [2.1.0] - 2026-07-05
### Added
- Handle-based multi-instance API (`tsdb_open_h()` returning a `tsdb_t *`, plus `_h` variants of every operation: write, query, aggregate, stats, migrate, clear, delete). Multiple independent databases can be open simultaneously. Fully backward-compatible — the original global-singleton API is preserved as wrappers over an internal default handle.
- `tsdb_sync_h()` for an explicit durability commit of the header/dir entry (useful before planned reboots).
- Host-side regression test harness under `host_test/` (FreeRTOS/ESP shims, wrapped-ring and multi-handle tests, `build.sh`/`build_multi.sh`).

### Fixed
- Range queries (`tsdb_query_init`/`tsdb_query_next`) returned wrong or empty results once a database filled and began LRU-evicting (i.e. after the ring buffer wraps). Short time windows came back empty or with only the oldest edge, and full scans were non-monotonic. The iterator advanced the block number linearly with no ring wraparound and seeded its start position from the sparse index, whose entries are ordered by physical slot (not timestamp) after a wrap, making the binary search invalid. The iterator now walks the ring in logical time-ascending order from `oldest_record_idx` modulo `max_records`, with an early-exit once `ts > end_time`. Read-side only — no on-disk format or write-path change; existing data stays valid.
- Internal-RAM buffer pool now allocates with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`; plain `MALLOC_CAP_INTERNAL` could hand back IRAM on Xtensa targets (ESP32/S3), causing a LoadStoreError on byte access.
- `tsdb_open` no longer trusts a bare `stat()` to decide a database file exists — avoids treating a stale/phantom dir entry as a valid DB.
- All operations serialized behind a global mutex (safe concurrent access from multiple tasks); adaptive capacity recalculation on open when `max_records` changed or the filesystem shrank; stricter header validation before accepting an existing file.

## [2.0.2] - 2026-03-15
### Fixed
- Divide-by-zero crash when `max_records=0` (unlimited mode). All modulo/comparison operations now guard for unlimited mode — no ring buffer wrapping, no eviction, `record_idx = total_records` directly.

## [2.0.1] - 2026-03-15
### Fixed
- Block struct/disk layout mismatch when `records_per_block > 38`. The `tsdb_block_t` struct had fixed `timestamps[38]` and `params[16][38]` arrays, causing cross-column data corruption with fewer than 11 params. All block I/O now uses runtime offset macros.
- Automatic V2→V3 block migration on first open for affected databases.

### Changed
- Bumped `TSDB_VERSION` to 3.

## [2.0.0] - 2026-03-15
### Added
- Runtime extra parameters via overflow region (`tsdb_add_extra_params()`)
- Schema migration with data preservation (`tsdb_migrate_overflow()`) — add, drop, reorder columns
- Built-in on-device benchmark (`tsdb_run_benchmark()`)
- Overflow introspection: `tsdb_get_total_params()`, `tsdb_get_param_name()`, `tsdb_has_overflow()`
- Crash-safe overflow writes and migrations
- V1 backward compatibility (files open seamlessly under V2+ engine)

### Changed
- `TSDB_MAX_PARAMS` increased from 16 to 64
- Header `reserved[16]` replaced with overflow metadata fields
- Engine is now fully data-agnostic — no domain knowledge

## [1.0.0] - 2025-01-22
### Added
- Initial release
- Columnar storage with selective parameter reading
- Sparse time index with binary search
- Ring buffer with LRU eviction
- Configurable memory allocation (internal RAM, PSRAM, auto)
- Paged buffer support for fragmented heaps
- Streaming queries for HTTP/WebSocket
- Built-in aggregations: SUM, AVG, MIN, MAX, COUNT, FIRST, LAST
- Multi-aggregation in single pass
- Batch write support
- Header reconstruction from data blocks on corruption
