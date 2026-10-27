// Upgrade and long-run sizing on the real littlefs (see test_lfs_cost.c for
// the harness). Three modes:
//
//   test_lfs_upgrade upgrade <legacy_dir> [opts]
//       Copies a 2.x database built by gen_legacy (file, sidecars, 2.4.4
//       <db>.d/ slots) onto a littlefs partition laid out like the firmware's
//       (other files included), opens it with 3.0 using the firmware's
//       capacity formula, and keeps writing. Checks: the adopted history is
//       exact; every write succeeds; littlefs never returns ENOSPC and the
//       other files stay writable; the legacy file counts toward the capacity
//       and the database's on-disk total stays within max_records plus one
//       segment; history is dropped oldest-first by the capacity policy; a
//       power cut at the end loses nothing.
//   test_lfs_upgrade fresh [opts]
//       A new 3.0 database on the same partition, written for --multiple x
//       capacity records. Checks: every write succeeds, no write costs more
//       erases than one segment file, disk usage plateaus.
//   test_lfs_upgrade faults
//       Write / fsync / device failures surface as errors from tsdb_write and
//       tsdb_open (3.0 path and the legacy V2->V3 migration path), and the
//       database stays consistent across them.
//
// Options: --fs-blocks N (864 = the 16 MB build's 3.375 MB partition; 32 = the
// 4 MB build's 128 KB), --other-kb N (other files, default 400 / 24),
// --writes N (upgrade: default 1.3 x capacity), --multiple X (fresh: 2.2),
// --interval S (300), --min-free-kb N (required free-space floor),
// --guard-kb N (configure the free-space guard with this reserve; off by
// default, as in the firmware).
#include "esp_tsdb.h"
#include "lfs/lfs_stdio.h"

#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_PATH HFS_MOUNT "/energy.tsdb"
#define NPARAM 7
static const char *names[NPARAM] = {"amps", "volts", "power", "energy", "temp", "soc", "pilot"};
static const uint32_t RPB = (TSDB_BLOCK_SIZE - 8) / (4 + 2 * NPARAM);

static int fails;
#define CHECK(cond, ...) do { \
        bool ok_ = (cond); \
        printf("  [%s] ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
        if (!ok_) fails++; \
    } while (0)

// Same rough ESP32 model as test_lfs_cost.c.
static double est_ms(const hfs_counters_t *d) {
    return d->reads * 0.030 + d->read_bytes * 0.0001 + d->progs * 0.7 + d->erases * 60.0;
}

static hfs_counters_t delta(const hfs_counters_t *a, const hfs_counters_t *b) {
    hfs_counters_t d;
    const uint64_t *pa = (const uint64_t *) a, *pb = (const uint64_t *) b;
    uint64_t *pd = (uint64_t *) &d;
    for (size_t k = 0; k < sizeof(d) / sizeof(uint64_t); k++) pd[k] = pb[k] - pa[k];
    return d;
}

static void fill(int16_t *v, uint32_t ts) {
    for (int p = 0; p < NPARAM; p++) v[p] = (int16_t) ((ts * 3u + p) & 0x7FFF);
}

// ---------------------------------------------------------------------------
// Filesystem setup
// ---------------------------------------------------------------------------

static bool write_lfs_file(const char *path, size_t len, char c) {
    static char buf[4096];
    memset(buf, c, sizeof(buf));
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    bool ok = true;
    for (size_t done = 0; done < len && ok; ) {
        size_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
        ok = fwrite(buf, n, 1, f) == 1;
        done += n;
    }
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

// What a running unit keeps besides the energy database (test_lfs_cost.c's
// root set), topped up with /logs/raw files to `other_kb` in total.
static void populate_other_files(uint32_t other_kb) {
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
    size_t total = 0;
    char p[160];
    size_t nfiles = sizeof(files) / sizeof(files[0]);
    if (other_kb < 64) nfiles = 8;   // 4 MB build: config + small state, no certificates
    for (size_t i = 0; i < nfiles; i++) {
        snprintf(p, sizeof(p), HFS_MOUNT "%s", files[i].path);
        write_lfs_file(p, files[i].len, 'a' + (char) i);
        total += files[i].len;
    }
    mkdir(HFS_MOUNT "/logs", 0777);
    mkdir(HFS_MOUNT "/logs/monthly", 0777);
    write_lfs_file(HFS_MOUNT "/logs/monthly/2026.json", 1200, 'm');
    total += 1200;
    if (total + 1024 <= (size_t) other_kb * 1024) mkdir(HFS_MOUNT "/logs/raw", 0777);
    for (int i = 0; total + 1024 <= (size_t) other_kb * 1024; i++) {
        size_t len = (size_t) other_kb * 1024 - total;
        if (len > 20 * 1024) len = 20 * 1024;
        snprintf(p, sizeof(p), HFS_MOUNT "/logs/raw/%03d.json", i);
        write_lfs_file(p, len, 'r');
        total += len;
    }
}

// Copy a gen_legacy output directory (host) into the littlefs root.
static int import_legacy(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) return -1;
    int n = 0;
    struct dirent *e;
    char src[600], dst[300];
    static char buf[1 << 16];
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "energy.tsdb", 11) != 0) continue;
        snprintf(src, sizeof(src), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(src, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            snprintf(dst, sizeof(dst), HFS_MOUNT "/%s", e->d_name);
            mkdir(dst, 0777);
            DIR *sd = opendir(src);
            struct dirent *se;
            while (sd && (se = readdir(sd)) != NULL) {
                if (se->d_name[0] == '.') continue;
                char s2[900], d2[600];
                snprintf(s2, sizeof(s2), "%s/%s", src, se->d_name);
                snprintf(d2, sizeof(d2), "%s/%s", dst, se->d_name);
                FILE *in = fopen(s2, "rb"), *out = fopen(d2, "wb");
                size_t r;
                while (in && out && (r = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, r, out);
                if (in) fclose(in);
                if (out) fclose(out);
                n++;
            }
            if (sd) closedir(sd);
            continue;
        }
        snprintf(dst, sizeof(dst), HFS_MOUNT "/%s", e->d_name);
        FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
        if (in == NULL || out == NULL) { if (in) fclose(in); if (out) fclose(out); closedir(d); return -1; }
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, r, out) != r) { fclose(in); fclose(out); closedir(d); return -1; }
        }
        fclose(in);
        if (fclose(out) != 0) { closedir(d); return -1; }
        n++;
    }
    closedir(d);
    return n;
}

static bool lfs_exists(const char *lfs_rel) {
    struct lfs_info li;
    bool was = hfs_counting;
    hfs_counting = false;
    bool r = lfs_stat(hfs_lfs(), lfs_rel, &li) == 0;
    hfs_counting = was;
    return r;
}

// Largest segment file and number of segment files.
static void seg_files(int64_t *max_size, int *count) {
    *max_size = 0;
    *count = 0;
    bool was = hfs_counting;
    hfs_counting = false;
    lfs_dir_t d;
    if (lfs_dir_open(hfs_lfs(), &d, "/") == 0) {
        struct lfs_info info;
        while (lfs_dir_read(hfs_lfs(), &d, &info) > 0) {
            if (strncmp(info.name, "energy.tsdb.s", 13) == 0) {
                (*count)++;
                if ((int64_t) info.size > *max_size) *max_size = info.size;
            }
        }
        lfs_dir_close(hfs_lfs(), &d);
    }
    hfs_counting = was;
}

// ---------------------------------------------------------------------------
// tsdb helpers
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t fs_blocks, other_kb, interval, min_free_kb, guard_kb;
    uint32_t writes;
    double multiple;
} opts_t;

static uint32_t fw_max_records(uint32_t fs_total) {
    // src/tsdb_energy_logger.cpp init_db()
    const uint32_t reserve = 384u * 1024u;
    uint32_t fs_budget = fs_total > reserve ? fs_total - reserve : fs_total / 4;
    uint32_t budget = 2500u * 1024u;
    if (budget > fs_budget) budget = fs_budget;
    return TSDB_CALC_MAX_RECORDS(budget, NPARAM);
}

static uint64_t guard_free_space(void) {
    int64_t f = hfs_free_bytes();
    return f < 0 ? UINT64_MAX : (uint64_t) f;
}
static uint32_t g_guard_kb;

static tsdb_config_t fw_config(uint32_t max_records) {
    tsdb_config_t cfg = {0};
    cfg.filepath = DB_PATH;
    cfg.num_params = NPARAM;
    cfg.param_names = names;
    cfg.max_records = max_records;
    cfg.index_stride = 380;
    cfg.alloc_strategy = TSDB_ALLOC_PSRAM;
    cfg.buffer_pool_size = 16 * 1024;
    cfg.use_paged_allocation = true;
    cfg.page_size = 4096;
    if (g_guard_kb) {
        // The free-space guard, as a firmware would wire it to LittleFS.
        cfg.free_space_cb = guard_free_space;
        cfg.min_free_bytes = g_guard_kb * 1024;
    }
    return cfg;
}

typedef struct { uint32_t n, first, last; bool ordered, values; } scan_t;

static scan_t scan(tsdb_t *db) {
    scan_t s = {0, 0, 0, true, true};
    tsdb_query_t q;
    uint8_t cols[NPARAM] = {0, 1, 2, 3, 4, 5, 6};
    bool was = hfs_counting;
    hfs_counting = false;
    if (tsdb_query_init_h(db, &q, 0, 0xFFFFFFFFu, cols, NPARAM) == ESP_OK) {
        uint32_t ts;
        int16_t v[NPARAM], want[NPARAM];
        while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
            if (s.n == 0) s.first = ts;
            else if (ts <= s.last) s.ordered = false;
            fill(want, ts);
            if (memcmp(v, want, sizeof(v)) != 0) s.values = false;
            s.last = ts;
            s.n++;
        }
        tsdb_query_close(&q);
    } else {
        s.ordered = s.values = false;
    }
    hfs_counting = was;
    return s;
}

static uint64_t disk_usage(void) {
    uint64_t b = 0;
    bool was = hfs_counting;
    hfs_counting = false;
    tsdb_disk_usage(DB_PATH, &b);
    hfs_counting = was;
    return b;
}

static uint64_t data_bytes(uint32_t records) {
    return (uint64_t) ((records + RPB - 1) / RPB) * TSDB_BLOCK_SIZE;
}

static int mount_fs(const opts_t *o) {
    hfs_config_t fc = hfs_default_config();
    fc.block_count = o->fs_blocks;
    if (hfs_format_and_mount(&fc) != 0) { printf("mount FAIL\n"); return -1; }
    populate_other_files(o->other_kb);
    return 0;
}

// ---------------------------------------------------------------------------
// Write loop shared by upgrade and fresh
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t writes_ok, first_fail_at;
    esp_err_t first_fail_err;
    uint64_t max_erases;
    double max_ms, sum_ms;
    uint32_t max_ms_at;
    int64_t min_free;
    uint32_t min_free_at;
    uint32_t config_writes, config_fails;
    int64_t max_slack;          // disk usage - data_bytes(max_records)
    uint32_t retained_over_cap;
    uint64_t usage_at[8];       // disk usage at k * capacity / 2 written records
    uint64_t max_usage_late;    // max disk usage after 1.2 x capacity
    // Legacy tracking (upgrade)
    bool legacy_present;
    uint32_t legacy_gone_at;    // write index at which the legacy file disappeared
    uint32_t retained_before_gone, retained_after_gone;
    uint32_t legacy_partial_at, retained_before_partial, retained_after_partial;
    bool leftovers_after_gone;  // .h0/.h1/.d still present after the file went
    uint32_t history_mismatch;  // retained != expected while the legacy file was present
    bool saw_nospc;
    int last_nseg;
    int64_t max_over;           // slack beyond file prefixes
    uint32_t max_over_at;
    int64_t max_seg_file;       // largest segment file seen (a full segment)
} run_t;

static void write_loop(tsdb_t *db, const tsdb_config_t *cfg, uint32_t writes, uint32_t *ts,
                       uint32_t interval, uint32_t legacy_count, run_t *r) {
    int16_t v[NPARAM];
    int64_t legacy_size = -1;
    struct stat st;
    if (stat(DB_PATH, &st) == 0) legacy_size = st.st_size;
    r->legacy_present = legacy_size >= 0;
    r->min_free = hfs_free_bytes();
    tsdb_stats_t s0;
    tsdb_get_stats_h(db, &s0);
    uint32_t retained_prev = s0.total_records;
    for (uint32_t w = 0; w < writes; w++) {
        hfs_refresh_root_pairs();
        fill(v, *ts);
        hfs_counters_t before = hfs_ctr;
        esp_err_t e = tsdb_write_h(db, *ts, v);
        hfs_counters_t d = delta(&before, &hfs_ctr);
        *ts += interval;
        if (hfs_nospc && !r->saw_nospc) {
            // Report it now: littlefs 2.11.3 can assert on a later operation
            // after an ENOSPC (pcache left dirty), which aborts the run.
            r->saw_nospc = true;
            struct stat ls;
            printf("  first ENOSPC from littlefs at new write %u (tsdb_write returned %d); legacy file %s, "
                   "retained before %u\n", w, e, stat(DB_PATH, &ls) == 0 ? "present" : "gone", retained_prev);
        }
        if (e != ESP_OK) {
            if (r->first_fail_err == ESP_OK) { r->first_fail_err = e; r->first_fail_at = w; }
            continue;
        }
        r->writes_ok++;
        double ms = est_ms(&d);
        r->sum_ms += ms;
        if (ms > r->max_ms) { r->max_ms = ms; r->max_ms_at = w; }
        if (d.erases > r->max_erases) r->max_erases = d.erases;

        tsdb_stats_t s;
        tsdb_get_stats_h(db, &s);
        if (s.total_records > cfg->max_records) r->retained_over_cap++;

        // Free space moves by whole 4 KB blocks as files grow (every ~220
        // records here) or go: sampling every 8 writes and around every
        // change of the file set finds the minimum to within one block.
        int nseg_now = (int) s.segment_count;
        bool files_changed = nseg_now != r->last_nseg || s.total_records != retained_prev + 1;
        r->last_nseg = nseg_now;
        if ((w & 7) == 0 || files_changed || w + 1 == writes) {
            int64_t fr = hfs_free_bytes();
            if (fr < r->min_free) { r->min_free = fr; r->min_free_at = w; }
            uint64_t use = disk_usage();
            int64_t maxseg; int nseg;
            seg_files(&maxseg, &nseg);
            int64_t legacy_now = (stat(DB_PATH, &st) == 0) ? st.st_size : 0;
            // Slack over max_records worth of data blocks, less what every
            // file carries beyond its records: its prefix (1 KB segment
            // header; the legacy file's header + index, < 4 KB) and a
            // partly filled last block. Compared at the end against one
            // full segment file.
            int64_t legacy_extra = legacy_now > 0 ? 4096 + TSDB_BLOCK_SIZE : 0;
            int64_t slack = (int64_t) use - (int64_t) data_bytes(cfg->max_records);
            int64_t over = slack - (int64_t) nseg * 2 * TSDB_BLOCK_SIZE - legacy_extra;
            if (slack > r->max_slack) r->max_slack = slack;
            if (over > r->max_over) { r->max_over = over; r->max_over_at = w; }
            if (maxseg > r->max_seg_file) r->max_seg_file = maxseg;
            uint64_t written = (uint64_t) legacy_count + w + 1;
            for (int k = 1; k < 8; k++) {
                if (written >= (uint64_t) cfg->max_records * k / 2 && r->usage_at[k] == 0) r->usage_at[k] = use;
            }
            if ((double) (w + 1) > 1.2 * cfg->max_records && use > r->max_usage_late) r->max_usage_late = use;
        }

        if (r->legacy_present) {
            bool still = stat(DB_PATH, &st) == 0;
            uint64_t expect = (uint64_t) legacy_count + w + 1;
            if (expect > cfg->max_records) expect = cfg->max_records;
            if (!still) {
                printf("  legacy file removed at new write %u: retained %u -> %u, free %lld KB\n", w,
                       retained_prev, s.total_records, (long long) hfs_free_bytes() / 1024);
                r->legacy_present = false;
                r->legacy_gone_at = w;
                r->retained_before_gone = retained_prev;
                r->retained_after_gone = s.total_records;
                r->leftovers_after_gone = lfs_exists("/energy.tsdb.d") || lfs_exists("/energy.tsdb.h0") ||
                                          lfs_exists("/energy.tsdb.h1");
            } else {
                if (st.st_size < legacy_size && r->legacy_partial_at == 0) {
                    r->legacy_partial_at = w;
                    r->retained_before_partial = retained_prev;
                    r->retained_after_partial = s.total_records;
                }
                legacy_size = st.st_size;
                if (r->legacy_partial_at == 0 && s.total_records != expect) r->history_mismatch++;
            }
        }
        retained_prev = s.total_records;

        if ((w % 2000) == 1999) {
            // Some other subsystem saves its config (not counted).
            bool was = hfs_counting;
            hfs_counting = false;
            r->config_writes++;
            if (!write_lfs_file(HFS_MOUNT "/config.json", 2600, 'c')) r->config_fails++;
            hfs_counting = was;
        }
    }
}

static void days(uint32_t records, char *out, size_t n) {
    snprintf(out, n, "%.1f days at 1/min, %.1f days at 1/5 min", records / 1440.0, records / 288.0);
}

// ---------------------------------------------------------------------------
// upgrade
// ---------------------------------------------------------------------------

static int run_upgrade(const char *legacy_dir, const opts_t *o) {
    char expf[600];
    snprintf(expf, sizeof(expf), "%s/expected", legacy_dir);
    FILE *ef = fopen(expf, "r");
    uint32_t exp_n = 0, exp_first = 0, exp_last = 0;
    if (ef == NULL || fscanf(ef, "%u %u %u", &exp_n, &exp_first, &exp_last) != 3) {
        printf("cannot read %s\n", expf);
        return 1;
    }
    fclose(ef);

    if (mount_fs(o) != 0) return 1;
    uint32_t fs_total = o->fs_blocks * 4096;
    tsdb_config_t cfg = fw_config(fw_max_records(fs_total));
    bool was = hfs_counting;
    hfs_counting = false;
    int imported = import_legacy(legacy_dir);
    hfs_counting = was;
    struct stat st;
    stat(DB_PATH, &st);
    printf("upgrade %s: fs %u KB, other files %u KB, max_records %u, legacy file %ld bytes (%d files), free %lld KB\n",
           legacy_dir, fs_total / 1024, o->other_kb, cfg.max_records, (long) st.st_size, imported,
           (long long) hfs_free_bytes() / 1024);

    tsdb_t *db = tsdb_open(&cfg);
    if (db == NULL) { printf("  [FAIL] tsdb_open\n"); return 1; }
    scan_t s0 = scan(db);
    CHECK(s0.n == exp_n && s0.first == exp_first && s0.last == exp_last && s0.ordered && s0.values,
          "adopted history: %u records %u..%u (want %u %u..%u), ordered %d, values %d",
          s0.n, s0.first, s0.last, exp_n, exp_first, exp_last, s0.ordered, s0.values);

    uint32_t writes = o->writes ? o->writes : (uint32_t) (1.3 * cfg.max_records);
    uint32_t ts = exp_last + o->interval;
    run_t r = {0};
    uint64_t nospc0 = hfs_nospc;
    write_loop(db, &cfg, writes, &ts, o->interval, s0.n, &r);
    uint64_t nospc = hfs_nospc - nospc0;

    char dd[96];
    CHECK(r.writes_ok == writes, "every write succeeds: %u of %u (first failure at %u, err %d)",
          r.writes_ok, writes, r.first_fail_at, r.first_fail_err);
    CHECK(nospc == 0, "littlefs never returned ENOSPC (%llu times)", (unsigned long long) nospc);
    CHECK(r.min_free >= (int64_t) o->min_free_kb * 1024,
          "min free space %lld KB at write %u (floor %u KB)", (long long) r.min_free / 1024, r.min_free_at,
          o->min_free_kb);
    CHECK(r.config_fails == 0, "other files stay writable: %u of %u config saves failed",
          r.config_fails, r.config_writes);
    CHECK(r.retained_over_cap == 0, "retained records never exceed max_records");
    CHECK(r.max_over <= r.max_seg_file,
          "on-disk total within max_records + one segment: max %lld KB over max_records, %lld KB of it "
          "beyond file headers (write %u); one segment file = %lld KB",
          (long long) r.max_slack / 1024, (long long) r.max_over / 1024, r.max_over_at,
          (long long) r.max_seg_file / 1024);
    CHECK(r.history_mismatch == 0,
          "while the legacy file is whole, every adopted record stays readable until the window passes it "
          "(%u mismatches)", r.history_mismatch);
    if (r.legacy_partial_at) {
        days(r.legacy_partial_at, dd, sizeof(dd));
        printf("  legacy file truncated (oldest part dropped) at new write %u (%s): retained %u -> %u\n",
               r.legacy_partial_at, dd, r.retained_before_partial, r.retained_after_partial);
    }
    if (!r.legacy_present) {
        days(r.legacy_gone_at + 1, dd, sizeof(dd));
        printf("  legacy history dropped after %u new records (%s): retained %u -> %u\n",
               r.legacy_gone_at + 1, dd, r.retained_before_gone, r.retained_after_gone);
        CHECK(!r.leftovers_after_gone, "legacy sidecars (.h0/.h1/.d) removed with the file");
    } else {
        printf("  legacy file still present after %u writes\n", writes);
    }
    printf("  worst write: %llu erases, est %.0f ms (write %u); mean est %.1f ms\n",
           (unsigned long long) r.max_erases, r.max_ms, r.max_ms_at, r.sum_ms / (r.writes_ok ? r.writes_ok : 1));

    // Power cut, then a full scan.
    tsdb_stats_t sb;
    tsdb_get_stats_h(db, &sb);
    scan_t s1 = scan(db);
    hfs_remount();
    db = tsdb_open(&cfg);
    tsdb_stats_t sa = {0};
    scan_t s2 = {0};
    if (db) { tsdb_get_stats_h(db, &sa); s2 = scan(db); }
    CHECK(db && sa.total_records == sb.total_records && s2.n == s1.n && s2.first == s1.first &&
          s2.last == s1.last && s2.ordered && s2.values,
          "power cut at the end: %u records %u..%u before, %u %u..%u after, ordered %d values %d",
          s1.n, s1.first, s1.last, s2.n, s2.first, s2.last, s2.ordered, s2.values);
    if (db) tsdb_close_h(db);
    return 0;
}

// ---------------------------------------------------------------------------
// fresh
// ---------------------------------------------------------------------------

static int run_fresh(const opts_t *o) {
    if (mount_fs(o) != 0) return 1;
    uint32_t fs_total = o->fs_blocks * 4096;
    tsdb_config_t cfg = fw_config(fw_max_records(fs_total));
    printf("fresh: fs %u KB, other files %u KB, max_records %u, free %lld KB\n", fs_total / 1024, o->other_kb,
           cfg.max_records, (long long) hfs_free_bytes() / 1024);
    tsdb_t *db = tsdb_open(&cfg);
    if (db == NULL) { printf("  [FAIL] tsdb_open\n"); return 1; }
    uint32_t writes = (uint32_t) (o->multiple * cfg.max_records);
    uint32_t ts = 1700000000u;
    run_t r = {0};
    uint64_t nospc0 = hfs_nospc;
    write_loop(db, &cfg, writes, &ts, o->interval, 0, &r);
    uint64_t nospc = hfs_nospc - nospc0;
    int64_t maxseg; int nseg;
    seg_files(&maxseg, &nseg);
    uint64_t seg_erase_blocks = (uint64_t) (maxseg + 4095) / 4096;
    // A write that starts a segment also commits the directory (create +
    // unlink) and may compact it: a few blocks that do not scale with the
    // database. On the 4 MB build a segment is only 2 blocks, so the bound
    // there is that fixed metadata work instead.
    uint64_t erase_bound = seg_erase_blocks > 8 ? seg_erase_blocks : 8;

    CHECK(r.writes_ok == writes, "every write succeeds: %u of %u (first failure at %u, err %d)",
          r.writes_ok, writes, r.first_fail_at, r.first_fail_err);
    CHECK(nospc == 0, "littlefs never returned ENOSPC (%llu times)", (unsigned long long) nospc);
    CHECK(r.min_free >= (int64_t) o->min_free_kb * 1024, "min free space %lld KB (floor %u KB)",
          (long long) r.min_free / 1024, o->min_free_kb);
    CHECK(r.config_fails == 0, "other files stay writable: %u of %u config saves failed",
          r.config_fails, r.config_writes);
    CHECK(r.max_erases <= erase_bound,
          "worst write: %llu erases (bound %llu; one segment file = %llu blocks of 4 KB), est %.0f ms at write %u; "
          "mean %.1f ms",
          (unsigned long long) r.max_erases, (unsigned long long) erase_bound,
          (unsigned long long) seg_erase_blocks, r.max_ms, r.max_ms_at,
          r.sum_ms / (r.writes_ok ? r.writes_ok : 1));
    CHECK(r.max_over <= r.max_seg_file,
          "on-disk total within max_records + one segment: max %lld KB over max_records, %lld KB of it "
          "beyond file headers (write %u); one segment file = %lld KB",
          (long long) r.max_slack / 1024, (long long) r.max_over / 1024, r.max_over_at,
          (long long) r.max_seg_file / 1024);
    printf("  disk usage at 0.5x/1x/1.5x/2x capacity: %llu / %llu / %llu / %llu KB; max after 1.2x: %llu KB; "
           "%d segment files of up to %lld bytes\n",
           (unsigned long long) r.usage_at[1] / 1024, (unsigned long long) r.usage_at[2] / 1024,
           (unsigned long long) r.usage_at[3] / 1024, (unsigned long long) r.usage_at[4] / 1024,
           (unsigned long long) r.max_usage_late / 1024, nseg, (long long) maxseg);
    CHECK(r.max_usage_late <= data_bytes(cfg.max_records) + (uint64_t) maxseg + (uint64_t) nseg * 1024,
          "disk usage plateaus at max_records + one segment");
    tsdb_close_h(db);
    return 0;
}

// ---------------------------------------------------------------------------
// faults
// ---------------------------------------------------------------------------

static tsdb_config_t small_config(uint32_t max_records) {
    tsdb_config_t cfg = fw_config(max_records);
    cfg.buffer_pool_size = 12 * 1024;
    cfg.use_paged_allocation = false;
    cfg.page_size = 0;
    cfg.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    return cfg;
}

// Writes until n successes; returns the next timestamp.
static uint32_t write_n(tsdb_t *db, uint32_t ts, uint32_t n) {
    int16_t v[NPARAM];
    for (uint32_t i = 0; i < n; i++, ts += 60) {
        fill(v, ts);
        if (tsdb_write_h(db, ts, v) != ESP_OK) printf("    (unexpected write failure)\n");
    }
    return ts;
}

static void fault_case(const char *label, int mode) {
    // mode 0: fsync fails; 1: device program failure; 2: ENOSPC with nothing
    // to evict; 3: fsync fails in a batch, when it moves to the next block
    opts_t o = {.fs_blocks = 64, .other_kb = 24};
    mount_fs(&o);
    tsdb_config_t cfg = small_config(20000);
    tsdb_t *db = tsdb_open(&cfg);
    uint32_t ts = write_n(db, 1700000000u, 100);
    int16_t v[NPARAM];
    fill(v, ts);
    esp_err_t e;
    if (mode == 0) {
        hfs_fail_fsyncs = 1;
        e = tsdb_write_h(db, ts, v);
        hfs_fail_fsyncs = 0;
    } else if (mode == 3) {
        // 100 records fill 1 block + 44 of the next; the batch tops that
        // block up in RAM and flushes it when it moves on, at record 12.
        static uint32_t tss[40];
        static int16_t rows[40][NPARAM];
        const int16_t *vals[40];
        for (int i = 0; i < 40; i++) { tss[i] = ts + i * 60; fill(rows[i], tss[i]); vals[i] = rows[i]; }
        hfs_fail_fsyncs = 1;
        e = tsdb_write_batch_h(db, tss, vals, 40);
        hfs_fail_fsyncs = 0;
        ts += 40 * 60;
    } else if (mode == 1) {
        hfs_fail_progs_after = 0;
        e = tsdb_write_h(db, ts, v);
        hfs_fail_progs_after = -1;
    } else {
        // Fill the filesystem with another file (committed 4 KB at a time),
        // leaving no room for the database to grow and only its own active
        // segment to evict.
        bool was = hfs_counting;
        hfs_counting = false;
        FILE *f = fopen(HFS_MOUNT "/filler.bin", "wb");
        static char buf[4096];
        memset(buf, 'f', sizeof(buf));
        while (f && fwrite(buf, sizeof(buf), 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0) {}
        if (f) fclose(f);
        hfs_counting = was;
        e = ESP_OK;
        for (int i = 0; i < 200 && e == ESP_OK; i++) {   // first write that needs space
            e = tsdb_write_h(db, ts, v);
            if (e == ESP_OK) { ts += 60; fill(v, ts); }
        }
        unlink(HFS_MOUNT "/filler.bin");
    }
    CHECK(e != ESP_OK, "%s: tsdb_write returns the failure (got %d)", label, e);
    tsdb_stats_t sb;
    tsdb_get_stats_h(db, &sb);
    scan_t s0 = scan(db);
    CHECK(sb.total_records == s0.n && s0.ordered && s0.values,
          "%s: the database counts only what a scan finds (stats %u, scan %u)", label, sb.total_records, s0.n);
    ts += 60;
    ts = write_n(db, ts, 30);
    tsdb_stats_t sm;
    tsdb_get_stats_h(db, &sm);
    CHECK(sm.total_records == sb.total_records + 30, "%s: writes after the failure succeed (%u -> %u)",
          label, sb.total_records, sm.total_records);
    scan_t s1 = scan(db);
    hfs_remount();   // power cut
    db = tsdb_open(&cfg);
    scan_t s2 = db ? scan(db) : (scan_t){0};
    CHECK(db && s2.n == s1.n && s2.last == s1.last && s2.ordered && s2.values,
          "%s: power cut keeps exactly what was acknowledged (%u records before, %u after, ordered %d values %d)",
          label, s1.n, s2.n, s2.ordered, s2.values);
    if (db) tsdb_close_h(db);
}

// Review B2: a batch that crosses a segment boundary. The full segment's last
// block holds deferred records; moving on flushes it (they are now on disk)
// and then creating the next segment fails. The batch reports the failure,
// but the records it had already flushed are on disk and must be counted —
// "records before the failing one are persisted".
static void fault_batch_segment_create(void) {
    opts_t o = {.fs_blocks = 64, .other_kb = 24};
    mount_fs(&o);
    tsdb_config_t cfg = small_config(20000);
    tsdb_t *db = tsdb_open(&cfg);
    // Fill the first segment to 32 records short of full with single writes.
    uint32_t ts = write_n(db, 1700000000u, 1);
    tsdb_stats_t s;
    int64_t seg_size; int nseg;
    seg_files(&seg_size, &nseg);
    uint32_t room = 0;
    {
        // Records per segment: data blocks (file size - 1 KB header, at full
        // size) x RPB. Derived by writing until a second segment appears.
        uint32_t n = 1;
        for (;;) {
            seg_files(&seg_size, &nseg);
            if (nseg > 1) break;
            ts = write_n(db, ts, 1);
            n++;
        }
        room = n - 1;
    }
    tsdb_close_h(db);
    // Start over: room - 32 records, then a 64-record batch whose 33rd record
    // needs the next segment.
    mount_fs(&o);
    db = tsdb_open(&cfg);
    ts = write_n(db, 1700000000u, room - 32);
    static uint32_t tss[64];
    static int16_t rows[64][NPARAM];
    const int16_t *vals[64];
    for (int i = 0; i < 64; i++) { tss[i] = ts + i * 60; fill(rows[i], tss[i]); vals[i] = rows[i]; }
    hfs_fail_creates = 1;
    hfs_fail_create_match = ".tsdb.s";
    esp_err_t e = tsdb_write_batch_h(db, tss, vals, 64);
    hfs_fail_creates = 0;
    CHECK(e != ESP_OK, "batch across a segment boundary, next segment cannot be created: the batch reports it (%d)", e);
    tsdb_get_stats_h(db, &s);
    scan_t s0 = scan(db);
    hfs_remount();   // power cut
    db = tsdb_open(&cfg);
    tsdb_stats_t sa = {0};
    scan_t s1 = {0};
    if (db) { tsdb_get_stats_h(db, &sa); s1 = scan(db); }
    CHECK(s.total_records == room && s0.n == room && s1.n == room && sa.total_records == room && s1.ordered &&
          s1.values,
          "batch rollback forgets only what did not reach disk: %u records on disk (segment full), stats %u, "
          "scan %u; after a power cut stats %u, scan %u", room, s.total_records, s0.n, sa.total_records, s1.n);
    if (db) {
        int16_t v[NPARAM];
        fill(v, tss[63] + 60);
        CHECK(tsdb_write_h(db, tss[63] + 60, v) == ESP_OK, "next write starts the next segment");
        tsdb_close_h(db);
    }
}

// Review B5: a write on the task that holds a query open (the handle lock is
// recursive, so it is not blocked). A write can close the active segment the
// query reads through (segment roll-over), unlink files and shift the segment
// table under the query's cursor; it must be refused while a query is open on
// the handle, like the schema operations are. Other tasks block on the lock,
// which the query holds from tsdb_query_init to tsdb_query_close.
static void fault_write_during_query(void) {
    opts_t o = {.fs_blocks = 64, .other_kb = 24};
    mount_fs(&o);
    tsdb_config_t cfg = small_config(20000);
    tsdb_t *db = tsdb_open(&cfg);
    uint32_t ts = write_n(db, 1700000000u, 1000);   // segment 1 full, segment 2 active
    tsdb_query_t q;
    uint8_t cols[NPARAM] = {0, 1, 2, 3, 4, 5, 6};
    uint32_t from = ts - 100 * 60;                  // the newest 100: inside the active segment
    tsdb_query_init_h(db, &q, from, 0xFFFFFFFFu, cols, NPARAM);
    uint32_t t; int16_t v[NPARAM], w[NPARAM];
    bool ok = tsdb_query_next(&q, &t, v) == ESP_OK && t == from;
    fill(w, ts);
    esp_err_t e1 = tsdb_write_h(db, ts, w);
    const int16_t *rows[1] = {w};
    uint32_t tss[1] = {ts};
    esp_err_t e2 = tsdb_write_batch_h(db, tss, rows, 1);
    int n = 1;
    while (tsdb_query_next(&q, &t, v) == ESP_OK) {
        fill(w, t);
        if (memcmp(v, w, sizeof(v)) != 0) ok = false;
        n++;
    }
    tsdb_query_close(&q);
    CHECK(e1 == ESP_ERR_INVALID_STATE && e2 == ESP_ERR_INVALID_STATE,
          "write and batch on the query's own task are refused while it is open (got %d, %d)", e1, e2);
    CHECK(ok && n == 100, "the open query is unaffected (%d records)", n);
    fill(w, ts);
    CHECK(tsdb_write_h(db, ts, w) == ESP_OK, "the write succeeds once the query is closed");
    tsdb_close_h(db);
}

// A pre-2.0.1 (V2) file: records_per_block > 38 with the old struct offsets,
// which open converts in place.
static void make_v2_file(const char *path, uint32_t n) {
    tsdb_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = TSDB_MAGIC;
    h.version = 2;
    h.num_params = NPARAM;
    h.param_size = 2;
    h.record_size = 4 + 2 * NPARAM;
    h.records_per_block = RPB;
    h.max_records = 1000;
    h.index_stride = 380;
    h.index_offset = 512;
    h.index_entries = 16;            // data starts at 640, past the 584-byte header
    h.total_records = n;
    h.total_writes = n;
    h.oldest_record_idx = 0;
    h.newest_record_idx = n - 1;
    h.oldest_timestamp = 1700000000u;
    h.newest_timestamp = 1700000000u + (n - 1) * 60;
    for (int i = 0; i < NPARAM; i++) strncpy(h.param_names[i], names[i], 31);
    uint32_t data_off = h.index_offset + h.index_entries * 8;
    uint32_t blocks = (n + RPB - 1) / RPB;
    size_t size = data_off + blocks * TSDB_BLOCK_SIZE;
    uint8_t *img = calloc(1, size);
    memcpy(img, &h, sizeof(h));
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t *blk = img + data_off + b * TSDB_BLOCK_SIZE;
        uint32_t cnt = n - b * RPB < RPB ? n - b * RPB : RPB;
        *(uint32_t *) blk = 0x424C4B54u;
        *(uint16_t *) (blk + 4) = (uint16_t) cnt;
        for (uint32_t r = 0; r < cnt; r++) {
            uint32_t ts = 1700000000u + (b * RPB + r) * 60;
            *(uint32_t *) (blk + 8 + r * 4) = ts;           // OLD_TS_OFFSET
            int16_t v[NPARAM];
            fill(v, ts);
            for (int p = 0; p < NPARAM; p++) {
                uint32_t o = 160 + p * 76 + r * 2;           // OLD_PARAM_OFFSET
                if (o + 2 <= TSDB_BLOCK_SIZE) *(int16_t *) (blk + o) = v[p];
            }
        }
    }
    FILE *f = fopen(path, "wb");
    fwrite(img, size, 1, f);
    fclose(f);
    free(img);
}

static void fault_legacy_migration(void) {
    // The V2 layout only holds records whose old param offsets fit in the
    // block (r < 38 for the last column), so keep n small enough that every
    // value survives the conversion: 37 records.
    const uint32_t n = 37;
    opts_t o = {.fs_blocks = 64, .other_kb = 24};
    mount_fs(&o);
    make_v2_file(DB_PATH, n);
    tsdb_config_t cfg = small_config(1000);
    hfs_fail_progs_after = 0;          // every program fails during the conversion
    tsdb_t *db = tsdb_open(&cfg);
    hfs_fail_progs_after = -1;
    CHECK(db == NULL, "legacy V2->V3 conversion: tsdb_open fails when the conversion cannot be written");
    if (db) { tsdb_close_h(db); }
    hfs_remount();
    db = tsdb_open(&cfg);
    scan_t s = db ? scan(db) : (scan_t){0};
    // Either the file is intact and converted now, or it was discarded as a
    // torn conversion: never adopted with unconverted blocks.
    CHECK(db && s.values && s.ordered && (s.n == n || s.n == 0),
          "legacy V2->V3 conversion: after the failure, the next open sees the converted records or none "
          "(%u records, values %d)", s.n, s.values);
    if (db) tsdb_close_h(db);
}

static int run_faults(void) {
    printf("faults:\n");
    fault_case("fsync failure", 0);
    fault_case("device program failure", 1);
    fault_case("ENOSPC, nothing to evict", 2);
    fault_case("batch, fsync failure mid-batch", 3);
    fault_batch_segment_create();
    fault_write_during_query();
    fault_legacy_migration();
    return 0;
}

// ---------------------------------------------------------------------------
// legacy: failure cases around an adopted 2.x database
// ---------------------------------------------------------------------------

typedef struct { uint32_t n, first, last; } expect_t;

static bool read_expected(const char *dir, expect_t *e) {
    char p[600];
    snprintf(p, sizeof(p), "%s/expected", dir);
    FILE *f = fopen(p, "r");
    bool ok = f && fscanf(f, "%u %u %u", &e->n, &e->first, &e->last) == 3;
    if (f) fclose(f);
    return ok;
}

// Fresh partition (firmware layout) holding the 2.x database from `dir`.
static void setup_legacy(const char *dir, const opts_t *o) {
    mount_fs(o);
    bool was = hfs_counting;
    hfs_counting = false;
    import_legacy(dir);
    hfs_counting = was;
}

static bool exact(const scan_t *s, const expect_t *e) {
    return s->n == e->n && s->first == e->first && s->last == e->last && s->ordered && s->values;
}

// Any file the 2.x database or its conversion leaves besides segments.
static int legacy_leftovers(void) {
    static const char *const f[] = {"/energy.tsdb.h0", "/energy.tsdb.h1", "/energy.tsdb.d/h0",
                                    "/energy.tsdb.d/h1", "/energy.tsdb.d", "/energy.tsdb.mig",
                                    "/energy.tsdb.cv"};
    int n = 0;
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) n += lfs_exists(f[i]);
    return n;
}

// Review B1: removing an adopted 2.x database (here: tsdb_clear) must not
// leave its sidecars behind if the power is cut part-way, and must never
// leave a main file that a reboot would adopt with a stale header (2.4.4's
// newest state lives only in the sidecars). Power cut before every mutating
// operation of the clear; after the reboot, the database must read as the
// whole adopted history or as empty, and once the main file is gone no
// sidecar may remain.
static void legacy_clear_crash_sweep(const char *dir, const opts_t *o, const expect_t *e,
                                     const tsdb_config_t *cfg) {
    setup_legacy(dir, o);
    tsdb_t *db = tsdb_open(cfg);
    uint64_t m0 = hfs_mut_ops;
    tsdb_clear_h(db);
    uint32_t ops = (uint32_t) (hfs_mut_ops - m0);
    tsdb_close_h(db);
    uint32_t orphaned = 0, stale = 0, failed_open = 0, orphan_at = 0, stale_at = 0;
    for (uint32_t k = 0; k < ops; k++) {
        setup_legacy(dir, o);
        db = tsdb_open(cfg);
        jmp_buf jb;
        hfs_crash_jmp = &jb;
        if (setjmp(jb) == 0) {
            hfs_crash_after = k;
            tsdb_clear_h(db);
        }
        hfs_crash_after = -1;
        hfs_remount();
        db = tsdb_open(cfg);
        if (db == NULL) { failed_open++; continue; }
        scan_t s = scan(db);
        if (!(s.n == 0 || exact(&s, e))) { if (!stale) stale_at = k; stale++; }
        if (!lfs_exists("/energy.tsdb") && legacy_leftovers() > 0) { if (!orphaned) orphan_at = k; orphaned++; }
        tsdb_close_h(db);
    }
    CHECK(failed_open == 0 && stale == 0,
          "clear, power cut before each of its %u mutating operations: the reboot reads the whole history "
          "or none (%u stale, first at op %u; %u failed opens)", ops, stale, stale_at, failed_open);
    CHECK(orphaned == 0,
          "clear, power cut part-way: no sidecar outlives the main file (%u of %u crash points left some, "
          "first at op %u)", orphaned, ops, orphan_at);
}

static int run_legacy(const char *dir, const opts_t *o) {
    expect_t e;
    if (!read_expected(dir, &e)) { printf("cannot read %s/expected\n", dir); return 1; }
    tsdb_config_t cfg = fw_config(fw_max_records(o->fs_blocks * 4096));
    printf("legacy %s: %u records %u..%u, max_records %u\n", dir, e.n, e.first, e.last, cfg.max_records);
    legacy_clear_crash_sweep(dir, o, &e, &cfg);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_lfs_upgrade upgrade <legacy_dir> | legacy <legacy_dir> | fresh | faults [opts]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    opts_t o = {.fs_blocks = 864, .other_kb = 0, .interval = 300, .min_free_kb = 0, .writes = 0, .multiple = 2.2};
    bool other_set = false, floor_set = false;
    const char *mode = argv[1];
    const char *dir = NULL;
    int i = 2;
    if (strcmp(mode, "upgrade") == 0 || strcmp(mode, "legacy") == 0) {
        if (argc < 3) return 2;
        dir = argv[2];
        i = 3;
    }
    for (; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "0";
#define OPT(name, dst, setflag) if (strcmp(a, name) == 0) { dst = strtoul(v, NULL, 0); setflag; i++; continue; }
        OPT("--fs-blocks", o.fs_blocks, (void) 0);
        OPT("--other-kb", o.other_kb, other_set = true);
        OPT("--writes", o.writes, (void) 0);
        OPT("--interval", o.interval, (void) 0);
        OPT("--min-free-kb", o.min_free_kb, floor_set = true);
        OPT("--guard-kb", o.guard_kb, (void) 0);
#undef OPT
        if (strcmp(a, "--multiple") == 0) { o.multiple = atof(v); i++; continue; }
        fprintf(stderr, "unknown option %s\n", a);
        return 2;
    }
    bool small = o.fs_blocks * 4096u <= 384u * 1024u;
    if (!other_set) o.other_kb = small ? 8 : 400;
    // The floor every other LittleFS user can count on. The firmware sets no
    // free-space guard; this is what the other files need to keep saving.
    if (!floor_set) o.min_free_kb = small ? 16 : 128;

    g_guard_kb = o.guard_kb;
    int r;
    if (strcmp(mode, "upgrade") == 0) r = run_upgrade(dir, &o);
    else if (strcmp(mode, "legacy") == 0) r = run_legacy(dir, &o);
    else if (strcmp(mode, "fresh") == 0) r = run_fresh(&o);
    else if (strcmp(mode, "faults") == 0) r = run_faults();
    else return 2;
    if (r != 0) fails++;
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
