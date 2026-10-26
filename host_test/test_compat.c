// Cross-version driver: built twice by build_compat.sh — against this tree and
// against an older release (default v2.4.3) — and run by run_compat.sh, which
// alternates the two on one database to check compatibility in both
// directions, including after an unclean shutdown (exit without close).
//
//   test_compat <db> write <n> <first_ts> close|crash
//   test_compat <db> count     -> "<records> <first_ts> <last_ts> <ordered 0|1>"
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPARAM 7
static const char *names[NPARAM] = {"amps", "volts", "power", "energy", "temp", "soc", "pilot"};

static tsdb_t *open_db(const char *path) {
    tsdb_config_t cfg = {0};
    cfg.filepath = path;
    cfg.num_params = NPARAM;
    cfg.param_names = names;
    cfg.max_records = 300;      // small ring so scenarios cross the wrap
    cfg.index_stride = 20;
    cfg.buffer_pool_size = 12 * 1024;
    return tsdb_open(&cfg);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    tsdb_t *db = open_db(argv[1]);
    if (db == NULL) { printf("open FAIL\n"); return 1; }
    if (strcmp(argv[2], "write") == 0 && argc >= 6) {
        int n = atoi(argv[3]);
        uint32_t ts = (uint32_t) strtoul(argv[4], NULL, 0);
        for (int i = 0; i < n; i++) {
            int16_t v[NPARAM];
            for (int p = 0; p < NPARAM; p++) v[p] = (int16_t) ((ts + i) * 3 + p);
            if (tsdb_write_h(db, ts + i, v) != ESP_OK) { printf("write FAIL\n"); return 1; }
        }
        if (strcmp(argv[5], "close") == 0) tsdb_close_h(db);
        return 0;   // "crash": exit with the handle open, no close
    }
    if (strcmp(argv[2], "count") == 0) {
        tsdb_query_t q;
        uint8_t cols[NPARAM] = {0, 1, 2, 3, 4, 5, 6};
        int n = 0, ordered = 1;
        uint32_t first = 0, last = 0;
        if (tsdb_query_init_h(db, &q, 0, 0xFFFFFFFFu, cols, NPARAM) == ESP_OK) {
            uint32_t ts;
            int16_t v[NPARAM];
            while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
                if (n == 0) first = ts;
                else if (ts <= last) ordered = 0;
                for (int p = 0; p < NPARAM; p++) if (v[p] != (int16_t) (ts * 3 + p)) ordered = 0;
                last = ts;
                n++;
            }
            tsdb_query_close(&q);
        }
        printf("%d %u %u %d\n", n, (unsigned) first, (unsigned) last, ordered);
        return 0;   // no close: counting must not change the on-disk state
    }
    return 2;
}
