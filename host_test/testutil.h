// Shared helpers for the host suites (3.0 segment format).
#pragma once
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Remove a database completely: legacy file, sidecars, .mig and segments.
static inline void tsdb_test_wipe(const char *path) {
    char b[300];
    remove(path);
    snprintf(b, sizeof b, "%s.h0", path); remove(b);
    snprintf(b, sizeof b, "%s.h1", path); remove(b);
    snprintf(b, sizeof b, "%s.mig", path); remove(b);
    for (int i = 1; i < 2000; i++) { snprintf(b, sizeof b, "%s.s%010d", path, i); if (remove(b) != 0 && i > 200) break; }
}

static inline int tsdb_test_exists(const char *path) {
    struct stat st; return stat(path, &st) == 0;
}

// Number of segment files present for a database path.
static inline int tsdb_test_segments(const char *path) {
    char b[300]; int n = 0;
    for (int i = 1; i < 2000; i++) { snprintf(b, sizeof b, "%s.s%010d", path, i); if (tsdb_test_exists(b)) n++; }
    return n;
}

// Total bytes across legacy file + segments.
static inline long tsdb_test_bytes(const char *path) {
    char b[300]; long t = 0; struct stat st;
    if (stat(path, &st) == 0) t += st.st_size;
    for (int i = 1; i < 2000; i++) { snprintf(b, sizeof b, "%s.s%010d", path, i); if (stat(b, &st) == 0) t += st.st_size; }
    return t;
}
