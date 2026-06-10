// Host regression for PR#1's handle-based multi-instance API: two databases
// open at once, written + queried independently, proving handle isolation.
// Also re-checks the wrapped-ring fix runs through the _h path (not just the
// legacy global wrapper). Build with ./build_multi.sh.
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>

#define T0 100000u
#define DT 10u

static tsdb_t *mkdb(const char *path, uint32_t maxrec, int nwrite, int16_t base) {
    remove(path);
    tsdb_config_t c = {0};
    c.filepath = path; c.num_params = 1; c.max_records = maxrec; c.index_stride = 10;
    c.buffer_pool_size = 16*1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    tsdb_t *db = tsdb_open(&c);
    if (db == NULL) { printf("tsdb_open(%s) FAIL\n", path); exit(2); }
    for (int i = 0; i < nwrite; i++) {
        int16_t val = (int16_t)(base + i);
        uint32_t ts = T0 + (uint32_t)i*DT;
        if (tsdb_write_h(db, ts, &val) != ESP_OK) { printf("write_h(%s) FAIL\n", path); exit(2); }
    }
    return db;
}

// Range query over a handle; returns count, asserts ascending + in-range, and
// (optionally) checks the first value equals expect_first_val to prove handle
// isolation (each DB was seeded with a different base value).
static int q(tsdb_t *db, uint32_t s, uint32_t e, const char *label,
             int expect, int expect_first_val) {
    tsdb_query_t qy; uint8_t cols[1] = {0};
    if (tsdb_query_init_h(db, &qy, s, e, cols, 1) != ESP_OK) { printf("  [%s] init FAIL\n", label); return 1; }
    uint32_t ts; int16_t v[4]; int n=0; uint32_t prev=0, first=0, last=0; int mono=1, inr=1, firstval=-99999;
    while (tsdb_query_next(&qy, &ts, v) == ESP_OK) {
        if (n==0) { first=ts; firstval=v[0]; }
        last=ts;
        if (n && ts<=prev) mono=0;
        if (ts<s || ts>e) inr=0;
        prev=ts; n++;
    }
    tsdb_query_close(&qy);
    int valok = (expect_first_val == -99999) || (n==0) || (firstval==expect_first_val);
    int ok = (n==expect) && mono && inr && valok;
    printf("  [%-10s] [%u,%u] got=%d exp=%d firstval=%d %s%s%s-> %s\n",
           label, s, e, n, expect, firstval, mono?"":"NONMONO ", inr?"":"OOR ",
           valok?"":"VALMISMATCH ", ok?"PASS":"FAIL");
    return ok ? 0 : 1;
}

int main(void) {
    int f = 0;

    printf("MULTI-HANDLE: two DBs open simultaneously, independent data\n");
    // DB A: values start at 1000.  DB B: values start at 5000.
    tsdb_t *A = mkdb("/tmp/m_a.tsdb", 100, 250, 1000); // wrapped ring
    tsdb_t *B = mkdb("/tmp/m_b.tsdb", 0,   200, 5000); // unlimited

    // Both handles live at once. Query each; isolation = A never sees B's values.
    // A wrapped: retained rec 150..249 -> values 1150..1249. recent[102200,102490]=rec220..249 -> first val 1220.
    f += q(A, 102200, 103000, "A-recent", 30, 1220);
    f += q(A, 0,      200000, "A-full",  100, 1150);   // oldest retained value = 1000+150
    // B unlimited: all 200, first value 5000.
    f += q(B, 0,      999999, "B-all",   200, 5000);
    f += q(B, 100500, 100700, "B-mid",    21, -99999); // value check skipped, just range/count

    // Interleave: write more to A while B is still open, re-query B unchanged.
    int16_t x = 9999; tsdb_write_h(A, T0 + 250*DT, &x);
    f += q(B, 101990, 101990, "B-last",    1, 5199);   // B untouched by A's write

    // Stats per handle are independent.
    tsdb_stats_t sa = {0}, sb = {0};
    tsdb_get_stats_h(A, &sa);
    tsdb_get_stats_h(B, &sb);
    printf("  stats: A.total=%lu  B.total=%lu  %s\n",
           (unsigned long)sa.total_records, (unsigned long)sb.total_records,
           (sb.total_records == 200) ? "PASS" : "FAIL");
    if (sb.total_records != 200) f++;

    tsdb_close_h(A);
    tsdb_close_h(B);
    tsdb_close_h(NULL); // PR#1 says this is now a safe no-op

    printf("\n%s (%d failed)\n", f ? "*** FAIL ***" : "*** PASS ***", f);
    return f ? 1 : 0;
}
