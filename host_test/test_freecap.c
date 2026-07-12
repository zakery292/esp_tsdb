// Host regression for the free-space guard (tsdb_config_t.free_space_cb /
// min_free_bytes) and the close/reopen handle-recycle lifecycle.
//
// The guard: before the data file grows by a new block, the engine calls
// free_space_cb(); below min_free_bytes it caps max_records at the current
// count and rings from then on — BEFORE hard ENOSPC, so other files on the
// filesystem keep their free-block reserve.
//
// The recycler: LIFECYCLE SEMANTICS ONLY. The host shims are single-threaded
// (semaphores are no-op stubs), so the concurrency this design exists for —
// a straggler blocked on the mutex across a close, the memset-while-blocked
// window, cross-incarnation generation detection — is NOT exercised here.
// This test verifies: close → global API fails fast; repeated close/reopen
// cycles reuse the parked handle without crash or state bleed. A TSan
// threaded harness (writer thread vs close/init loop) is the missing piece.
//
// Build with ./build_freecap.sh
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// tmpfs: fsync-bound on real disks, and this test does ~4000 fsync'd writes
#define DB_PATH "/dev/shm/tsdb_freecap_test.bin"
#define T0 100000u

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    int _ok = (cond); \
    printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", msg); \
    if (!_ok) g_fail++; \
} while (0)

// Controllable fake filesystem probe.
static uint64_t g_fake_free = UINT64_MAX;
static int g_probe_calls = 0;
static uint64_t fake_free_space(void) { g_probe_calls++; return g_fake_free; }

static tsdb_config_t base_cfg(void) {
    tsdb_config_t c = {0};
    c.filepath = DB_PATH;
    c.num_params = 1;                 // record_size 6 -> 681 records/block
    c.max_records = 5000;
    c.index_stride = 100;
    c.buffer_pool_size = 16 * 1024;
    c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    c.free_space_cb = fake_free_space;
    c.min_free_bytes = 100000;
    return c;
}

static void write_n(int n, uint32_t *ts_next) {
    for (int i = 0; i < n; i++) {
        int16_t v = (int16_t)(*ts_next & 0x7FFF);
        if (tsdb_write(*ts_next, &v) != ESP_OK) { printf("write FAIL at ts=%u\n", *ts_next); exit(2); }
        *ts_next += 10;
    }
}

int main(void) {
    uint32_t ts = T0;

    printf("Part 1: guard caps capacity at a block boundary\n");
    remove(DB_PATH);
    tsdb_config_t c = base_cfg();
    if (tsdb_init(&c) != ESP_OK) { printf("init FAIL\n"); return 2; }

    g_fake_free = UINT64_MAX;          // plenty of space: grow several blocks
    g_probe_calls = 0;
    write_n(1500, &ts);
    tsdb_stats_t st;
    tsdb_get_stats(&st);
    CHECK(st.total_records == 1500, "1500 records written");
    CHECK(g_probe_calls >= 1, "probe consulted at block boundaries during growth");
    CHECK(st.max_records == 5000, "capacity untouched while space is fine");

    g_fake_free = 50000;               // below min_free_bytes=100000
    int capped_at = -1;
    for (int i = 0; i < 800; i++) {    // cap fires at the next block boundary
        write_n(1, &ts);
        tsdb_get_stats(&st);
        if (st.max_records < 5000) { capped_at = i; break; }
    }
    tsdb_get_stats(&st);
    CHECK(capped_at >= 0, "capacity capped within one block of low space");
    CHECK(st.max_records == st.total_records, "cap == record count at cap time");
    uint32_t cap = st.max_records;

    write_n(700, &ts);                 // keep writing well past the cap
    tsdb_get_stats(&st);
    CHECK(st.max_records == cap, "cap sticky across further writes");
    CHECK(st.total_records == cap, "ring stays at capacity");
    CHECK(st.total_evictions >= 700, "writes proceed as LRU evictions");

    // Cap must survive close/reopen (persisted in the header).
    tsdb_close();
    c = base_cfg();
    if (tsdb_init(&c) != ESP_OK) { printf("reopen FAIL\n"); return 2; }
    tsdb_get_stats(&st);
    CHECK(st.max_records == cap, "cap persisted across reopen");

    printf("Part 2: failed probe (UINT64_MAX) never caps\n");
    tsdb_close();
    remove(DB_PATH);
    c = base_cfg();
    if (tsdb_init(&c) != ESP_OK) { printf("init2 FAIL\n"); return 2; }
    g_fake_free = UINT64_MAX;          // \"unknown\" — must not cap
    ts = T0;
    write_n(1500, &ts);                // grow across multiple blocks
    tsdb_get_stats(&st);
    CHECK(st.max_records == 5000, "unknown free space never latches a cap");
    CHECK(st.total_records == 1500, "growth unhindered");

    printf("Part 3: close/reopen recycle lifecycle\n");
    tsdb_close();
    int16_t v = 1;
    CHECK(tsdb_write(T0, &v) != ESP_OK, "global write fails fast after close");
    CHECK(!tsdb_is_initialized(), "not initialized after close");
    for (int i = 0; i < 5; i++) {      // repeated cycles reuse the parked handle
        c = base_cfg();
        if (tsdb_init(&c) != ESP_OK) { printf("cycle init FAIL\n"); return 2; }
        tsdb_get_stats(&st);
        if (st.total_records != 1500) { g_fail++; printf("  [FAIL] cycle %d record count\n", i); }
        if (tsdb_close() != ESP_OK) { g_fail++; printf("  [FAIL] cycle %d close\n", i); }
    }
    CHECK(1, "5 close/reopen cycles completed (recycler reuse, no crash)");

    remove(DB_PATH);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
