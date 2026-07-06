// Host regression for tsdb_resize_h (v2.3): live ring-capacity changes.
// Covers: fast header-only grow on an unwrapped DB (incl. writing past the
// old capacity and past the preallocated index region — exercises the
// index-write bounds guard), grow of a wrapped ring (streaming rewrite),
// shrink keeping the newest records, overflow-extras fold on resize, and
// no-op resize. Build with ./build_resize.sh.
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0 100000u
#define DT 10u
#define PATH "/tmp/resize.tsdb"

static int fails = 0;
#define CHECK(cond, msg) do { \
    int _ok = (cond); \
    printf("  [%-36s] %s\n", msg, _ok ? "PASS" : "FAIL"); \
    if (!_ok) fails++; \
} while (0)

// Count records over [s,e]; verify ascending; capture first/last first-column.
static int qscan(tsdb_t *db, uint32_t s, uint32_t e, int16_t *first0, int16_t *last0) {
    tsdb_query_t q;
    if (tsdb_query_init_h(db, &q, s, e, NULL, 0) != ESP_OK) return -1;
    uint32_t ts; int16_t v[8]; int n = 0; uint32_t prev = 0;
    while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
        if (n && ts <= prev) { tsdb_query_close(&q); return -2; }  // non-monotonic
        if (n == 0 && first0) *first0 = v[0];
        if (last0) *last0 = v[0];
        prev = ts; n++;
    }
    tsdb_query_close(&q);
    return n;
}

static tsdb_t *mk(uint32_t maxrec, uint8_t np, const char **names, int nwrite) {
    remove(PATH); remove(PATH ".mig");
    tsdb_config_t c = {0};
    c.filepath = PATH; c.num_params = np; c.max_records = maxrec;
    c.index_stride = 10; c.buffer_pool_size = 16 * 1024;
    c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM; c.param_names = names;
    tsdb_t *db = tsdb_open(&c);
    if (!db) { printf("open FAIL\n"); exit(2); }
    for (int i = 0; i < nwrite; i++) {
        int16_t v[8];
        for (uint8_t p = 0; p < np; p++) v[p] = (int16_t)(i + p * 1000);
        if (tsdb_write_h(db, T0 + (uint32_t)i * DT, v) != ESP_OK) {
            printf("write %d FAIL\n", i); exit(2);
        }
    }
    return db;
}

int main(void) {
    const char *two[] = {"soc", "vbat"};
    tsdb_stats_t st = {0};
    int16_t f0, l0; int n;

    // ---- 1. Fast grow: unwrapped, 30/50 records, grow to 300, keep writing
    //         past both the old capacity (50) and the old index region
    //         (6 entries * stride 10 = 60 records) — the bounds guard must
    //         silently stop indexing, never corrupt data blocks.
    printf("PART 1: fast grow (header-only) + index guard\n");
    tsdb_t *db = mk(50, 2, two, 30);
    CHECK(tsdb_resize_h(db, 300, NULL) == ESP_OK, "grow 50 -> 300 succeeds");
    tsdb_get_stats_h(db, &st);
    CHECK(st.max_records == 300, "max_records updated");
    CHECK(st.total_records == 30, "no data touched");
    for (int i = 30; i < 150; i++) {
        int16_t v[2] = {(int16_t)i, (int16_t)(i + 1000)};
        if (tsdb_write_h(db, T0 + (uint32_t)i * DT, v) != ESP_OK) {
            printf("write %d FAIL\n", i); exit(2);
        }
    }
    n = qscan(db, 0, 999999u, &f0, &l0);
    CHECK(n == 150 && f0 == 0 && l0 == 149, "150 records intact past old capacity");
    tsdb_close_h(db);

    // Reopen: header change persisted; data still clean.
    tsdb_config_t cr = {0};
    cr.filepath = PATH; cr.num_params = 2; cr.max_records = 300;
    cr.index_stride = 10; cr.buffer_pool_size = 16 * 1024;
    cr.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM; cr.param_names = two;
    db = tsdb_open(&cr);
    CHECK(db != NULL, "reopen after fast grow");
    tsdb_get_stats_h(db, &st);
    CHECK(st.max_records == 300 && st.total_records == 150, "grown capacity persists");
    n = qscan(db, 0, 999999u, &f0, &l0);
    CHECK(n == 150 && f0 == 0, "data intact after reopen");

    // No-op resize.
    CHECK(tsdb_resize_h(db, 300, NULL) == ESP_OK, "no-op resize OK");
    tsdb_close_h(db);

    // ---- 2. Wrapped grow: 100 writes into a 60-ring, grow to 200 (rewrite).
    printf("PART 2: wrapped grow (rewrite)\n");
    db = mk(60, 2, two, 100);
    CHECK(tsdb_resize_h(db, 200, NULL) == ESP_OK, "grow wrapped 60 -> 200");
    tsdb_get_stats_h(db, &st);
    CHECK(st.max_records == 200, "capacity now 200");
    CHECK(st.total_records == 60, "retained 60 records");
    n = qscan(db, 0, 999999u, &f0, &l0);
    CHECK(n == 60 && f0 == 40 && l0 == 99, "records 40..99 survive, ordered");
    // Ring continues cleanly after the rewrite.
    for (int i = 100; i < 130; i++) {
        int16_t v[2] = {(int16_t)i, (int16_t)(i + 1000)};
        tsdb_write_h(db, T0 + (uint32_t)i * DT, v);
    }
    n = qscan(db, 0, 999999u, &f0, &l0);
    CHECK(n == 90 && f0 == 40 && l0 == 129, "writes continue after grow");

    // ---- 3. Shrink: 90 retained -> keep newest 25.
    printf("PART 3: shrink keeps newest\n");
    CHECK(tsdb_resize_h(db, 25, NULL) == ESP_OK, "shrink to 25");
    tsdb_get_stats_h(db, &st);
    CHECK(st.max_records == 25 && st.total_records == 25, "capacity + count = 25");
    n = qscan(db, 0, 999999u, &f0, &l0);
    CHECK(n == 25 && f0 == 105 && l0 == 129, "newest 25 kept (105..129)");
    tsdb_close_h(db);

    // ---- 4. Overflow extras force the rewrite path and get folded.
    printf("PART 4: resize folds extras\n");
    db = mk(100, 2, two, 20);
    const char *extras[] = {"tbat"};
    CHECK(tsdb_add_extra_params_h(db, extras, 1) == ESP_OK, "add extra");
    for (int i = 20; i < 40; i++) {
        int16_t v[3] = {(int16_t)i, (int16_t)(i + 1000), (int16_t)(i + 2000)};
        tsdb_write_h(db, T0 + (uint32_t)i * DT, v);
    }
    CHECK(tsdb_resize_h(db, 500, NULL) == ESP_OK, "resize with extras (rewrite)");
    tsdb_get_stats_h(db, &st);
    CHECK(st.max_records == 500, "capacity 500");
    CHECK(st.num_params == 3 && st.extra_params == 0, "extras folded to base");
    tsdb_query_t q;
    uint32_t ts; int16_t v3[3];
    tsdb_query_init_h(db, &q, T0 + 30 * DT, T0 + 30 * DT, NULL, 0);
    n = 0;
    while (tsdb_query_next(&q, &ts, v3) == ESP_OK) n++;
    tsdb_query_close(&q);
    CHECK(n == 1 && v3[2] == 2030, "extra value survives fold");
    tsdb_close_h(db);

    printf("\n%s (%d failed)\n", fails ? "*** FAIL ***" : "*** PASS ***", fails);
    return fails ? 1 : 0;
}
