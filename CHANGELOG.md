# Changelog

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
