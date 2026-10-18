# Changelog

## [3.0.0-beta.1] - 2026-10-18
### Changed — new on-disk format: append-only segments
- **The ring buffer is gone; a database is now a set of append-only segment files** (`<path>.s0000000001`, `.s0000000002`, …). Each segment is a write-once header (its own column list, geometry and lifetime counters) followed by 1 KB columnar blocks in time order. Records are only ever appended to the newest segment; when it reaches its block budget (`segment_bytes`, default: capacity split into ~32 segments) or the schema changes, the next write starts a new one. Eviction is logical at the head (the oldest `total_on_disk − max_records` records are invisible) and physical per segment (the oldest file is unlinked once every record in it has left the window), so capacity is exact and nothing has to be persisted to track it.

  **Why:** littlefs stores a file as a backward-linked skip-list, so writing in the *middle* of a file rewrites every block after it — ~20 ms per KB that follows, measured in 2.4.0. Once a 2.x ring wrapped, each write landed near the head of the file and cost tens of seconds on a 2 MB database (task-watchdog territory). 2.4.0 fixed exactly this for the header by moving it to a sidecar; the data blocks had the same problem waiting at the first wrap. With segments the only in-place write is the tail block of a small file, and eviction is an unlink.

- **No header to persist, no sidecar.** The record count of a segment is derived from its size and a content check of the tail block on open (magic, count, ascending non-zero timestamps), so a torn write loses at most the records of the block being written and never corrupts anything. A write is now: one tail-block write + one fsync. Host measurement, 500 single writes: 1001 fsyncs and 506 file creates in 2.5 → 501 fsyncs and 39 creates in 3.0; a 500-record batch: 503 fsyncs → 34.
- **Existing databases are adopted in place, read-only, with no copy.** A 2.x single-file database at the configured path becomes the oldest segment (sequence 0); its records are read through the unchanged 2.5 reader (sidecar adoption, header reconstruction, V2→V3 layout migration, orphaned `.mig` recovery all still apply on first open). New records go to new segments. When the capacity window moves past its last record, the old file and its sidecars are unlinked. Verified byte-for-byte: `host_test/make_fixtures.sh` builds seven 2.5.0 databases (wrapped ×2, overflow extras, V4 wide, pre-2.2 index-at-512, sidecar-ahead crash, unlimited, partial block) and `test_legacy.c` requires 3.0 to dump each identically, then write past it, evict it and survive a reopen.
- **Schema changes are instant and never rewrite data.** Every segment carries its column names; readers map segment columns onto the current schema by name (and by one-hop alias after `tsdb_rename_param`). `tsdb_migrate_schema()`, `tsdb_add_extra_params()`, `tsdb_migrate_overflow()`, `tsdb_rename_param()` and `tsdb_resize()` all take effect immediately and persist by starting a new segment on the spot — so a rename or an added extra survives a reboot even with no write in between. `tsdb_migrate_opts_t`, the progress callback and `schema_policy` are kept for source compatibility and ignored: both policies reconcile by name; the only refusal left is a column-count mismatch when `param_names` is NULL (positional mapping is ambiguous).
- **Extras are ordinary columns.** The sequential overflow region is gone (it only ever existed because the ring's geometry could not change). Runtime extras live in the segment headers, work on unlimited databases, and are read at the same speed as base columns. Legacy overflow extras in an adopted 2.x file are still read from its overflow region.
- Free-space guard and ENOSPC now **turn the file set over** instead of switching to in-place reuse: the oldest segment is unlinked and the capacity is latched at what is on disk plus one segment, so the files stop growing while writes continue. The latch is in RAM; a reopen restores the config value and the guard simply fires again if space is still short.
- `tsdb_clear()` unlinks every segment (flat cost, space returned). `tsdb_sync_h()` closes and reopens the active segment; it is no longer needed for durability.
- `tsdb_stats_t.segment_count` added. `tsdb_config_t.segment_bytes` added. `index_stride` is ignored.
- Removed sources `tsdb_index.c` and `tsdb_migrate.c` (sparse index and the streaming rewrite engine); added `tsdb_legacy.c` and `tsdb_segment.c`. Public API unchanged apart from the additions above; `tsdb_query_t`'s internals changed (it was always meant to be opaque).

### Behaviour notes
- **Upgrade caveat for devices already below the free-space reserve** (e.g. a 2.x ring that the guard had capped): the first 3.0 write cannot create a segment within the reserve, so the adopted 2.x file is deleted to make room (ERROR logged with the record count). The same happens on hard ENOSPC. Free space before deploying 3.0 if that history matters. Pinned by `test_legacy.c`.
- Schema changes, `tsdb_clear()` and `tsdb_delete()` return `ESP_ERR_INVALID_STATE` while a query is open on the handle (they close or unlink files the query reads through). Close queries first.
- Raising the capacity (config or `tsdb_resize`) brings back recently evicted records that are still on disk; records are only gone once their segment file is unlinked.
- Downgrading firmware below 3.0 after an upgrade is not supported: the old firmware will not see the segment files (it finds and uses its original single file if that has not yet been evicted).
- Boot cost: one open and two small reads per segment file (default ~32 segments).
- Timestamps are assumed monotonic (unchanged assumption; the within-segment seek relies on it).

## [2.5.0] - 2026-10-13
### Added
- **Schema reconciliation at open** (`tsdb_config_t.schema_policy = TSDB_SCHEMA_MIGRATE`, with `migrate_opts` for the space budget). Until now a firmware whose column list changed could not open its own old file: `tsdb_open` refused on a count mismatch, and `tsdb_migrate_schema()` needs an open handle — so the app had to know the *old* column list to get far enough to migrate. With the policy set, open compares the file's column names to `param_names`, and when they differ (count, order or names) runs the existing streaming-rewrite engine by name before returning the handle: kept columns carry their data, dropped ones go, new ones read 0, reorders are honoured, overflow extras are folded in. When nothing differs it is a plain comparison. The default (`TSDB_SCHEMA_STRICT`, zero) keeps the old refusal. Caveat: a rename looks like drop+add to by-name matching, so rename first with `tsdb_rename_param()`.
- `tsdb_rename_param[_h]()` — header-only rename of a base column (persisted through the sidecar for columns 0–15, a 20-byte write to the extended-names region for 16–63). `tsdb_find_param[_h]()` — column index by name.
- `tsdb_query_set_stride()` — return every N-th in-range record. A chart that wants 500 points from a year of 5-minute samples no longer reads all ~3,400 blocks: with the stride at or above `records_per_block` the iterator reads about one block per returned point.
- `tsdb_write_batch[_h]()` is now a real batch: one lock, the current block kept in RAM and written once when the batch moves to the next block (or ends), one header persist at the end. For a backfill of N records at R records per block that is at most 2N/R block writes (one to allocate each block while the file grows, one when the batch leaves it; one per block once the ring has wrapped) + 1 sidecar write, instead of N + N. Blocks that *grow* the file are still written immediately so the adaptive-capacity path sees ENOSPC exactly as before. A batch that fails mid-way leaves the header describing only what reached disk (the dirty block's records are rolled back, not counted).
- Host suite `host_test/test_v25.c` covering all of the above plus the fixes below.

### Changed
- **Queries: O(log n) where a scan is not needed.** `tsdb_query_count()` is now two binary seeks (it was documented "without iterating" and iterated every record). `tsdb_aggregate()` with `COUNT`, `FIRST` or `LAST` is two seeks plus at most one record read. `SUM/AVG/MIN/MAX` and `tsdb_aggregate_multi()` accumulate straight off each loaded block instead of pulling every record through the iterator. A window entirely outside the retained time range (future, or already evicted) now costs zero block reads.
- Legacy overflow extras in a query are read with one seek+read per record instead of a save-position/seek/read/restore cycle *per extra column per record*.
- **Writes: fewer explicit block reads per record.** The block written last stays in the pool's write-cache region, so a record landing in the same block (every record but one per block) skips the library's read-back; and when the ring has wrapped, the new oldest record's timestamp is taken from that in-RAM block instead of a second read whenever they share a block. How much of that reaches the flash depends on the C library's stdio buffering around `fseek` (glibc re-primes its buffer on seek; newlib's behaviour depends on the VFS's `st_blksize`) — measure with the benchmark endpoint rather than assume.
- The two capacity-cap paths in `tsdb_write()` (free-space guard, ENOSPC) persisted the cap with an **in-place header rewrite** — the exact operation 2.4.0 moved to the sidecar because it costs seconds on a large littlefs file, and at zero free space it could not be relied on to succeed anyway. Both now persist through the sidecar (also written on the failure exit, so the cap survives a reboot). The `tsdb_resize()` fast path likewise.
- `tsdb_clear()` **recreates** the file (header + empty index) instead of rewriting the index in place at the head of the file — which on littlefs copy-on-writes every block after it. Cost is now flat regardless of size, and the blocks go back to the filesystem. Databases with a legacy overflow region keep the in-place path (the region lives past the data blocks).
- `tsdb_aggregate_multi()` rejects more than `TSDB_MAX_PARAMS` requests instead of indexing past its accumulator arrays.

### Fixed
- **A ring that wrapped more than twice was "corrupt" on the next open.** The header sanity check bounded the *lifetime* `total_records` counter by `2 × max_records`, so once a database had cycled twice, every boot rejected the header and reconstructed it from the blocks — resetting the statistics and, worse, computing the next write slot from the record *count* rather than the lifetime counter, so the next write overwrote a record that was not the oldest and broke the ring's time order (non-monotonic queries, binary seek falling back to full scans). The check now bounds the ring cursors instead, and header reconstruction derives a slot-consistent counter for a full ring. At one record per 5 minutes this needed ~2 × capacity writes — ~450 days on a 65k-record ring, but only ~140 days on a ring the free-space guard had capped at 20k.
- **Overflow extras were placed inside the data region's growth envelope.** `tsdb_add_extra_params()` put the region at the end of the file *at that moment*; the ring allocates blocks lazily, so a database that had not yet reached `max_records` kept growing straight through it — later data blocks overwrote the extras, and the per-record extras writes landed inside those blocks (clobbering their magic/count, so the next read reinitialised the block and lost its records). The region now goes at the end of the fully grown data region (the filesystem zero-fills the gap once); `tsdb_migrate_overflow()` applies the same floor; unlimited databases refuse extras (`ESP_ERR_NOT_SUPPORTED`, use `tsdb_migrate_schema()`); and open logs an error for an existing region that sits inside the envelope. The host suites never hit this because their rings fit in one block.
- `tsdb_write()` applied the eviction bookkeeping (oldest cursor advance, eviction count) *before* the block write; a failed write left the header one record ahead of the file. Bookkeeping now follows the write.

### Known limitation (unchanged, now documented)
- On littlefs, once the ring has wrapped, every write lands in a block near the **head** of the file and littlefs copy-on-writes every block after it — the same linear cost 2.4.0 measured for the header (~20 ms/KB). A 2 MB ring that has wrapped costs tens of seconds per write on littlefs. Pre-wrap writes append and are cheap; SD/FAT in-place writes are cheap at any point. See the README for the arithmetic and the planned segment-file format.

## [2.4.3] - 2026-08-31
### Fixed
- The published PlatformIO package no longer carries build byproducts. `release_notes.md` and `dist/` are written by earlier steps of the release workflow and were being swept into the package tarball; `.github/` was shipping too. All three are now excluded, alongside `host_test/`. Harmless (nothing was compiled from them), but they are not part of the library.

## [2.4.2] - 2026-08-31
### Added
- First release published to the **PlatformIO Registry**, alongside the existing ESP Component Registry upload. PlatformIO consumers previously had to pin the component by git URL, which `pio pkg outdated` cannot see — so a downstream project's dependency scanner never reported a new esp_tsdb release. They can now depend on a version spec and be told about updates. No code change in this version.

## [2.4.1] - 2026-08-31
### Added
- **PlatformIO manifest (`library.json`).** The component shipped only `idf_component.yml`, which PlatformIO cannot read — so a PlatformIO consumer saw the package as `esp_tsdb@0.0.0+<sha>` and could not express a version constraint at all; the git ref was the only way to pin it. With a manifest, `esp_tsdb@^2.4.1` resolves normally. `srcDir`/`includeDir` match the layout the LDF already inferred and `host_test/` is excluded from the package, so the set of compiled files is unchanged. ESP-IDF consumers are unaffected: `idf_component.yml` and `CMakeLists.txt` still drive that path.

## [2.4.0] - 2026-08-31
### Changed
- **The database header is written to a sidecar file instead of in place.** The in-file header sits at offset 0, and littlefs stores a file as a CTZ skip-list of block addresses — so rewriting byte 0 rewrites the *whole* file, at a cost linear in its size (~20 ms/KB measured on esp_littlefs; ~5.1 s for a 268 KB database). Since the header is rewritten on every sample, a database that worked for weeks eventually crosses the task-watchdog budget and panics the device mid-write; observed live as a boot loop on an ESP32 logging one record per 5 minutes, once the file reached ~170 KB. The header now goes to one of two alternating slots (`<db>.h0` / `<db>.h1`) holding `{magic, seq, crc32, header}` — a fresh small file each time, so the cost is flat regardless of database size (~56-80 ms).

  Backward compatible in both directions, and no migration step. On open, the in-file header stays authoritative; a sidecar is adopted only when it is valid (magic + CRC), geometrically identical, and strictly ahead of it — i.e. only to recover the records an unclean shutdown would otherwise have lost. `tsdb_close()` refreshes the in-file header and removes the sidecars, so a cleanly-closed database is byte-for-byte an ordinary esp_tsdb file that 2.3.0 reads unchanged.

  Host coverage in `host_test/test_sidecar.c`: clean close, crash-with-sidecar-ahead, and corrupt-sidecar-ignored.

### Added
- `esp32p4` added to the component manifest's supported targets.

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
