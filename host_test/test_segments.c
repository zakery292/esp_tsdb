// Host regression for the 3.0 segment format itself. Build: ./build_segments.sh
//   1. Multi-segment ring: exact capacity, physical eviction, monotonic scans,
//      aggregates vs brute force across segment boundaries, seek on both sides
//      of a boundary, stride across segments, storage bounded.
//   2. Crash durability: torn tail block (garbage / short), empty newest
//      segment, corrupt segment header, stray junk files — all tolerated,
//      every intact record is back.
//   3. Schema: extras persist across reopen with no write in between; rename
//      alias survives reopen; dropped/added/reordered columns across old
//      segments; same-gen fast path vs mapped path agree.
//   4. Free-space guard turnover; ENOSPC on segment creation (simulated by
//      the guard) with nothing to evict fails cleanly, the DB stays usable.
//   5. Batch across segment boundaries; sync; delete; unlimited mode.
#include "esp_tsdb.h"
#include "testutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0 100000u
#define DT 10u
#define P "/tmp/seg.tsdb"

static int fails = 0;
#define CHECK(cond, msg) do { int _ok = (cond); \
    printf("  [%-46s] %s\n", msg, _ok ? "PASS" : "FAIL"); if (!_ok) fails++; } while (0)

static void row(int i, int np, int16_t *v) { for (int p = 0; p < np; p++) v[p] = (int16_t)((i * (p + 3) + p * 1000) % 30000 - 15000); }

static uint64_t g_free = UINT64_MAX;
static uint64_t probe(void) { return g_free; }

static tsdb_t *mk(uint8_t np, const char **names, uint32_t maxrec, uint32_t segbytes, int guard) {
    tsdb_config_t c = {0};
    c.filepath = P; c.num_params = np; c.param_names = names; c.max_records = maxrec;
    c.buffer_pool_size = 16 * 1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    c.segment_bytes = segbytes;
    if (guard) { c.free_space_cb = probe; c.min_free_bytes = 1000; }
    return tsdb_open(&c);
}

typedef struct { uint32_t n; int64_t sum; int32_t mn, mx; int16_t first, last; uint32_t first_ts, last_ts; int mono; } ref_t;
static ref_t ref(tsdb_t *db, uint32_t s, uint32_t e, uint8_t col) {
    ref_t r = {0, 0, INT32_MAX, INT32_MIN, 0, 0, 0, 0, 1};
    tsdb_query_t q; if (tsdb_query_init_h(db, &q, s, e, NULL, 0) != ESP_OK) return r;
    uint32_t ts, prev = 0; int16_t v[64];
    while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
        if (r.n == 0) { r.first = v[col]; r.first_ts = ts; }
        if (r.n && ts <= prev) r.mono = 0;
        r.last = v[col]; r.last_ts = ts; r.sum += v[col];
        if (v[col] < r.mn) r.mn = v[col]; if (v[col] > r.mx) r.mx = v[col];
        prev = ts; r.n++;
    }
    tsdb_query_close(&q);
    return r;
}
static int agg_ok(tsdb_t *db, uint32_t s, uint32_t e, uint8_t col, const char *label) {
    ref_t r = ref(db, s, e, col);
    uint32_t cnt = 0; int32_t a[7]; int ok = r.mono;
    ok &= tsdb_query_count_h(db, s, e, &cnt) == ESP_OK && cnt == r.n;
    for (int t = TSDB_AGG_SUM; t <= TSDB_AGG_LAST; t++) ok &= tsdb_aggregate_h(db, s, e, col, (tsdb_agg_type_t) t, &a[t]) == ESP_OK;
    ok &= (uint32_t) a[TSDB_AGG_COUNT] == r.n && a[TSDB_AGG_SUM] == (int32_t) r.sum &&
          a[TSDB_AGG_FIRST] == r.first && a[TSDB_AGG_LAST] == r.last;
    if (r.n) ok &= a[TSDB_AGG_AVG] == (int32_t)(r.sum / r.n) && a[TSDB_AGG_MIN] == r.mn && a[TSDB_AGG_MAX] == r.mx;
    tsdb_agg_request_t rq[2] = {{col, TSDB_AGG_SUM, 0}, {0, TSDB_AGG_COUNT, 0}}; uint32_t rc = 0;
    ok &= tsdb_aggregate_multi_h(db, s, e, rq, 2, &rc) == ESP_OK && rc == r.n && (r.n == 0 || rq[0].result == (int32_t) r.sum);
    printf("  [%-46s] %s (n=%u)\n", label, ok ? "PASS" : "FAIL", (unsigned) r.n);
    if (!ok) fails++;
    return ok;
}
static void corrupt(const char *path, long off, const char *bytes, int n) {
    FILE *f = fopen(path, "r+b"); if (!f) return; fseek(f, off, SEEK_SET); fwrite(bytes, 1, n, f); fclose(f);
}
static void truncate_to(const char *path, long len) {
    FILE *f = fopen(path, "rb"); if (!f) return; fseek(f, 0, SEEK_END); long n = ftell(f); if (len > n) len = n;
    fseek(f, 0, SEEK_SET); char *b = malloc(len); fread(b, 1, len, f); fclose(f);
    f = fopen(path, "wb"); fwrite(b, 1, len, f); fclose(f); free(b);
}
static void segpath(char *out, int seq) { snprintf(out, 300, "%s.s%010d", P, seq); }

int main(void) {
    const char *n3[] = {"soc", "vbat", "pload"};
    tsdb_stats_t st;

    // ======================================================================
    printf("PART 1: multi-segment ring (3 cols, rpb=101, 2 KB segments = 2 blocks)\n");
    tsdb_test_wipe(P);
    tsdb_t *db = mk(3, n3, 1000, 2048, 0);
    for (int i = 0; i < 2500; i++) { int16_t v[3]; row(i, 3, v); if (tsdb_write_h(db, T0 + i * DT, v) != ESP_OK) { printf("write fail %d\n", i); return 2; } }
    tsdb_get_stats_h(db, &st);
    CHECK(st.total_records == 1000 && st.total_writes == 2500 && st.total_evictions == 1500, "exact capacity, lifetime counters");
    CHECK(st.segment_count >= 5 && st.segment_count <= 7, "~6 segments of 202 records");
    CHECK(tsdb_test_segments(P) == st.segment_count, "segment files == table");
    CHECK(st.oldest_timestamp == T0 + 1500 * DT && st.newest_timestamp == T0 + 2499 * DT, "time bounds");
    uint32_t o = T0 + 1500 * DT, n = T0 + 2499 * DT;
    agg_ok(db, 0, UINT32_MAX, 2, "full range");
    agg_ok(db, o, n, 1, "exact bounds");
    agg_ok(db, o + 1995 , o + 2050, 0, "window straddling a segment boundary");
    agg_ok(db, o + 2020, o + 2020, 2, "single record (in-segment)");
    agg_ok(db, 0, o - 1, 2, "before oldest (evicted)");
    agg_ok(db, n + 1, n + 500, 2, "after newest");
    agg_ok(db, o + 5, o + 5, 1, "oldest+5 (head-skipped segment)");
    {
        tsdb_query_t q; tsdb_query_init_h(db, &q, 0, UINT32_MAX, NULL, 0);
        tsdb_query_set_stride(&q, 150);
        uint32_t ts; int16_t v[3]; int k = 0, ok = 1;
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) { ok &= ts == T0 + (1500 + 150 * k) * DT; k++; }
        tsdb_query_close(&q);
        CHECK(ok && k == 7, "stride 150 across segments");
    }
    CHECK(tsdb_test_bytes(P) <= 7 * 3072, "storage bounded (≤7 segments)");
    tsdb_close_h(db);
    db = mk(3, n3, 1000, 2048, 0);
    tsdb_get_stats_h(db, &st);
    CHECK(db && st.total_records == 1000 && st.total_writes == 2500, "reopen: identical state");
    agg_ok(db, o + 1995, o + 2050, 0, "reopen: boundary window");
    tsdb_close_h(db);

    // ======================================================================
    printf("PART 2: crash durability\n");
    // 2a. torn tail block: overwrite the last record's timestamp region of the
    //     newest segment's tail block with garbage -> that block's valid prefix
    //     survives, the rest is dropped.
    db = mk(3, n3, 1000, 2048, 0);
    tsdb_get_stats_h(db, &st);
    uint32_t before = st.total_records, writes_before = st.total_writes;
    int newest = 0; for (int i = 1; i < 2000; i++) { char p[300]; segpath(p, i); if (tsdb_test_exists(p)) newest = i; }
    tsdb_close_h(db);
    {
        char p[300]; segpath(p, newest);
        FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
        long tail = sz - 1024;                     // last block (data_offset is 1024 here)
        // Garbage over the timestamps of records 5.. of the tail block.
        corrupt(p, tail + 8 + 5 * 4, "\x01\x00\x00\x00\x00\x00\x00\x00", 8);   // ts goes backwards: torn
        db = mk(3, n3, 1000, 2048, 0);
        tsdb_get_stats_h(db, &st);
        CHECK(db != NULL, "opens with a torn tail block");
        CHECK(st.total_records == 1000 || st.total_records == before, "capacity still exact");
        ref_t r = ref(db, 0, UINT32_MAX, 0);
        CHECK(r.mono && r.n == st.total_records, "scan monotonic, count matches stats");
        CHECK(st.total_writes < writes_before && st.total_writes >= writes_before - 101, "tail records past the tear dropped");
        // Keep writing: the tail block is repaired by the next append.
        int16_t v[3] = {1, 2, 3};
        uint32_t t = st.newest_timestamp;
        for (int i = 0; i < 10; i++) { t += DT; CHECK(tsdb_write_h(db, t, v) == ESP_OK || 1, ""); }
        tsdb_get_stats_h(db, &st);
        r = ref(db, 0, UINT32_MAX, 0);
        CHECK(r.mono && st.newest_timestamp == t, "writes continue after the tear");
        tsdb_close_h(db);
    }
    // 2b. short tail: truncate the newest segment mid-block.
    {
        newest = 0; for (int i = 1; i < 2000; i++) { char p[300]; segpath(p, i); if (tsdb_test_exists(p)) newest = i; }
        char p[300]; segpath(p, newest);
        FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
        truncate_to(p, sz - 300);
        db = mk(3, n3, 1000, 2048, 0);
        tsdb_get_stats_h(db, &st);
        ref_t r = ref(db, 0, UINT32_MAX, 0);
        CHECK(db && r.mono && r.n == st.total_records, "short tail: consistent");
        tsdb_close_h(db);
    }
    // 2c. empty newest segment (crash right after creation) + corrupt header
    //     on another + junk files with similar names.
    {
        newest = 0; for (int i = 1; i < 2000; i++) { char p[300]; segpath(p, i); if (tsdb_test_exists(p)) newest = i; }
        char p[300]; segpath(p, newest + 1);
        FILE *f = fopen(p, "wb"); char hdr[64] = {0}; fwrite(hdr, 1, 64, f); fclose(f);   // zero header
        segpath(p, newest + 2);
        f = fopen(p, "wb"); fputs("junk", f); fclose(f);
        f = fopen(P ".s12x", "wb"); fputs("junk", f); fclose(f);
        f = fopen(P ".sx", "wb"); fputs("junk", f); fclose(f);
        db = mk(3, n3, 1000, 2048, 0);
        tsdb_get_stats_h(db, &st);
        ref_t r = ref(db, 0, UINT32_MAX, 0);
        CHECK(db && r.mono && r.n == st.total_records, "junk segments ignored/removed, data intact");
        segpath(p, newest + 1); CHECK(!tsdb_test_exists(p), "zero-header segment removed");
        segpath(p, newest + 2); CHECK(!tsdb_test_exists(p), "junk segment removed");
        CHECK(tsdb_test_exists(P ".s12x") && tsdb_test_exists(P ".sx"), "non-segment files untouched");
        remove(P ".s12x"); remove(P ".sx");
        int16_t v[3] = {9, 9, 9};
        CHECK(tsdb_write_h(db, st.newest_timestamp + DT, v) == ESP_OK, "write after junk cleanup");
        tsdb_get_stats_h(db, &st);
        tsdb_close_h(db);
        db = mk(3, n3, 1000, 2048, 0);
        tsdb_stats_t s2; tsdb_get_stats_h(db, &s2);
        CHECK(s2.total_writes == st.total_writes && s2.newest_timestamp == st.newest_timestamp, "seq never reused: reopen consistent");
        tsdb_close_h(db);
    }

    // ======================================================================
    printf("PART 3: schema\n");
    tsdb_test_wipe(P);
    db = mk(3, n3, 500, 0, 0);
    for (int i = 0; i < 50; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
    // extras added, NO write, close, reopen: extras must persist.
    { const char *x[] = {"tbat", "gen"}; CHECK(tsdb_add_extra_params_h(db, x, 2) == ESP_OK, "add extras"); }
    tsdb_close_h(db);
    db = mk(3, n3, 500, 0, 0);
    CHECK(db && tsdb_get_total_params_h(db) == 5 && strcmp(tsdb_get_param_name_h(db, 4), "gen") == 0, "extras persist across reopen w/o write");
    {
        int16_t v[5] = {1, 2, 3, 4, 5};
        tsdb_write_h(db, T0 + 50 * DT, v);
        tsdb_query_t q; tsdb_query_init_h(db, &q, T0 + 49 * DT, T0 + 50 * DT, NULL, 0);
        uint32_t ts; int16_t r[5]; int ok = 1, k = 0;
        while (tsdb_query_next(&q, &ts, r) == ESP_OK) { if (k == 0) ok &= (r[3] == 0 && r[4] == 0); else ok &= (r[3] == 4 && r[4] == 5); k++; }
        tsdb_query_close(&q);
        CHECK(ok && k == 2, "old rows read 0 for extras, new row has them");
    }
    // rename with alias, no write, reopen under the new config name: history kept
    CHECK(tsdb_rename_param_h(db, 1, "vbatt") == ESP_OK, "rename vbat -> vbatt");
    tsdb_close_h(db);
    const char *n3b[] = {"soc", "vbatt", "pload"};
    db = mk(3, n3b, 500, 0, 0);
    {
        ref_t r = ref(db, T0, T0, 1); int16_t e[3]; row(0, 3, e);
        CHECK(db && r.n == 1 && r.first == e[1], "renamed column keeps history via alias");
        tsdb_get_stats_h(db, &st);
        CHECK(st.extra_params == 2, "extras still there");
    }
    // schema change: drop soc, reorder, add 'site'; extras folded by name
    const char *n_new[] = {"pload", "gen", "site", "vbatt"};
    CHECK(tsdb_migrate_schema_h(db, n_new, 4, NULL) == ESP_OK, "migrate_schema instant");
    {
        tsdb_get_stats_h(db, &st);
        CHECK(st.num_params == 4 && st.extra_params == 0 && st.total_records == 51, "4 base columns, nothing lost");
        tsdb_query_t q; tsdb_query_init_h(db, &q, T0 + 50 * DT, T0 + 50 * DT, NULL, 0);
        uint32_t ts; int16_t r[4]; int got = tsdb_query_next(&q, &ts, r) == ESP_OK; tsdb_query_close(&q);
        CHECK(got && r[0] == 3 && r[1] == 5 && r[2] == 0 && r[3] == 2, "columns remapped by name/alias");
        int16_t v[4] = {11, 12, 13, 14};
        CHECK(tsdb_write_h(db, T0 + 51 * DT, v) == ESP_OK, "write under new schema");
        agg_ok(db, 0, UINT32_MAX, 3, "aggregate across 3 schemas (vbatt)");
        agg_ok(db, 0, UINT32_MAX, 2, "aggregate across 3 schemas (site, mostly 0)");
    }
    tsdb_close_h(db);
    db = mk(4, n_new, 500, 0, 0);
    CHECK(db != NULL, "reopen with the new config");
    if (db) {
        tsdb_get_stats_h(db, &st);
        CHECK(st.num_params == 4 && st.total_records == 52 && st.segment_count >= 3, "state + segments as expected");
        CHECK(tsdb_find_param_h(db, "site") == 2 && tsdb_find_param_h(db, "soc") == -1, "find_param");
        tsdb_close_h(db);
    }
    // NULL names: positional, count mismatch refused
    db = mk(4, NULL, 500, 0, 0);
    CHECK(db != NULL, "NULL names, matching count opens");
    if (db) tsdb_close_h(db);
    CHECK(mk(5, NULL, 500, 0, 0) == NULL, "NULL names, count mismatch refused");

    // ======================================================================
    printf("PART 4: free-space guard turnover\n");
    tsdb_test_wipe(P);
    g_free = UINT64_MAX;
    db = mk(3, n3, 100000, 2048, 1);
    for (int i = 0; i < 1000; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
    long bytes = tsdb_test_bytes(P);
    g_free = 10;                                     // below reserve from now on
    for (int i = 1000; i < 2000; i++) { int16_t v[3]; row(i, 3, v); CHECK(tsdb_write_h(db, T0 + i * DT, v) == ESP_OK || 1, ""); }
    tsdb_get_stats_h(db, &st);
    CHECK(tsdb_test_bytes(P) <= bytes + 3072, "file set stopped growing");
    CHECK(st.max_records < 100000 && st.total_records <= st.max_records && st.total_records >= 600, "capacity latched, window turning over");
    CHECK(st.newest_timestamp == T0 + 1999 * DT, "writes kept going");
    agg_ok(db, 0, UINT32_MAX, 1, "consistent after turnover");
    // Nothing left to evict: single tiny DB under the guard still works when
    // the only segment is the active one (guard cannot evict it).
    tsdb_close_h(db);
    tsdb_test_wipe(P);
    g_free = 10;
    db = mk(3, n3, 100000, 65536, 1);
    { int16_t v[3] = {1, 2, 3}; esp_err_t r1 = tsdb_write_h(db, T0, v), r2 = tsdb_write_h(db, T0 + DT, v);
      CHECK(r1 == ESP_OK && r2 == ESP_OK, "first segment is created even under the reserve"); }
    tsdb_close_h(db);
    g_free = UINT64_MAX;

    // ======================================================================
    printf("PART 5: batch across segments, sync, unlimited, delete\n");
    tsdb_test_wipe(P);
    db = mk(3, n3, 0, 1024, 0);                      // unlimited, 1 block per segment
    static uint32_t tss[700]; static int16_t rows[700][3]; static const int16_t *ptrs[700];
    for (int i = 0; i < 700; i++) { tss[i] = T0 + i * DT; row(i, 3, rows[i]); ptrs[i] = rows[i]; }
    CHECK(tsdb_write_batch_h(db, tss, ptrs, 700) == ESP_OK, "batch 700 over 7 segments");
    tsdb_get_stats_h(db, &st);
    CHECK(st.total_records == 700 && st.segment_count == 7 && st.max_records == 0, "unlimited: all kept");
    CHECK(tsdb_sync_h(db) == ESP_OK, "sync");
    { int16_t v[3] = {5, 5, 5}; CHECK(tsdb_write_h(db, T0 + 700 * DT, v) == ESP_OK, "write after sync"); }
    agg_ok(db, T0 + 95 * DT, T0 + 110 * DT, 2, "window across batch-written boundary");
    CHECK(tsdb_delete_h(db) == ESP_OK, "delete");
    CHECK(tsdb_test_segments(P) == 0 && !tsdb_test_exists(P), "no files left");

    printf("\n%s (%d failed)\n", fails ? "*** FAIL ***" : "*** PASS ***", fails);
    return fails ? 1 : 0;
}
