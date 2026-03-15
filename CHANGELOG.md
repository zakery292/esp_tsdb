# Changelog

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
