# Changelog

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
