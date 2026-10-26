#pragma once
// Host shim: run esp_tsdb's stdio file I/O on a real littlefs over a RAM block
// device, with esp_littlefs's VFS semantics, and count block-device traffic.
//
// Linked with -Wl,--wrap=fopen,--wrap=fileno,--wrap=fsync,--wrap=unlink,
// --wrap=rename,--wrap=stat,--wrap=mkdir. Paths under HFS_MOUNT ("/littlefs")
// go to littlefs; anything else falls through to the host libc.
//
// esp_littlefs (joltwallet, v1.2x) behaviours reproduced here:
//   - every file is opened with one user attribute ('t', 8-byte mtime) when
//     CONFIG_LITTLEFS_USE_MTIME=y. littlefs marks a writable file with attrs
//     DIRTY at open, so the attribute is rewritten on every sync/close.
//   - a writable open is followed immediately by lfs_file_sync()
//     ("Rewriting file frees space immediately (#7426)") — i.e. one metadata
//     commit per writable fopen, even for "r+b".
//   - fsync() and close() set mtime = time(NULL) then sync.
//   - newlib FILE buffering (ESP-IDF's BUFSIZ is 128).
#include <stdbool.h>
#include <stdint.h>
#include "lfs.h"
#include "lfs_harness_hooks.h"

#define HFS_MOUNT "/littlefs"

typedef struct {
    lfs_size_t read_size, prog_size, cache_size, lookahead_size;
    lfs_size_t block_size, block_count;
    int32_t block_cycles;
    lfs_size_t metadata_max, inline_max, name_max;
    bool mtime;          // CONFIG_LITTLEFS_USE_MTIME
    size_t stdio_buf;    // FILE buffer size (newlib BUFSIZ on ESP-IDF = 128)
} hfs_config_t;

// The firmware's config: esp_littlefs defaults from sdkconfig, 3.375 MB.
hfs_config_t hfs_default_config(void);

typedef struct {
    uint64_t reads, read_bytes;     // block-device read calls / bytes
    uint64_t progs, prog_bytes;
    uint64_t erases;
    uint64_t ev[LFS_H_COUNT];       // instrumented littlefs events
    uint64_t root_commits;          // commits to a metadata pair of "/"
    uint64_t root_compacts;
    uint64_t root_splits;
} hfs_counters_t;

extern hfs_counters_t hfs_ctr;      // cumulative since mount
extern bool hfs_counting;           // false = do not count (bookkeeping I/O)

int  hfs_format_and_mount(const hfs_config_t *cfg);
int  hfs_remount(void);             // unmount + mount (simulated reboot)
void hfs_unmount(void);
lfs_t *hfs_lfs(void);

// Re-learn which metadata pairs make up the root directory (it can split and
// relocate). Cheap; does not count.
void hfs_refresh_root_pairs(void);

// Per-phase attribution: every stdio op is tagged "<op>:<class>" where class
// is data (*.tsdb), side (sidecar slots) or other.
#define HFS_MAX_PHASES 32
typedef struct {
    char name[32];
    hfs_counters_t c;
    uint64_t calls;
} hfs_phase_t;
extern hfs_phase_t hfs_phases[HFS_MAX_PHASES];
extern int hfs_nphases;
void hfs_reset_phases(void);

// Direct helpers for the test (bypass stdio).
int hfs_write_file(const char *path, const void *data, size_t len);

// Free bytes on the filesystem (block_count - lfs_fs_size, in bytes). Not counted.
int64_t hfs_free_bytes(void);
// Sum of regular-file sizes in dir_path whose name starts with prefix. Not counted.
int64_t hfs_tree_bytes(const char *dir_path, const char *prefix);
// Fault injection: when >= 0, the block device fails every program after this
// many more succeed (LFS_ERR_IO). -1 = off.
extern int hfs_fail_progs_after;
// Every LFS_ERR_NOSPC littlefs returned through the stdio mapping.
extern uint64_t hfs_nospc;
// Fault injection: the next N fsync() calls on littlefs files fail with EIO
// without syncing.
extern int hfs_fail_fsyncs;
