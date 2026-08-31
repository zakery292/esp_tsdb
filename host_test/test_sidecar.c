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
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    // Corrupt both slots; a clean close already removed any older pair.
    for (int slot = 0; slot < 2; slot++) {
        char p[256];
        snprintf(p, sizeof(p), "%s.h%d", PATH, slot);
        FILE *f = fopen(p, "r+b");
        if (f != NULL) { fseek(f, 8, SEEK_SET); fputc(0xFF, f); fputc(0xFF, f); fclose(f); }
    }
    db = open_db();
    n = count_records(db);
    // In-file header is from the last clean close (80). A corrupt sidecar must
    // not be adopted and must not crash the open.
    printf("3. corrupt sidecar    : %d records (expect 80, ignored) %s\n",
           n, n == 80 ? "PASS" : "FAIL");
    if (n != 80) fails++;
    tsdb_close_h(db);

    cleanup();
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails != 0;
}
