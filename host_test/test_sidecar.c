// Host regression for crash durability (was: sidecar header persistence).
//
// Since 3.0 there is no header to persist: every write fsyncs the active
// segment and the record count is derived from the file on open. The three
// scenarios from the 2.4 sidecar suite still hold, with stronger outcomes:
//
//   1. clean close -> data intact
//   2. simulated crash (no close) -> every fsynced record is back
//   3. garbage sidecar files next to the database -> ignored, nothing lost
#include "esp_tsdb.h"
#include "testutil.h"
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
    tsdb_test_wipe(PATH);
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

    // --- 3. garbage sidecar files must be ignored, not fatal ----------------
    db = open_db();
    for (int i = 0; i < 10; i++) tsdb_write_h(db, 3000 + i, v);
    for (int slot = 0; slot < 2; slot++) {
        char p[256];
        snprintf(p, sizeof(p), "%s.h%d", PATH, slot);
        FILE *f = fopen(p, "wb");
        if (f != NULL) { fputs("garbage sidecar", f); fclose(f); }
    }
    db = open_db();
    n = count_records(db);
    printf("3. garbage sidecars   : %d records (expect 90, ignored) %s\n",
           n, n == 90 ? "PASS" : "FAIL");
    if (n != 90) fails++;
    tsdb_close_h(db);

    cleanup();
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails != 0;
}
