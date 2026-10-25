// Per-record littlefs cost of tsdb_write, measured on the real littlefs.
//
// Runs the unmodified esp_tsdb sources on littlefs (the exact version
// esp_littlefs ships, instrumented — see lfs/instrument_lfs.py) over a RAM
// block device configured like the firmware's partition, and counts
// block-device traffic per tsdb_write. On an ESP32 every flash read is a cache
// disable + IPC to the other core, so READ CALLS are the cost driver; the
// worst write is what trips the task watchdog.
//
// The filesystem is populated first with a realistic set of other files in
// the same root directory (configs, certs, a /logs tree), because root-dir
// metadata compaction copies every one of their entries — and inline files'
// contents — each time the root's metadata block fills.
//
//   ./test_lfs_cost [options]
//     --records N        records to write (default 20000)
//     --budget BYTES     ring storage budget, firmware formula (default 2560000)
//     --params N         columns (default 7, the energy logger)
//     --stride N         index_stride (default 380)
//     --interval S       seconds between records (default 300)
//     --cache N --read N --prog N --lookahead N --metadata-max N
//     --inline-max N     littlefs knobs (default: firmware sdkconfig)
//     --no-mtime         CONFIG_LITTLEFS_USE_MTIME=n
//     --churn K          rewrite one other root file every K records (default 0)
//     --trace N          print the per-operation breakdown of record N
//     --csv PATH         per-write reads to PATH
//     --check            assert regression thresholds (used by run_lfs_cost.sh)
//
// Prints a summary: reads per write (mean/p50/p99/p99.9/max), progs, erases,
// metadata commits/compactions (all dirs and root only), full-FS traversals,
// and the same broken down by stdio operation ("<op>:<data|side|other>").
// Then checks durability on littlefs: a simulated power cut (remount without
// close) must bring back every record, and a clean close must leave a plain
// file with no sidecars. Exit status is non-zero on any failure.
#include "esp_tsdb.h"
#include "lfs/lfs_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DB_PATH HFS_MOUNT "/energy.tsdb"

static const char *names[] = {"amps", "volts", "power", "energy", "temp", "soc", "pilot",
                              "p7", "p8", "p9", "p10", "p11", "p12", "p13", "p14", "p15"};

// Rough ESP32 time model, for orientation only: 30 us per block-device read
// call (cache disable + IPC to the other core) + 0.1 us/byte, 0.7 ms per
// program call, 60 ms per 4 KB erase. With it, a whole-file rewrite comes out
// at ~16 ms/KB against ~20 ms/KB measured on hardware.
static double est_ms(const hfs_counters_t *d) {
    return d->reads * 0.030 + d->read_bytes * 0.0001 + d->progs * 0.7 + d->erases * 60.0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
    return x < y ? -1 : x > y;
}

static void populate_root(void) {
    // What a running OpenEVSE WiFi unit keeps in the LittleFS root besides
    // the energy database. Sizes are representative; small ones are inlined
    // into the root metadata block, larger ones get their own blocks.
    static const struct { const char *path; size_t len; } files[] = {
        {"/config.json", 2600},           {"/schedule.json", 480},
        {"/emeter.json", 320},            {"/rfid_users.json", 260},
        {"/loadsharing_peers.json", 120}, {"/limit.json", 90},
        {"/boost.json", 80},              {"/claims.json", 200},
        {"/scheduler_plan.json", 700},    {"/ha_token.json", 400},
        {"/divert.json", 150},            {"/mqtt_ca.pem", 1800},
        {"/https_cert.pem", 1300},        {"/https_key.pem", 1700},
        {"/FFFFFFFFFFFFFFFF.json", 220},  {"/crash_report.json", 600},
    };
    char buf[4096];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        memset(buf, 'a' + (int) i, sizeof(buf));
        char p[128];
        snprintf(p, sizeof(p), HFS_MOUNT "%s", files[i].path);
        hfs_write_file(p, buf, files[i].len);
    }
    mkdir(HFS_MOUNT "/certificates", 0777);
    hfs_write_file(HFS_MOUNT "/certificates/FFFFFFFFFFFFFFFF.json", buf, 900);
    mkdir(HFS_MOUNT "/logs", 0777);
    mkdir(HFS_MOUNT "/logs/daily", 0777);
    mkdir(HFS_MOUNT "/logs/monthly", 0777);
    mkdir(HFS_MOUNT "/logs/raw", 0777);
    hfs_write_file(HFS_MOUNT "/logs/annual.json", buf, 300);
    hfs_write_file(HFS_MOUNT "/logs/index.json", buf, 64);
    for (int i = 0; i < 6; i++) {
        char p[128];
        snprintf(p, sizeof(p), HFS_MOUNT "/logs/%d.json", i);
        hfs_write_file(p, buf, 2000);
    }
}

static void print_counters(const char *label, const hfs_counters_t *c, uint64_t calls, uint64_t n) {
    double d = n ? (double) n : 1.0;
    printf("  %-14s calls/rec %5.2f  reads/rec %8.1f  KB/rec %7.1f  progs/rec %6.1f  erases/rec %5.2f"
           "  commits/rec %5.2f (root %5.2f)  compacts %llu (root %llu)  splits %llu  fs_size %llu  alloc_scan %llu\n",
           label, calls / d, c->reads / d, c->read_bytes / d / 1024.0, c->progs / d, c->erases / d,
           c->ev[LFS_H_COMMIT] / d, c->root_commits / d,
           (unsigned long long) c->ev[LFS_H_COMPACT], (unsigned long long) c->root_compacts,
           (unsigned long long) c->ev[LFS_H_SPLIT], (unsigned long long) c->ev[LFS_H_FS_SIZE],
           (unsigned long long) c->ev[LFS_H_ALLOC_SCAN]);
}

int main(int argc, char **argv) {
    uint32_t records = 20000, budget = 2560000, params = 7, stride = 380, interval = 300, churn = 0;
    uint32_t trace = UINT32_MAX;
    const char *csv = NULL;
    bool check = false;
    hfs_config_t fc = hfs_default_config();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "0";
#define OPT(name, dst) if (strcmp(a, name) == 0) { dst = strtoul(v, NULL, 0); i++; continue; }
        OPT("--records", records);
        OPT("--budget", budget);
        OPT("--params", params);
        OPT("--stride", stride);
        OPT("--interval", interval);
        OPT("--churn", churn);
        OPT("--trace", trace);
        OPT("--cache", fc.cache_size);
        OPT("--read", fc.read_size);
        OPT("--prog", fc.prog_size);
        OPT("--lookahead", fc.lookahead_size);
        OPT("--metadata-max", fc.metadata_max);
        OPT("--inline-max", fc.inline_max);
        OPT("--block-cycles", fc.block_cycles);
        OPT("--stdio-buf", fc.stdio_buf);
#undef OPT
        if (strcmp(a, "--no-mtime") == 0) { fc.mtime = false; continue; }
        if (strcmp(a, "--csv") == 0) { csv = v; i++; continue; }
        if (strcmp(a, "--check") == 0) { check = true; continue; }
        fprintf(stderr, "unknown option %s\n", a);
        return 2;
    }

    if (hfs_format_and_mount(&fc) != 0) { printf("mount FAIL\n"); return 1; }
    populate_root();

    tsdb_config_t cfg = {0};
    cfg.filepath = DB_PATH;
    cfg.num_params = params;
    cfg.param_names = names;
    cfg.max_records = TSDB_CALC_MAX_RECORDS(budget, params);
    cfg.index_stride = stride;
    cfg.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    cfg.buffer_pool_size = 12 * 1024;
    tsdb_t *db = tsdb_open(&cfg);
    if (db == NULL) { printf("tsdb_open FAIL\n"); return 1; }

    uint64_t *reads = calloc(records, sizeof(uint64_t));
    uint64_t *progs = calloc(records, sizeof(uint64_t));
    uint64_t max_reads = 0, max_at = 0, writes_with_root_compact = 0, max_erases = 0;
    double est_total = 0, est_max = 0;
    uint64_t est_max_at = 0;
    hfs_counters_t max_c = {0};
    hfs_reset_phases();
    hfs_counters_t start = hfs_ctr;
    FILE *cf = csv ? fopen(csv, "w") : NULL;
    if (cf) fprintf(cf, "record,reads,read_bytes,progs,erases,commits,root_commits,compacts,root_compacts,fs_size,alloc_scan\n");

    int16_t v[16] = {0};
    uint32_t ts = 1700000000u;
    uint32_t churn_i = 0;
    for (uint32_t r = 0; r < records; r++) {
        hfs_refresh_root_pairs();
        hfs_counters_t before = hfs_ctr;
        for (uint32_t p = 0; p < params; p++) v[p] = (int16_t) (r * 7 + p);
        hfs_phase_t saved[HFS_MAX_PHASES];
        int saved_n = hfs_nphases;
        if (r == trace) { memcpy(saved, hfs_phases, sizeof(saved)); hfs_reset_phases(); }
        esp_err_t e = tsdb_write_h(db, ts, v);
        if (r == trace) {
            printf("trace of record %u:\n", r);
            for (int i = 0; i < hfs_nphases; i++) print_counters(hfs_phases[i].name, &hfs_phases[i].c, hfs_phases[i].calls, 1);
            memcpy(hfs_phases, saved, sizeof(saved));
            hfs_nphases = saved_n;
        }
        ts += interval;
        if (e != ESP_OK) { printf("write %u FAIL (%d)\n", r, e); return 1; }
        hfs_counters_t d;
        uint64_t *pa = (uint64_t *) &d, *pb = (uint64_t *) &before, *pc = (uint64_t *) &hfs_ctr;
        for (size_t k = 0; k < sizeof(d) / sizeof(uint64_t); k++) pa[k] = pc[k] - pb[k];
        reads[r] = d.reads;
        progs[r] = d.progs;
        if (d.root_compacts) writes_with_root_compact++;
        if (d.erases > max_erases) max_erases = d.erases;
        double em = est_ms(&d);
        est_total += em;
        if (em > est_max) { est_max = em; est_max_at = r; }
        if (d.reads > max_reads) { max_reads = d.reads; max_at = r; max_c = d; }
        if (cf) fprintf(cf, "%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", r,
                        (unsigned long long) d.reads, (unsigned long long) d.read_bytes,
                        (unsigned long long) d.progs, (unsigned long long) d.erases,
                        (unsigned long long) d.ev[LFS_H_COMMIT], (unsigned long long) d.root_commits,
                        (unsigned long long) d.ev[LFS_H_COMPACT], (unsigned long long) d.root_compacts,
                        (unsigned long long) d.ev[LFS_H_FS_SIZE], (unsigned long long) d.ev[LFS_H_ALLOC_SCAN]);
        if (churn && (r % churn) == churn - 1) {
            // Some other subsystem rewrites a small root file (not counted).
            bool was = hfs_counting;
            hfs_counting = false;
            char buf[320];
            memset(buf, 'z', sizeof(buf));
            hfs_write_file(churn_i++ & 1 ? HFS_MOUNT "/emeter.json" : HFS_MOUNT "/schedule.json", buf, sizeof(buf));
            hfs_counting = was;
        }
    }
    if (cf) fclose(cf);

    hfs_counters_t tot;
    uint64_t *pa = (uint64_t *) &tot, *pb = (uint64_t *) &start, *pc = (uint64_t *) &hfs_ctr;
    for (size_t k = 0; k < sizeof(tot) / sizeof(uint64_t); k++) pa[k] = pc[k] - pb[k];

    tsdb_stats_t st;
    tsdb_get_stats_h(db, &st);
    struct stat sb;
    stat(DB_PATH, &sb);

    uint64_t *sorted = malloc(records * sizeof(uint64_t));
    memcpy(sorted, reads, records * sizeof(uint64_t));
    qsort(sorted, records, sizeof(uint64_t), cmp_u64);
    double mean = (double) tot.reads / records;
    printf("littlefs: read %u prog %u cache %u lookahead %u block %u x %u cycles %d metadata_max %u inline_max %u mtime %d\n",
           (unsigned) fc.read_size, (unsigned) fc.prog_size, (unsigned) fc.cache_size,
           (unsigned) fc.lookahead_size, (unsigned) fc.block_size, (unsigned) fc.block_count,
           (int) fc.block_cycles, (unsigned) fc.metadata_max, (unsigned) fc.inline_max, fc.mtime);
    printf("tsdb: %u params, max_records %u, stride %u; wrote %u -> total_records %lu, evictions %lu, file %ld bytes\n",
           params, cfg.max_records, stride, records, (unsigned long) st.total_records,
           (unsigned long) st.total_evictions, (long) sb.st_size);
    printf("READS/WRITE mean %.1f p50 %llu p99 %llu p99.9 %llu max %llu (record %llu)\n", mean,
           (unsigned long long) sorted[records / 2], (unsigned long long) sorted[(uint64_t) records * 99 / 100],
           (unsigned long long) sorted[(uint64_t) records * 999 / 1000], (unsigned long long) max_reads,
           (unsigned long long) max_at);
    printf("worst write: reads %llu (%llu KB) progs %llu erases %llu commits %llu root_commits %llu compacts %llu root_compacts %llu fs_size %llu alloc_scan %llu\n",
           (unsigned long long) max_c.reads, (unsigned long long) max_c.read_bytes / 1024,
           (unsigned long long) max_c.progs, (unsigned long long) max_c.erases,
           (unsigned long long) max_c.ev[LFS_H_COMMIT], (unsigned long long) max_c.root_commits,
           (unsigned long long) max_c.ev[LFS_H_COMPACT], (unsigned long long) max_c.root_compacts,
           (unsigned long long) max_c.ev[LFS_H_FS_SIZE], (unsigned long long) max_c.ev[LFS_H_ALLOC_SCAN]);
    printf("EST ESP32 ms/write (rough model): mean %.0f max %.0f (record %llu); max erases in one write %llu\n",
           est_total / records, est_max, (unsigned long long) est_max_at, (unsigned long long) max_erases);
    printf("writes with a root compaction: %llu of %u (1 per %.1f records)\n",
           (unsigned long long) writes_with_root_compact, records,
           writes_with_root_compact ? (double) records / writes_with_root_compact : 0.0);
    print_counters("TOTAL", &tot, records, records);
    for (int i = 0; i < hfs_nphases; i++) {
        if (hfs_phases[i].calls == 0 && hfs_phases[i].c.reads == 0) continue;
        print_counters(hfs_phases[i].name, &hfs_phases[i].c, hfs_phases[i].calls, records);
    }

    int fails = 0;
    // Power cut: remount without closing (the handle is abandoned, as a reset
    // would), reopen. Every record must come back from the sidecars.
    hfs_remount();
    db = tsdb_open(&cfg);
    tsdb_stats_t st2;
    if (db == NULL || tsdb_get_stats_h(db, &st2) != ESP_OK || st2.total_records != st.total_records) {
        printf("reopen after power cut: FAIL\n");
        fails++;
    } else {
        printf("reopen after power cut: %lu records PASS\n", (unsigned long) st2.total_records);
    }
    // Then a clean close and reboot: the file alone must carry everything.
    if (db) {
        tsdb_write_h(db, ts, v);
        tsdb_close_h(db);
    }
    hfs_remount();
    struct lfs_info li;
    bool plain = lfs_stat(hfs_lfs(), "/energy.tsdb.d", &li) != 0 &&
                 lfs_stat(hfs_lfs(), "/energy.tsdb.h0", &li) != 0 &&
                 lfs_stat(hfs_lfs(), "/energy.tsdb.h1", &li) != 0;
    db = tsdb_open(&cfg);
    if (db == NULL || tsdb_get_stats_h(db, &st2) != ESP_OK ||
        st2.total_records != (st.total_records < cfg.max_records ? st.total_records + 1 : st.total_records) ||
        !plain) {
        printf("reopen after close: FAIL (plain file %d)\n", plain);
        fails++;
    } else {
        printf("reopen after close: %lu records, no sidecars left PASS\n", (unsigned long) st2.total_records);
    }
    if (db) tsdb_close_h(db);
    if (check) {
        // Regression guards for the 2.4.4 changes (default geometry, before
        // the ring wraps): no whole-file rewrite on any write (2.4.3 erased
        // ~90 blocks every index_stride records at 20k records), and at most
        // one root-directory commit per record plus the periodic mirror.
        double root_per_rec = (double) tot.root_commits / records;
        bool ok_erase = max_erases <= 16;
        bool ok_root = root_per_rec <= 1.2;
        printf("check: max erases/write %llu (<= 16) %s; root commits/record %.2f (<= 1.2) %s\n",
               (unsigned long long) max_erases, ok_erase ? "PASS" : "FAIL", root_per_rec,
               ok_root ? "PASS" : "FAIL");
        if (!ok_erase || !ok_root) fails++;
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
