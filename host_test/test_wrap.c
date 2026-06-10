// Host reproduction/regression for the wrapped-ring query bug (singleton API).
// Writes > max_records records (forces LRU eviction / ring wrap), then queries
// sub-ranges and checks every in-range record is returned, in ascending order.
//
// Build with ./build.sh (gcc, ESP-IDF deps shimmed under shims/).
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>

#define T0 100000u
#define DT 10u

static int q(uint32_t s, uint32_t e, const char *label, int expect) {
    tsdb_query_t qy; uint8_t cols[1] = {0};
    if (tsdb_query_init(&qy, s, e, cols, 1) != ESP_OK) { printf("  [%s] init FAIL\n", label); return 1; }
    uint32_t ts; int16_t v[4]; int n = 0; uint32_t prev = 0, first = 0, last = 0; int mono = 1, inr = 1;
    while (tsdb_query_next(&qy, &ts, v) == ESP_OK) {
        if (n == 0) first = ts;
        last = ts;
        if (n && ts <= prev) mono = 0;
        if (ts < s || ts > e) inr = 0;
        prev = ts; n++;
    }
    tsdb_query_close(&qy);
    int ok = (n == expect) && mono && inr;
    printf("  [%-11s] [%u,%u] got=%d exp=%d first=%u last=%u %s%s-> %s\n",
           label, s, e, n, expect, first, last, mono?"":"NONMONO ", inr?"":"OOR ", ok?"PASS":"FAIL");
    return ok ? 0 : 1;
}

static void mkdb(const char *path, uint32_t maxrec, int nwrite) {
    remove(path);
    tsdb_config_t c = {0};
    c.filepath = path; c.num_params = 1; c.max_records = maxrec; c.index_stride = 10;
    c.buffer_pool_size = 16*1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    if (tsdb_init(&c) != ESP_OK) { printf("tsdb_init FAIL\n"); exit(2); }
    for (int i = 0; i < nwrite; i++) { int16_t val=(int16_t)i; uint32_t ts=T0+(uint32_t)i*DT;
        if (tsdb_write(ts, &val) != ESP_OK){ printf("write FAIL\n"); exit(2);} }
}

int main(void) {
    int f = 0;

    printf("A) WRAPPED ring (250 writes, max 100) -- the bug:\n");
    mkdb("/tmp/u_wrap.tsdb", 100, 250);   // retained ts 101500..102490 (rec 150..249)
    f += q(102200, 103000, "recent", 30); // ts 102200..102490 = rec 220..249
    f += q(101800, 102000, "mid",    21); // rec 180..200
    f += q(0,      200000, "full",  100); // all retained, monotonic
    f += q(101500, 101550, "oldedge", 6); // rec 150..155
    f += q(102490, 102490, "newest",  1);
    tsdb_close();

    printf("B) NOT wrapped (30 of 100):\n");
    mkdb("/tmp/u_nowrap.tsdb", 100, 30);  // ts 100000..100290
    f += q(0, 999999, "all", 30);
    f += q(100100, 100200, "mid", 11);
    f += q(200000, 300000, "future", 0);
    tsdb_close();

    printf("C) UNLIMITED (max=0, 200 records):\n");
    mkdb("/tmp/u_unl.tsdb", 0, 200);      // ts 100000..101990
    f += q(0, 999999, "all", 200);
    f += q(100500, 100700, "mid", 21);
    f += q(101990, 101990, "last", 1);
    tsdb_close();

    printf("\n%s (%d failed)\n", f ? "*** FAIL ***" : "*** PASS ***", f);
    return f ? 1 : 0;
}
