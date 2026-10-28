// Host regression for the v2.5 changes:
//   - O(log n) count / FIRST / LAST and block-level SUM/AVG/MIN/MAX agree
//     with a brute-force reference, on wrapped, unwrapped and unlimited DBs,
//     with and without legacy overflow extras.
//   - Query early-outs for windows outside the retained range.
//   - tsdb_query_set_stride returns every N-th in-range record.
//   - tsdb_write_batch_h produces a file that queries identically to the same
//     records written one at a time (growth, wrap, index boundaries), and
//     the write cache survives interleaved single writes.
//   - tsdb_clear_h recreates the file (size shrinks, sidecars gone, V4 names
//     kept, writes work afterwards).
//   - TSDB_SCHEMA_MIGRATE at open: drop/add/reorder by name, no-op when the
//     names match, strict still refuses, rename keeps history.
//   - tsdb_rename_param / tsdb_find_param.
// Build with ./build_v25.sh.
#include "esp_tsdb.h"
#include "testutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define T0 100000u
#define DT 10u
#define PA "/tmp/v25a.tsdb"
#define PB "/tmp/v25b.tsdb"
#define PC "/tmp/v25c.tsdb"

static int fails = 0;
#define CHECK(cond, msg) do { \
    int _ok = (cond); \
    printf("  [%-40s] %s\n", msg, _ok ? "PASS" : "FAIL"); \
    if (!_ok) fails++; \
} while (0)

static void wipe(const char *p) { tsdb_test_wipe(p); }
static long fsize(const char *p) { return tsdb_test_bytes(p); }

static int exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static tsdb_t *mk(const char *path, uint8_t np, const char **names, uint32_t maxrec,
                  tsdb_schema_policy_t pol) {
    tsdb_config_t c = {0};
    c.filepath = path; c.num_params = np; c.param_names = names;
    c.max_records = maxrec; c.index_stride = 10;
    c.buffer_pool_size = 16 * 1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    c.schema_policy = pol;
    return tsdb_open(&c);
}

// Row i for a 3-column DB: {i, 1000+i, (i*7)%50 - 25}
static void row3(int i, int16_t *v) {
    v[0] = (int16_t)i; v[1] = (int16_t)(1000 + i); v[2] = (int16_t)((i * 7) % 50 - 25);
}

// Brute-force reference: iterate with the public iterator, all params.
typedef struct { uint32_t n; int64_t sum; int32_t mn, mx; int16_t first, last; } ref_t;
static ref_t ref_scan(tsdb_t *db, uint32_t s, uint32_t e, uint8_t col) {
    ref_t r = {0, 0, INT32_MAX, INT32_MIN, 0, 0};
    tsdb_query_t q;
    if (tsdb_query_init_h(db, &q, s, e, NULL, 0) != ESP_OK) return r;
    uint32_t ts; int16_t v[64];
    while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
        if (r.n == 0) r.first = v[col];
        r.last = v[col]; r.sum += v[col];
        if (v[col] < r.mn) r.mn = v[col];
        if (v[col] > r.mx) r.mx = v[col];
        r.n++;
    }
    tsdb_query_close(&q);
    return r;
}

static int agg_matches(tsdb_t *db, uint32_t s, uint32_t e, uint8_t col, const char *label) {
    ref_t r = ref_scan(db, s, e, col);
    uint32_t cnt = 0xFFFFFFFF;
    int32_t sum = 0, avg = 0, mn = 0, mx = 0, c = 0, fi = 0, la = 0;
    int ok = 1;
    ok &= tsdb_query_count_h(db, s, e, &cnt) == ESP_OK && cnt == r.n;
    ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_COUNT, &c) == ESP_OK && (uint32_t)c == r.n;
    ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_SUM, &sum) == ESP_OK && sum == (int32_t)r.sum;
    ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_FIRST, &fi) == ESP_OK && fi == r.first;
    ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_LAST, &la) == ESP_OK && la == r.last;
    if (r.n > 0) {
        ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_AVG, &avg) == ESP_OK && avg == (int32_t)(r.sum / r.n);
        ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_MIN, &mn) == ESP_OK && mn == r.mn;
        ok &= tsdb_aggregate_h(db, s, e, col, TSDB_AGG_MAX, &mx) == ESP_OK && mx == r.mx;
    }
    // multi: SUM + MIN + LAST on the same column plus COUNT on column 0
    tsdb_agg_request_t rq[4] = {
        {col, TSDB_AGG_SUM, 0}, {col, TSDB_AGG_MIN, 0}, {col, TSDB_AGG_LAST, 0}, {0, TSDB_AGG_COUNT, 0}};
    uint32_t rc = 0;
    ok &= tsdb_aggregate_multi_h(db, s, e, rq, 4, &rc) == ESP_OK && rc == r.n;
    if (r.n > 0) {
        ok &= rq[0].result == (int32_t)r.sum && rq[1].result == r.mn &&
              rq[2].result == r.last && rq[3].result == (int32_t)r.n;
    }
    printf("  [%-40s] %s  (n=%u sum=%lld)\n", label, ok ? "PASS" : "FAIL",
           (unsigned)r.n, (long long)r.sum);
    if (!ok) fails++;
    return ok;
}

static void agg_suite(tsdb_t *db, uint32_t oldest, uint32_t newest, const char *tag) {
    char l[64];
    snprintf(l, sizeof l, "%s: full range", tag);        agg_matches(db, 0, UINT32_MAX, 2, l);
    snprintf(l, sizeof l, "%s: exact bounds", tag);      agg_matches(db, oldest, newest, 2, l);
    snprintf(l, sizeof l, "%s: middle window", tag);     agg_matches(db, oldest + 123, newest - 77, 1, l);
    snprintf(l, sizeof l, "%s: single record", tag);     agg_matches(db, oldest + 50, oldest + 50, 0, l);
    snprintf(l, sizeof l, "%s: between samples", tag);   agg_matches(db, oldest + 51, oldest + 59, 0, l);
    snprintf(l, sizeof l, "%s: before oldest", tag);     agg_matches(db, 0, oldest - 1, 2, l);
    snprintf(l, sizeof l, "%s: after newest", tag);      agg_matches(db, newest + 1, newest + 999, 2, l);
    snprintf(l, sizeof l, "%s: tail window", tag);       agg_matches(db, newest - 205, UINT32_MAX, 2, l);
    snprintf(l, sizeof l, "%s: head window", tag);       agg_matches(db, 1, oldest + 205, 2, l);
}

int main(void) {
    const char *n3[] = {"a", "b", "c"};
    wipe(PA); wipe(PB); wipe(PC);

    // ========================================================================
    printf("PART 1: aggregates vs brute force\n");
    // Wrapped ring: 3 cols, max 150, 400 writes -> retained 250..399
    tsdb_t *db = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    if (!db) { printf("open FAIL\n"); return 2; }
    for (int i = 0; i < 400; i++) { int16_t v[3]; row3(i, v); tsdb_write_h(db, T0 + i * DT, v); }
    agg_suite(db, T0 + 250 * DT, T0 + 399 * DT, "wrapped");
    tsdb_close_h(db);

    // Unlimited: 3 cols, 300 writes
    db = mk(PB, 3, n3, 0, TSDB_SCHEMA_STRICT);
    for (int i = 0; i < 300; i++) { int16_t v[3]; row3(i, v); tsdb_write_h(db, T0 + i * DT, v); }
    agg_suite(db, T0, T0 + 299 * DT, "unlimited");
    tsdb_close_h(db);

    // Not wrapped + overflow extras (per-record aggregate path): 2 base + 1
    // extra added at write 100, 260 writes into a 1000-ring.
    const char *n2[] = {"a", "b"};
    db = mk(PC, 2, n2, 1000, TSDB_SCHEMA_STRICT);
    for (int i = 0; i < 260; i++) {
        if (i == 100) { const char *x[] = {"c"}; tsdb_add_extra_params_h(db, x, 1); }
        int16_t v[3]; row3(i, v);
        tsdb_write_h(db, T0 + i * DT, v);
    }
    agg_suite(db, T0, T0 + 259 * DT, "extras");
    {
        // extras read correctly in a plain query (one read per record)
        tsdb_query_t q; uint8_t cols[2] = {2, 0};
        tsdb_query_init_h(db, &q, T0 + 150 * DT, T0 + 152 * DT, cols, 2);
        uint32_t ts; int16_t v[2]; int n = 0, ok = 1;
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
            int16_t r[3]; row3(150 + n, r);
            ok &= (v[0] == r[2] && v[1] == r[0]);
            n++;
        }
        tsdb_query_close(&q);
        CHECK(n == 3 && ok, "extras: column values via query");
    }
    tsdb_close_h(db);

    // ========================================================================
    printf("PART 2: stride\n");
    tsdb_config_t ca = {0};
    db = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    {
        tsdb_query_t q;
        tsdb_query_init_h(db, &q, T0 + 300 * DT, T0 + 399 * DT, NULL, 0);
        tsdb_query_set_stride(&q, 7);
        uint32_t ts; int16_t v[3]; int n = 0, ok = 1;
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
            ok &= (ts == T0 + (300 + 7 * n) * DT) && (v[0] == 300 + 7 * n);
            n++;
        }
        tsdb_query_close(&q);
        CHECK(ok && n == 15, "stride 7 over 100 records -> 15 rows");

        tsdb_query_init_h(db, &q, 0, UINT32_MAX, NULL, 0);
        tsdb_query_set_stride(&q, 1000);
        n = 0;
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) n++;
        tsdb_query_close(&q);
        CHECK(n == 1, "stride > window -> first record only");
    }
    tsdb_close_h(db);
    (void)ca;

    // ========================================================================
    printf("PART 3: batch write equivalence\n");
    wipe(PA); wipe(PB);
    // A: single writes. B: batches of mixed sizes. 3 cols, ring 150 (rpb=101),
    // 400 records: growth, block boundaries, wrap, index stride boundaries.
    tsdb_t *A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    tsdb_t *B = mk(PB, 3, n3, 150, TSDB_SCHEMA_STRICT);
    static uint32_t tss[400]; static int16_t rows[400][3]; static const int16_t *ptrs[400];
    for (int i = 0; i < 400; i++) { tss[i] = T0 + i * DT; row3(i, rows[i]); ptrs[i] = rows[i]; }
    for (int i = 0; i < 400; i++) tsdb_write_h(A, tss[i], rows[i]);
    {
        int i = 0; const int sizes[] = {1, 37, 2, 150, 99, 5, 60, 46};
        int k = 0;
        while (i < 400) {
            int n = sizes[k++ % 8]; if (i + n > 400) n = 400 - i;
            if (n == 1) tsdb_write_h(B, tss[i], rows[i]);          // interleave singles
            else CHECK(tsdb_write_batch_h(B, &tss[i], &ptrs[i], n) == ESP_OK, "batch call OK");
            i += n;
        }
    }
    {
        tsdb_stats_t sa, sb;
        tsdb_get_stats_h(A, &sa); tsdb_get_stats_h(B, &sb);
        CHECK(sa.total_records == sb.total_records && sa.total_evictions == sb.total_evictions &&
              sa.oldest_timestamp == sb.oldest_timestamp && sa.newest_timestamp == sb.newest_timestamp &&
              sa.total_writes == sb.total_writes, "stats identical");
        tsdb_query_t qa, qb;
        tsdb_query_init_h(A, &qa, 0, UINT32_MAX, NULL, 0);
        tsdb_query_init_h(B, &qb, 0, UINT32_MAX, NULL, 0);
        uint32_t ta, tb; int16_t va[3], vb[3]; int n = 0, ok = 1;
        for (;;) {
            esp_err_t ra = tsdb_query_next(&qa, &ta, va), rb = tsdb_query_next(&qb, &tb, vb);
            if (ra != ESP_OK || rb != ESP_OK) { ok &= (ra == rb); break; }
            ok &= (ta == tb) && memcmp(va, vb, sizeof va) == 0 && (int)ta == (int)(T0 + (250 + n) * DT);
            n++;
        }
        tsdb_query_close(&qa); tsdb_query_close(&qb);
        CHECK(ok && n == 150, "all 150 retained rows identical");
    }
    tsdb_close_h(A); tsdb_close_h(B);
    CHECK(fsize(PA) == fsize(PB), "storage sizes identical");
    // Batch survives close/reopen.
    B = mk(PB, 3, n3, 150, TSDB_SCHEMA_STRICT);
    { tsdb_stats_t s; tsdb_get_stats_h(B, &s); CHECK(s.total_records == 150 && s.total_writes == 400, "batch state persisted"); }
    tsdb_close_h(B);

    // ========================================================================
    printf("PART 4: clear removes the files\n");
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    long before = fsize(PA);
    tsdb_write_h(A, T0 + 500 * DT, rows[0]);
    CHECK(tsdb_clear_h(A) == ESP_OK, "clear OK");
    long after = fsize(PA);
    CHECK(after < before && after <= 2048, "storage released (only an empty segment left)");
    {
        CHECK(tsdb_test_segments(PA) <= 1, "old segments removed");
        tsdb_stats_t s; tsdb_get_stats_h(A, &s);
        CHECK(s.total_records == 0 && s.max_records == 150 && s.num_params == 3, "empty, geometry kept");
        uint32_t cnt = 99; tsdb_query_count_h(A, 0, UINT32_MAX, &cnt);
        CHECK(cnt == 0, "count 0 after clear");
        for (int i = 0; i < 160; i++) tsdb_write_h(A, T0 + i * DT, rows[i]);
        agg_matches(A, 0, UINT32_MAX, 2, "writes + wrap after clear");
        tsdb_get_stats_h(A, &s);
        CHECK(s.total_records == 150 && s.total_evictions == 10, "ring state after clear+refill");
    }
    tsdb_close_h(A);
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    { tsdb_stats_t s; tsdb_get_stats_h(A, &s); CHECK(s.total_records == 150, "reopen after clear+refill"); }
    tsdb_close_h(A);

    // Wide (V4) clear keeps extended names
    wipe(PC);
    const char *w[20]; static char wb[20][8];
    for (int i = 0; i < 20; i++) { snprintf(wb[i], 8, "w%02d", i); w[i] = wb[i]; }
    tsdb_t *W = mk(PC, 20, w, 100, TSDB_SCHEMA_STRICT);
    { int16_t v[20] = {0}; tsdb_write_h(W, T0, v); }
    CHECK(tsdb_clear_h(W) == ESP_OK, "wide clear OK");
    { const char *nm = tsdb_get_param_name_h(W, 18); CHECK(nm && strcmp(nm, "w18") == 0, "wide ext names kept"); }
    tsdb_close_h(W);

    // ========================================================================
    printf("PART 5: schema policy at open\n");
    wipe(PA);
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    for (int i = 0; i < 200; i++) tsdb_write_h(A, tss[i], rows[i]);   // wrapped: 50..199, writes=200
    tsdb_close_h(A);

    const char *n4[] = {"a", "b", "c", "d"};
    A = mk(PA, 4, n4, 150, TSDB_SCHEMA_STRICT);
    CHECK(A != NULL, "3.0: count mismatch with names opens (by-name map)");
    if (A) {
        tsdb_stats_t s; tsdb_get_stats_h(A, &s);
        CHECK(s.total_records == 150 && s.num_params == 4, "history visible under the wider schema");
        tsdb_close_h(A);
    }
    // Back to 3 columns for the rest of the part (just a schema, nothing moves).

    // MIGRATE with identical names -> no rewrite (total_writes stays 200)
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_MIGRATE);
    CHECK(A != NULL, "migrate policy, same schema opens");
    if (A) { tsdb_stats_t s; tsdb_get_stats_h(A, &s); CHECK(s.total_writes == 200, "same schema -> no rewrite"); tsdb_close_h(A); }

    // Reorder + drop + add: [c, a, d]
    const char *n_cad[] = {"c", "a", "d"};
    A = mk(PA, 3, n_cad, 150, TSDB_SCHEMA_MIGRATE);
    CHECK(A != NULL, "migrate policy: reorder/drop/add opens");
    if (A) {
        tsdb_stats_t s; tsdb_get_stats_h(A, &s);
        CHECK(s.total_records == 150 && s.num_params == 3 && s.total_writes == 200, "150 rows kept, lifetime count intact");
        tsdb_query_t q; tsdb_query_init_h(A, &q, tss[123], tss[123], NULL, 0);
        uint32_t ts; int16_t v[3]; int got = tsdb_query_next(&q, &ts, v) == ESP_OK;
        tsdb_query_close(&q);
        int16_t r[3]; row3(123, r);
        CHECK(got && v[0] == r[2] && v[1] == r[0] && v[2] == 0, "c,a carried; d zero");
        CHECK(tsdb_find_param_h(A, "d") == 2 && tsdb_find_param_h(A, "b") == -1, "find_param");
        // rename d -> e in place, then reopen with [c, a, e]: history kept
        int16_t nv[3] = {7, 8, 9};
        tsdb_write_h(A, tss[300], nv);
        CHECK(tsdb_rename_param_h(A, 2, "e") == ESP_OK, "rename d -> e");
        CHECK(strcmp(tsdb_get_param_name_h(A, 2), "e") == 0, "name visible immediately");
        tsdb_close_h(A);
    }
    const char *n_cae[] = {"c", "a", "e"};
    A = mk(PA, 3, n_cae, 150, TSDB_SCHEMA_MIGRATE);
    CHECK(A != NULL, "reopen under renamed schema");
    if (A) {
        tsdb_stats_t s; tsdb_get_stats_h(A, &s);
        CHECK(s.total_writes == 201, "rename needed no rewrite");
        tsdb_query_t q; tsdb_query_init_h(A, &q, tss[300], tss[300], NULL, 0);
        uint32_t ts; int16_t v[3]; int got = tsdb_query_next(&q, &ts, v) == ESP_OK;
        tsdb_query_close(&q);
        CHECK(got && v[2] == 9, "renamed column kept its data");
        tsdb_close_h(A);
    }
    // Strict reopen with the matching count still works (names not compared)
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    CHECK(A != NULL, "strict reopen with equal count");
    if (A) tsdb_close_h(A);
    // MIGRATE without names falls back to strict refusal on count mismatch
    CHECK(mk(PA, 4, NULL, 150, TSDB_SCHEMA_MIGRATE) == NULL, "migrate w/o names refuses mismatch");

    // Fresh file under MIGRATE policy: plain create
    wipe(PB);
    B = mk(PB, 4, n4, 100, TSDB_SCHEMA_MIGRATE);
    CHECK(B != NULL, "fresh create under migrate policy");
    if (B) tsdb_close_h(B);

    // ========================================================================
    printf("PART 6: pre-existing bugs exposed by the above\n");
    // 6a. A ring that wrapped more than twice must reopen with its header
    //     intact (lifetime total_records > 2*max_records used to be flagged
    //     "insane" -> header reconstruction -> scrambled ring cursor).
    wipe(PA);
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    for (int i = 0; i < 400; i++) tsdb_write_h(A, tss[i], rows[i]);
    tsdb_close_h(A);
    A = mk(PA, 3, n3, 150, TSDB_SCHEMA_STRICT);
    {
        tsdb_stats_t s; tsdb_get_stats_h(A, &s);
        CHECK(s.total_writes == 400 && s.total_evictions == 250 && s.total_records == 150,
              "wrapped x2: header survives reopen");
        // and the ring keeps its time order through further writes
        for (int i = 400; i < 470; i++) { int16_t v[3]; row3(i, v); tsdb_write_h(A, T0 + i * DT, v); }
        agg_matches(A, 0, UINT32_MAX, 0, "wrapped x2: order kept after reopen");
        tsdb_query_t q; tsdb_query_init_h(A, &q, 0, UINT32_MAX, NULL, 0);
        uint32_t ts, prev = 0; int16_t v[3]; int mono = 1, n = 0;
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) { if (n && ts <= prev) mono = 0; prev = ts; n++; }
        tsdb_query_close(&q);
        CHECK(mono && n == 150 && prev == T0 + 469 * DT, "wrapped x2: monotonic, newest last");
    }
    tsdb_close_h(A);

    // 6b. Overflow extras added while the ring is still growing must not be
    //     overwritten by later data blocks (region used to be placed at the
    //     then-EOF, inside the data region's growth envelope).
    wipe(PC);
    tsdb_t *O = mk(PC, 2, n2, 1000, TSDB_SCHEMA_STRICT);
    for (int i = 0; i < 700; i++) {
        if (i == 100) { const char *x[] = {"c"}; CHECK(tsdb_add_extra_params_h(O, x, 1) == ESP_OK, "add extras on growing ring"); }
        int16_t v[3]; row3(i, v);
        tsdb_write_h(O, T0 + i * DT, v);
    }
    {
        tsdb_query_t q; uint8_t cols[3] = {0, 1, 2};
        tsdb_query_init_h(O, &q, 0, UINT32_MAX, cols, 3);
        uint32_t ts; int16_t v[3]; int n = 0, ok = 1;
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
            int i = (int)((ts - T0) / DT); int16_t r[3]; row3(i, r);
            ok &= (v[0] == r[0] && v[1] == r[1] && v[2] == (i >= 100 ? r[2] : 0));
            n++;
        }
        tsdb_query_close(&q);
        CHECK(ok && n == 700, "extras intact after 6 blocks of growth");
    }
    tsdb_close_h(O);
    // 3.0: extras are ordinary columns, so an unlimited DB takes them too
    wipe(PB);
    B = mk(PB, 2, n2, 0, TSDB_SCHEMA_STRICT);
    { const char *x[] = {"c"}; CHECK(tsdb_add_extra_params_h(B, x, 1) == ESP_OK, "unlimited DB accepts extras"); }
    tsdb_close_h(B);

    // 6c. A legacy file with the index at 512 (pre-2.2 layout) is adopted,
    //     written past, and gone after a clear.
    wipe(PB);
    {
        tsdb_header_t h; memset(&h, 0, sizeof h);
        h.magic = TSDB_MAGIC; h.version = TSDB_VERSION; h.num_params = 2; h.base_params = 2;
        h.param_size = 2; h.record_size = 8; h.records_per_block = (TSDB_BLOCK_SIZE - 8) / 8;
        h.max_records = 100; h.index_stride = 10; h.index_offset = 512; h.index_entries = 11;
        strcpy(h.param_names[0], "a"); strcpy(h.param_names[1], "b");
        FILE *f = fopen(PB, "wb");
        fwrite(&h, sizeof h, 1, f);
        fseek(f, 512 + 11 * 8, SEEK_SET);
        uint8_t z[TSDB_BLOCK_SIZE] = {0}; fwrite(z, sizeof z, 1, f);   // one empty block
        fclose(f);
    }
    B = mk(PB, 2, n2, 100, TSDB_SCHEMA_STRICT);
    CHECK(B != NULL, "legacy 512-offset file opens");
    if (B) {
        int16_t v[2] = {1, 2};
        tsdb_write_h(B, T0, v);
        CHECK(tsdb_clear_h(B) == ESP_OK, "legacy clear OK");
        tsdb_write_h(B, T0 + DT, v);
        tsdb_close_h(B);
        CHECK(!tsdb_test_exists(PB), "legacy file gone after clear");
        B = mk(PB, 2, n2, 100, TSDB_SCHEMA_STRICT);
        uint32_t cnt = 0; tsdb_query_count_h(B, 0, UINT32_MAX, &cnt);
        CHECK(cnt == 1, "record readable after legacy clear + reopen");
        tsdb_close_h(B);
    }

    printf("\n%s (%d failed)\n", fails ? "*** FAIL ***" : "*** PASS ***", fails);
    return fails ? 1 : 0;
}
