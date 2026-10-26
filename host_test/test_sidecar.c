// Host regression for sidecar header persistence.
//
// The hot write path persists the header to <db>.h0/.h1 instead of offset 0 of
// the database, because an in-place rewrite at offset 0 costs ~3.7 s on
// littlefs. The in-file header is only refreshed on close/sync, so it can lag.
// This checks the three cases that matters for:
//
//   1. clean close  -> in-file header current, sidecar removed, data intact
//   2. simulated crash (no close) -> sidecar is ahead, reopen must adopt it
//   3. corrupt sidecar -> ignored, in-file header used, no crash
//
// 2.4.4 moved the per-record slots to <db>.d/h0,h1 (compact TSH2) and keeps
// <db>.h0/.h1 (TSH1) as a periodic mirror for older readers:
//
//   4. torn newest TSH2 slot -> the other TSH2 slot wins (one record behind)
//   5. only the TSH1 mirror visible (what 2.4.3 sees) -> mirror state adopted
//   6. sparse index rebuilt on close matches what per-write updates produced
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#define PATH "/tmp/tsdb_sidecar_test.tsdb"
#define NPARAM 4
static const char *names[NPARAM] = {"a", "b", "c", "d"};

static tsdb_t *open_db(void) {
    tsdb_config_t cfg = {0};
    cfg.filepath = PATH;
    cfg.num_params = NPARAM;
    cfg.param_names = names;
    cfg.max_records = 500;
    cfg.index_stride = 64;
    cfg.buffer_pool_size = 8 * 1024;
    return tsdb_open(&cfg);
}

static void cleanup(void) {
    unlink(PATH);
    unlink(PATH ".h0");
    unlink(PATH ".h1");
    unlink(PATH ".d/h0");
    unlink(PATH ".d/h1");
    rmdir(PATH ".d");
}

static void corrupt(const char *p) {
    FILE *f = fopen(p, "r+b");
    if (f != NULL) { fseek(f, 20, SEEK_SET); fputc(0xFF, f); fputc(0xFF, f); fclose(f); }
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int count_records(tsdb_t *db) {
    tsdb_stats_t st;
    if (tsdb_get_stats_h(db, &st) != ESP_OK) return -1;
    return (int) st.total_records;
}

int main(void) {
    int fails = 0;
    cleanup();

    // --- 1. clean close ---------------------------------------------------
    tsdb_t *db = open_db();
    if (db == NULL) { printf("open FAIL\n"); return 1; }
    int16_t v[NPARAM] = {1, 2, 3, 4};
    for (int i = 0; i < 50; i++) tsdb_write_h(db, 1000 + i, v);
    tsdb_close_h(db);

    db = open_db();
    int n = count_records(db);
    printf("1. clean close        : %d records (expect 50) %s\n", n, n == 50 ? "PASS" : "FAIL");
    if (n != 50) fails++;

    // --- 2. simulated crash: write more, never close ----------------------
    for (int i = 0; i < 30; i++) tsdb_write_h(db, 2000 + i, v);
    // Deliberately NOT tsdb_close_h(db) — that is the whole point. Leak the
    // handle, exactly as a power cut would.
    db = open_db();
    n = count_records(db);
    printf("2. crash, sidecar wins: %d records (expect 80) %s\n", n, n == 80 ? "PASS" : "FAIL");
    if (n != 80) fails++;
    tsdb_close_h(db);

    // --- 3. corrupt sidecar must be ignored, not fatal --------------------
    db = open_db();
    for (int i = 0; i < 10; i++) tsdb_write_h(db, 3000 + i, v);
    // Corrupt every slot, both locations; a clean close already removed any
    // older ones.
    corrupt(PATH ".h0");
    corrupt(PATH ".h1");
    corrupt(PATH ".d/h0");
    corrupt(PATH ".d/h1");
    db = open_db();
    n = count_records(db);
    // In-file header is from the last clean close (80). A corrupt sidecar must
    // not be adopted and must not crash the open.
    printf("3. corrupt sidecar    : %d records (expect 80, ignored) %s\n",
           n, n == 80 ? "PASS" : "FAIL");
    if (n != 80) fails++;
    // A clean close leaves a plain file: no slots, no subdirectory.
    tsdb_close_h(db);
    bool plain = !exists(PATH ".h0") && !exists(PATH ".h1") && !exists(PATH ".d/h0") &&
                 !exists(PATH ".d/h1") && rmdir(PATH ".d") != 0;
    printf("   clean close leaves a plain file %s\n", plain ? "PASS" : "FAIL");
    if (!plain) fails++;

    // --- 4. torn newest TSH2 slot: the older one survives ------------------
    db = open_db();
    for (int i = 0; i < 7; i++) tsdb_write_h(db, 4000 + i, v);   // 87
    // Slots alternate starting from h1 after a clean close: writes 1,3,5,7
    // land in h1, so h1 holds the newest (87) and h0 the one before (86).
    corrupt(PATH ".d/h1");
    db = open_db();
    n = count_records(db);
    printf("4. torn newest slot   : %d records (expect 86) %s\n", n, n == 86 ? "PASS" : "FAIL");
    if (n != 86) fails++;
    tsdb_close_h(db);

    // --- 5. a reader that only knows <db>.h0/.h1 ---------------------------
    // The mirror is written on the first write after open and then every
    // TSDB_SIDECAR_LEGACY_EVERY (32) records: after 40 writes it holds the
    // state after write 33. Removing the TSH2 slots shows what 2.4.3 sees.
    db = open_db();                                               // 86
    for (int i = 0; i < 40; i++) tsdb_write_h(db, 5000 + i, v);   // 126
    unlink(PATH ".d/h0");
    unlink(PATH ".d/h1");
    rmdir(PATH ".d");
    db = open_db();
    n = count_records(db);
    printf("5. TSH1 mirror only   : %d records (expect 119) %s\n", n, n == 119 ? "PASS" : "FAIL");
    if (n != 119) fails++;
    tsdb_close_h(db);

    // --- 6. index rebuilt on close equals per-write index ------------------
    // 500 records at stride 64 -> entries 0..7 = {ts of record k*64, block}:
    // exactly what 2.4.3 wrote per record.
    cleanup();
    db = open_db();
    for (int i = 0; i < 500; i++) tsdb_write_h(db, 10000 + i * 10, v);
    tsdb_close_h(db);
    {
        FILE *f = fopen(PATH, "rb");
        uint32_t idx_off = 0, rpb = 0;
        int bad = 0;
        tsdb_header_t h;
        if (fread(&h, sizeof(h), 1, f) != 1) bad++;
        rpb = h.records_per_block;
        idx_off = h.index_offset;
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t e[2];
            fseek(f, idx_off + k * 8, SEEK_SET);
            if (fread(e, 8, 1, f) != 1) bad++;
            if (e[0] != 10000 + k * 64 * 10 || e[1] != (k * 64) / rpb) bad++;
        }
        if (h.index_entries != 8) bad++;   // 500 / 64 + 1
        fclose(f);
        printf("6. index rebuilt      : %d bad entries (rpb %u, idx_off %u) %s\n", bad,
               (unsigned) rpb, (unsigned) idx_off, bad == 0 ? "PASS" : "FAIL");
        if (bad) fails++;
    }

    cleanup();
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails != 0;
}
