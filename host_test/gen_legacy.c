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
// Prints "<retained> <first_ts> <last_ts>" — what an exact reader must see —
// and writes <dir>/records.bin: every record the release itself reads back
// (after a clean close: reopened; after a power cut: a copy of the files,
// opened as that release would at the next boot), as
// uint32 n, then n x {uint32 ts; int16 values[7]}.
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>

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
    bool crash = strcmp(argv[6], "crash") == 0;
    char ref[600], cmd[2048], out[600];
    if (crash) {
        // The bytes a power cut leaves (stdio buffers are what the files hold
        // at exit), copied; this release opens the copy as its next boot would.
        fflush(NULL);
        snprintf(ref, sizeof(ref), "%s/ref", argv[1]);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s' && cp -r '%s'/energy.tsdb* '%s'/", ref, ref, argv[1], ref);
        if (system(cmd) != 0) { fprintf(stderr, "copy FAIL\n"); return 1; }
        snprintf(path, sizeof(path), "%s/energy.tsdb", ref);
    } else {
        tsdb_close_h(db);
    }
    tsdb_t *rd = tsdb_open(&cfg);
    if (rd == NULL) { fprintf(stderr, "reopen FAIL\n"); return 1; }
    snprintf(out, sizeof(out), "%s/records.bin", argv[1]);
    FILE *f = fopen(out, "wb");
    uint32_t cnt = 0;
    fwrite(&cnt, 4, 1, f);
    tsdb_query_t q;
    if (tsdb_query_init_h(rd, &q, 0, 0xFFFFFFFFu, NULL, NPARAM) == ESP_OK) {
        uint32_t ts;
        int16_t v[NPARAM];
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
            fwrite(&ts, 4, 1, f);
            fwrite(v, sizeof(v), 1, f);
            cnt++;
        }
        tsdb_query_close(&q);
    }
    fseek(f, 0, SEEK_SET);
    fwrite(&cnt, 4, 1, f);
    fclose(f);
    tsdb_close_h(rd);
    if (crash) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", ref);
        if (system(cmd) != 0) return 1;
    }
    return 0;   // crash: exit with the first handle open
}
