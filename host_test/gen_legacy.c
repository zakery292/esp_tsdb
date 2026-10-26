// Builds a pre-3.0 (2.x) database on the host filesystem, for the 3.0 upgrade
// scenarios in test_lfs_upgrade.c. Compiled against an older release's
// sources by run_lfs_upgrade.sh (git archive v2.4.3 / v2.4.4), so the file,
// its sidecars and (2.4.4) the <db>.d/ slot directory are exactly what a
// device running that release leaves behind.
//
//   gen_legacy <dir> <max_records> <n> <t0> <interval> close|crash
//
// Writes n records (timestamp t0 + i*interval, values (ts*3 + p) & 0x7FFF)
// to <dir>/energy.tsdb with the firmware's energy-logger schema, then either
// closes cleanly or exits with the handle open (a power cut). fsync is a
// no-op here: durability on the host does not matter, only the bytes.
// Prints "<retained> <first_ts> <last_ts>" — what an exact reader must see.
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __wrap_fsync(int fd) { (void) fd; return 0; }

#define NPARAM 7
static const char *names[NPARAM] = {"amps", "volts", "power", "energy", "temp", "soc", "pilot"};

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: gen_legacy <dir> <max_records> <n> <t0> <interval> close|crash\n");
        return 2;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/energy.tsdb", argv[1]);
    uint32_t max = (uint32_t) strtoul(argv[2], NULL, 0);
    uint32_t n = (uint32_t) strtoul(argv[3], NULL, 0);
    uint32_t t0 = (uint32_t) strtoul(argv[4], NULL, 0);
    uint32_t iv = (uint32_t) strtoul(argv[5], NULL, 0);

    tsdb_config_t cfg = {0};
    cfg.filepath = path;
    cfg.num_params = NPARAM;
    cfg.param_names = names;
    cfg.max_records = max;
    cfg.index_stride = 380;
    cfg.buffer_pool_size = 12 * 1024;
    tsdb_t *db = tsdb_open(&cfg);
    if (db == NULL) { fprintf(stderr, "open FAIL\n"); return 1; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ts = t0 + i * iv;
        int16_t v[NPARAM];
        for (int p = 0; p < NPARAM; p++) v[p] = (int16_t) ((ts * 3u + p) & 0x7FFF);
        if (tsdb_write_h(db, ts, v) != ESP_OK) { fprintf(stderr, "write %u FAIL\n", i); return 1; }
    }
    uint32_t kept = n < max ? n : max;
    printf("%u %u %u\n", kept, t0 + (n - kept) * iv, t0 + (n - 1) * iv);
    fflush(stdout);
    if (strcmp(argv[6], "close") == 0) tsdb_close_h(db);
    return 0;   // crash: exit with the handle open
}
