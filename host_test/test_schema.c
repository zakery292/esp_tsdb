// Host regression for tsdb_migrate_schema_h (v2.2): full base-column rewrite.
// Covers: column survival by name, dropped column, new zero-backfilled column,
// overflow extras folded into base columns, wrapped-ring source, identical-
// schema no-op, refusal on a too-small space budget, trim-to-fit keeping the
// newest records, post-migration writes, and .mig crash-recovery (stale
// removal + orphan adoption). Build with ./build_schema.sh.
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0 100000u
#define DT 10u
#define PATH  "/tmp/schema.tsdb"
#define PATH2 "/tmp/schema2.tsdb"

static int fails = 0;
// NB: evaluate cond exactly once — several conds have side effects (writes,
// migrations), and double-evaluation corrupts the scenario under test.
#define CHECK(cond, msg) do { \
    int _ok = (cond); \
    printf("  [%-34s] %s\n", msg, _ok ? "PASS" : "FAIL"); \
    if (!_ok) fails++; \
} while (0)

// Progress-callback capture: verify monotonic 0..total reporting.
static struct { uint32_t calls, first, last, total; int monotonic; } prog;
static void on_progress(uint32_t done, uint32_t total, void *ctx) {
    (void)ctx;
    if (prog.calls == 0) prog.first = done;
    else if (done < prog.last) prog.monotonic = 0;
    prog.last = done; prog.total = total; prog.calls++;
}

// Query all params over [s,e]; returns count; copies first row into row0.
static int qcount(tsdb_t *db, uint32_t s, uint32_t e, int16_t *row0, int ncols) {
    tsdb_query_t q;
    if (tsdb_query_init_h(db, &q, s, e, NULL, 0) != ESP_OK) return -1;
    uint32_t ts; int16_t v[64]; int n = 0;
    while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
        if (n == 0 && row0) memcpy(row0, v, ncols * sizeof(int16_t));
        n++;
    }
    tsdb_query_close(&q);
    return n;
}

int main(void) {
    remove(PATH);  remove(PATH ".mig");
    remove(PATH2); remove(PATH2 ".mig");

    // ========================================================================
    // Part 1 — correctness: wrapped ring + overflow folding
    // Source: 3 base params, ring of 60, 100 writes (wrapped at 60),
    // 2 overflow extras added at write #80.
    // ========================================================================
    const char *base[] = {"soc", "vbat", "pload"};
    tsdb_config_t c = {0};
    c.filepath = PATH; c.num_params = 3; c.max_records = 60; c.index_stride = 10;
    c.buffer_pool_size = 16 * 1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    c.param_names = base;
    tsdb_t *db = tsdb_open(&c);
    if (!db) { printf("open FAIL\n"); return 2; }

    for (int i = 0; i < 100; i++) {
        if (i == 80) {
            const char *extras[] = {"tbat", "gen"};
            if (tsdb_add_extra_params_h(db, extras, 2) != ESP_OK) {
                printf("add_extras FAIL\n"); return 2;
            }
        }
        // Row i: soc=i, vbat=1000+i, pload=2000+i, [tbat=3000+i, gen=4000+i]
        int16_t v[5] = {(int16_t)i, (int16_t)(1000 + i), (int16_t)(2000 + i),
                        (int16_t)(3000 + i), (int16_t)(4000 + i)};
        if (tsdb_write_h(db, T0 + i * DT, v) != ESP_OK) {
            printf("write %d FAIL\n", i); return 2;
        }
    }

    printf("PART 1: wrapped ring, 3 base + 2 extras -> 4 base\n");

    // Migrate: keep soc, drop vbat+pload, promote extras tbat+gen, add fresh
    // zero-backfilled column "site". Progress reported every 10 records.
    const char *new_names[] = {"soc", "tbat", "gen", "site"};
    memset(&prog, 0, sizeof(prog)); prog.monotonic = 1;
    tsdb_migrate_opts_t popts = {0};
    popts.progress = on_progress; popts.progress_every = 10;
    CHECK(tsdb_migrate_schema_h(db, new_names, 4, &popts) == ESP_OK,
          "migrate returns ESP_OK");
    CHECK(prog.calls >= 3 && prog.first == 0 && prog.last == 60 &&
          prog.total == 60 && prog.monotonic,
          "progress: 0..total, monotonic");

    tsdb_stats_t st = {0};
    tsdb_get_stats_h(db, &st);
    CHECK(st.total_records == 60, "60 records retained");
    CHECK(st.num_params == 4 && st.extra_params == 0, "4 base params, no overflow");

    // Oldest retained row is record 40 (pre-overflow: extras read as 0).
    int16_t row[4] = {0};
    int n = qcount(db, 0, 999999u, row, 4);
    CHECK(n == 60, "full query sees 60 records");
    CHECK(row[0] == 40, "soc preserved (record 40)");
    CHECK(row[1] == 0 && row[2] == 0, "pre-overflow extras backfilled 0");
    CHECK(row[3] == 0, "new column zero-backfilled");

    // A post-overflow row: record 90 at ts = T0+900 (also exercises the seek).
    n = qcount(db, T0 + 900, T0 + 900, row, 4);
    CHECK(n == 1, "single-record window (seek path)");
    CHECK(row[0] == 90 && row[1] == 3090 && row[2] == 4090,
          "promoted extras carry values");

    // Writes with the NEW schema work; migrating flag is clear.
    int16_t w[4] = {101, 3101, 4101, 7};
    CHECK(tsdb_write_h(db, T0 + 101 * DT, w) == ESP_OK, "post-migration write OK");
    n = qcount(db, T0 + 101 * DT, T0 + 101 * DT, row, 4);
    CHECK(n == 1 && row[3] == 7, "new column readable after write");

    // Idempotence: migrating to the identical schema is a no-op.
    CHECK(tsdb_migrate_schema_h(db, new_names, 4, NULL) == ESP_OK,
          "identical schema no-op");
    tsdb_close_h(db);

    // ========================================================================
    // Part 2 — space budget: refusal and trim-to-fit (newest records win)
    // Source: 2 params, 400 records, no wrap. Migrating to 3 params
    // (rpb=101, index entries (400/10)+1=41 -> fixed cost 1024+328=1352).
    // ========================================================================
    printf("PART 2: space budget + trim-to-fit\n");
    const char *two[] = {"soc", "vbat"};
    tsdb_config_t c2 = {0};
    c2.filepath = PATH2; c2.num_params = 2; c2.max_records = 400; c2.index_stride = 10;
    c2.buffer_pool_size = 16 * 1024; c2.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    c2.param_names = two;
    tsdb_t *d2 = tsdb_open(&c2);
    if (!d2) { printf("open2 FAIL\n"); return 2; }
    for (int i = 0; i < 400; i++) {
        int16_t v[2] = {(int16_t)i, (int16_t)(1000 + i)};
        if (tsdb_write_h(d2, T0 + i * DT, v) != ESP_OK) { printf("write2 FAIL\n"); return 2; }
    }

    const char *three[] = {"soc", "vbat", "extra"};
    tsdb_migrate_opts_t opts = {0};

    // 400 records at rpb=101 -> 4 blocks -> needs 1352 + 4096 = 5448 bytes.
    opts.free_space_bytes = 3000;   // 2850 after reserve: too small, no trim
    opts.allow_trim = false;
    CHECK(tsdb_migrate_schema_h(d2, three, 3, &opts) == ESP_ERR_NO_MEM,
          "refuses when budget too small");
    tsdb_get_stats_h(d2, &st);
    CHECK(st.total_records == 400 && st.num_params == 2,
          "refusal left source untouched");

    // Same budget with allow_trim: fits (2850-1352)/1024 = 1 block = 101
    // newest records (299..399).
    opts.allow_trim = true;
    CHECK(tsdb_migrate_schema_h(d2, three, 3, &opts) == ESP_OK,
          "trim-to-fit migrates");
    tsdb_get_stats_h(d2, &st);
    CHECK(st.total_records == 101, "kept newest 101 records");
    CHECK(st.max_records == 101, "ring capped to proven capacity");
    int16_t row3[3] = {0};
    n = qcount(d2, 0, 999999u, row3, 3);
    CHECK(n == 101 && row3[0] == 299 && row3[1] == 1299 && row3[2] == 0,
          "oldest survivor is record 299, remapped");
    tsdb_close_h(d2);

    // ========================================================================
    // Part 3 — .mig crash recovery
    // ========================================================================
    printf("PART 3: crash recovery\n");
    tsdb_config_t c3 = {0};
    c3.filepath = PATH; c3.num_params = 4; c3.max_records = 60; c3.index_stride = 10;
    c3.buffer_pool_size = 16 * 1024; c3.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;

    // Stale .mig alongside a healthy main file -> removed, main wins.
    FILE *f = fopen(PATH ".mig", "wb");
    fputs("garbage", f); fclose(f);
    db = tsdb_open(&c3);
    CHECK(db != NULL, "reopen with stale .mig");
    f = fopen(PATH ".mig", "rb");
    CHECK(f == NULL, "stale .mig removed");
    if (f) fclose(f);
    tsdb_get_stats_h(db, &st);
    // 61 writes into a 60-ring: stats report the retained count (= max).
    CHECK(st.total_records == 60, "data survived reopen");
    tsdb_close_h(db);

    // Orphaned complete .mig with main missing (crash in the swap window)
    // -> adopted.
    rename(PATH, PATH ".mig");
    db = tsdb_open(&c3);
    CHECK(db != NULL, "reopen adopts orphaned .mig");
    tsdb_get_stats_h(db, &st);
    CHECK(st.total_records == 60, "adopted data intact");

    // ========================================================================
    // Part 4 — V4 wide schema: >16 columns (extended names region)
    // ========================================================================
    printf("PART 4: V4 wide schema (30 columns)\n");
    const char *wide_names[30];
    static char wide_buf[30][12];
    wide_names[0] = "soc"; wide_names[1] = "tbat";
    wide_names[2] = "gen"; wide_names[3] = "site";
    for (int i = 4; i < 30; i++) {
        snprintf(wide_buf[i], sizeof(wide_buf[i]), "p%02d", i);
        wide_names[i] = wide_buf[i];
    }

    // Migrate the 4-column DB (60 records, ring) up to 30 columns.
    CHECK(tsdb_migrate_schema_h(db, wide_names, 30, NULL) == ESP_OK,
          "migrate 4 -> 30 columns");
    tsdb_get_stats_h(db, &st);
    CHECK(st.num_params == 30, "30 base params");
    CHECK(st.total_records == 60, "records survive widening");

    // Old data preserved, new columns zero. Oldest survivor = record 41
    // (part 1 wrote 61 records into the 60-ring: 40..99 + the post-migration
    // write evicted record 40).
    int16_t wrow[30];
    n = qcount(db, 0, 999999u, wrow, 30);
    CHECK(n == 60, "full query sees 60 records");
    CHECK(wrow[0] == 41, "soc survives into V4");
    CHECK(wrow[29] == 0, "column 30 zero-backfilled");

    // Extended-region name lookup (index >= 16).
    const char *nm = tsdb_get_param_name_h(db, 20);
    CHECK(nm != NULL && strcmp(nm, "p20") == 0, "ext name readable (col 20)");

    // Writes and reads work in the wide geometry (rpb = 1016/64 = 15).
    int16_t wv[30];
    for (int i = 0; i < 30; i++) wv[i] = (int16_t)(500 + i);
    CHECK(tsdb_write_h(db, T0 + 200 * DT, wv) == ESP_OK, "write 30-col record");
    n = qcount(db, T0 + 200 * DT, T0 + 200 * DT, wrow, 30);
    CHECK(n == 1 && wrow[0] == 500 && wrow[29] == 529, "read back 30-col record");
    tsdb_close_h(db);

    // Reopen the V4 file (config must carry 30 params) — V4 header + ext
    // names survive close/open.
    tsdb_config_t c4 = {0};
    c4.filepath = PATH; c4.num_params = 30; c4.max_records = 60; c4.index_stride = 10;
    c4.buffer_pool_size = 16 * 1024; c4.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    c4.param_names = wide_names;
    db = tsdb_open(&c4);
    CHECK(db != NULL, "V4 file reopens");
    if (db) {
        tsdb_get_stats_h(db, &st);
        CHECK(st.num_params == 30 && st.total_records == 60, "V4 state persists");
        nm = tsdb_get_param_name_h(db, 25);
        CHECK(nm != NULL && strcmp(nm, "p25") == 0, "ext names persist");
        n = qcount(db, T0 + 200 * DT, T0 + 200 * DT, wrow, 30);
        CHECK(n == 1 && wrow[29] == 529, "V4 data readable after reopen");
        tsdb_close_h(db);
    }

    // Fresh-create a wide DB directly (no migration).
    remove(PATH2); remove(PATH2 ".mig");
    tsdb_config_t c5 = c4;
    c5.filepath = PATH2; c5.num_params = 20; c5.max_records = 100;
    db = tsdb_open(&c5);
    CHECK(db != NULL, "fresh 20-column create");
    if (db) {
        int16_t v20[20];
        for (int i = 0; i < 20; i++) v20[i] = (int16_t)i;
        CHECK(tsdb_write_h(db, T0, v20) == ESP_OK, "write to fresh wide DB");
        nm = tsdb_get_param_name_h(db, 17);
        CHECK(nm != NULL && strcmp(nm, "p17") == 0, "fresh wide ext names");
        tsdb_close_h(db);
    }

    printf("\n%s (%d failed)\n", fails ? "*** FAIL ***" : "*** PASS ***", fails);
    return fails ? 1 : 0;
}
