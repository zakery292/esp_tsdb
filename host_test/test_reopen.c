// Regression for reopening a ring that has taken more than 2 * max_records
// writes: the header must be accepted and the next write must replace the
// oldest record.
//
// Build with ./build_reopen.sh (gcc, ESP-IDF deps shimmed under shims/).
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH "/tmp/u_reopen.tsdb"
#define MAX 100u
#define T0 100000u
#define DT 10u
#define TS(i) (T0 + (uint32_t)(i) * DT)

static int fails = 0;

static void check(const char *label, uint32_t got, uint32_t want) {
    int ok = got == want;
    printf("  [%s] got=%u exp=%u -> %s\n", label, got, want, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static void db_open(void) {
    tsdb_config_t c = {0};
    c.filepath = PATH; c.num_params = 1; c.max_records = MAX; c.index_stride = 10;
    c.buffer_pool_size = 16 * 1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    if (tsdb_init(&c) != ESP_OK) { printf("tsdb_init FAIL\n"); exit(2); }
}

static void db_remove(void) {
    remove(PATH); remove(PATH ".h0"); remove(PATH ".h1");
    remove(PATH ".d/h0"); remove(PATH ".d/h1"); remove(PATH ".d");
}

static void db_write(int from, int to) {
    for (int i = from; i < to; i++) {
        int16_t val = (int16_t)i;
        if (tsdb_write(TS(i), &val) != ESP_OK) { printf("write FAIL\n"); exit(2); }
    }
}

// The ring must hold exactly records [next - MAX, next), in time order.
static void check_ring(int next) {
    tsdb_query_t qy; uint8_t cols[1] = {0};
    if (tsdb_query_init(&qy, 0, UINT32_MAX, cols, 1) != ESP_OK) { printf("query FAIL\n"); exit(2); }
    uint32_t ts, prev = 0, first = 0, count = 0, disorder = 0; int16_t v[1];
    while (tsdb_query_next(&qy, &ts, v) == ESP_OK) {
        if (count == 0) first = ts;
        else if (ts != prev + DT) disorder++;
        prev = ts; count++;
    }
    tsdb_query_close(&qy);
    check("retained", count, MAX);
    check("oldest", first, TS(next - (int)MAX));
    check("newest", prev, TS(next - 1));
    check("gaps", disorder, 0);
}

static void check_stats(uint32_t writes) {
    tsdb_stats_t s;
    if (tsdb_get_stats(&s) != ESP_OK) { printf("stats FAIL\n"); exit(2); }
    check("writes", s.total_writes, writes);
    check("evictions", s.total_evictions, writes - MAX);
}

// Run setup in a child that exits with the database open, as a power cut would.
static void crash_after(int writes, int sync_at) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        db_open();
        if (sync_at) { db_write(0, sync_at); tsdb_close(); db_open(); }  // folds the header
        db_write(sync_at, writes);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { printf("child FAIL\n"); exit(2); }
}

// Spoil the magic of a cleanly closed file so the next open reconstructs it.
static void spoil_header(void) {
    FILE *f = fopen(PATH, "r+b");
    uint32_t zero = 0;
    if (!f || fwrite(&zero, sizeof(zero), 1, f) != 1) { printf("spoil FAIL\n"); exit(2); }
    fclose(f);
}

int main(void) {
    // Both sides of the old bound, and a ring ending on its last slot.
    const int counts[] = {200, 201, 250, 300, 1234};
    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
        int w = counts[n];
        printf("A) %d writes, clean close, reopen\n", w);
        db_remove(); db_open(); db_write(0, w); tsdb_close();
        db_open(); check_stats(w); check_ring(w);
        db_write(w, w + 7); check_stats(w + 7); check_ring(w + 7);
        tsdb_close();

        printf("B) %d writes, power cut, reopen\n", w);
        db_remove(); crash_after(w, 0);
        db_open(); check_stats(w); check_ring(w);
        db_write(w, w + 7); check_ring(w + 7);
        tsdb_close();

        printf("D) %d writes, corrupt header, reconstruct\n", w);
        db_remove(); db_open(); db_write(0, w); tsdb_close();
        spoil_header(); db_open(); check_ring(w);
        db_write(w, w + 7); check_ring(w + 7);
        tsdb_close();
        db_open(); check_ring(w + 7); tsdb_close();
    }

    printf("C) in-file header below the old bound, sidecar above it, power cut\n");
    db_remove(); crash_after(250, 150);
    db_open(); check_stats(250); check_ring(250);
    tsdb_close();

    db_remove();
    printf("\n%s (%d failed)\n", fails ? "*** FAIL ***" : "*** PASS ***", fails);
    return fails ? 1 : 0;
}
