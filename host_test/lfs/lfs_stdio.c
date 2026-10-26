// See lfs_stdio.h.
#define _GNU_SOURCE
#include "lfs_stdio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

hfs_counters_t hfs_ctr;
int hfs_fail_fsyncs;
bool hfs_counting = true;
hfs_phase_t hfs_phases[HFS_MAX_PHASES];
int hfs_nphases;

static lfs_t g_lfs;
static struct lfs_config g_cfg;
static hfs_config_t g_hcfg;
static uint8_t *g_ram;
static bool g_mounted;
static int g_phase = -1;

#define ATTR_MTIME ((uint8_t) 't')

// ---------------------------------------------------------------------------
// counters
// ---------------------------------------------------------------------------

static int phase_index(const char *name) {
    for (int i = 0; i < hfs_nphases; i++) {
        if (strcmp(hfs_phases[i].name, name) == 0) return i;
    }
    if (hfs_nphases >= HFS_MAX_PHASES) return -1;
    snprintf(hfs_phases[hfs_nphases].name, sizeof(hfs_phases[0].name), "%s", name);
    return hfs_nphases++;
}

void hfs_reset_phases(void) {
    memset(hfs_phases, 0, sizeof(hfs_phases));
    hfs_nphases = 0;
}

static const char *path_class(const char *p) {
    size_t n = strlen(p);
    if (strstr(p, ".tsdb.h") != NULL || strstr(p, ".tsdb.d") != NULL) return "side";
    if (strstr(p, ".tsdb.s") != NULL) return "seg";
    if (n >= 5 && strcmp(p + n - 5, ".tsdb") == 0) return "data";
    if (strstr(p, ".mig") != NULL) return "mig";
    return "other";
}

static int phase_begin(const char *op, const char *path) {
    int prev = g_phase;
    char name[32];
    snprintf(name, sizeof(name), "%s:%s", op, path_class(path));
    g_phase = phase_index(name);
    if (g_phase >= 0 && hfs_counting) hfs_phases[g_phase].calls++;
    return prev;
}
static void phase_end(int prev) { g_phase = prev; }

#define BUMP(field, n) do { \
        if (hfs_counting) { \
            hfs_ctr.field += (n); \
            if (g_phase >= 0) hfs_phases[g_phase].c.field += (n); \
        } } while (0)

static uint32_t g_root_pairs[32][2];
static int g_nroot;

static bool is_root_pair(const uint32_t *pair) {
    if (pair == NULL) return false;
    for (int i = 0; i < g_nroot; i++) {
        if ((pair[0] == g_root_pairs[i][0] && pair[1] == g_root_pairs[i][1]) ||
            (pair[0] == g_root_pairs[i][1] && pair[1] == g_root_pairs[i][0])) {
            return true;
        }
    }
    return false;
}

void hfs_refresh_root_pairs(void) {
    bool was = hfs_counting;
    hfs_counting = false;
    g_nroot = 0;
    lfs_dir_t d;
    if (lfs_dir_open(&g_lfs, &d, "/") == 0) {
        struct lfs_info info;
        do {
            bool seen = is_root_pair(d.m.pair);
            if (!seen && g_nroot < 32) {
                g_root_pairs[g_nroot][0] = d.m.pair[0];
                g_root_pairs[g_nroot][1] = d.m.pair[1];
                g_nroot++;
            }
        } while (lfs_dir_read(&g_lfs, &d, &info) > 0);
        lfs_dir_close(&g_lfs, &d);
    }
    hfs_counting = was;
}

void lfs_harness_hook(int kind, const uint32_t *pair) {
    BUMP(ev[kind], 1);
    if (is_root_pair(pair)) {
        if (kind == LFS_H_COMMIT) BUMP(root_commits, 1);
        if (kind == LFS_H_COMPACT) BUMP(root_compacts, 1);
        if (kind == LFS_H_SPLIT) BUMP(root_splits, 1);
    }
}

// ---------------------------------------------------------------------------
// RAM block device
// ---------------------------------------------------------------------------

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t sz) {
    memcpy(buf, g_ram + (size_t) b * c->block_size + off, sz);
    BUMP(reads, 1);
    BUMP(read_bytes, sz);
    return 0;
}
extern int hfs_fail_progs_after;
static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t sz) {
    if (hfs_fail_progs_after == 0) return LFS_ERR_IO;          // injected device failure
    if (hfs_fail_progs_after > 0) hfs_fail_progs_after--;
    uint8_t *dst = g_ram + (size_t) b * c->block_size + off;
    for (lfs_size_t i = 0; i < sz; i++) dst[i] &= ((const uint8_t *) buf)[i];  // NOR semantics
    BUMP(progs, 1);
    BUMP(prog_bytes, sz);
    return 0;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t b) {
    memset(g_ram + (size_t) b * c->block_size, 0xFF, c->block_size);
    BUMP(erases, 1);
    return 0;
}
static int bd_sync(const struct lfs_config *c) { (void) c; return 0; }

hfs_config_t hfs_default_config(void) {
    hfs_config_t h = {0};
    h.read_size = 128;        // CONFIG_LITTLEFS_READ_SIZE
    h.prog_size = 128;        // CONFIG_LITTLEFS_WRITE_SIZE
    h.cache_size = 512;       // CONFIG_LITTLEFS_CACHE_SIZE
    h.lookahead_size = 128;   // CONFIG_LITTLEFS_LOOKAHEAD_SIZE
    h.block_size = 4096;
    h.block_count = 864;      // 0x360000 = 3.375 MB partition
    h.block_cycles = 512;     // CONFIG_LITTLEFS_BLOCK_CYCLES
    h.metadata_max = 0;       // esp_littlefs leaves these at littlefs defaults
    h.inline_max = 0;
    h.name_max = 0;           // LFS_NAME_MAX (255); OBJ_NAME_LEN is VFS-side only
    h.mtime = true;           // CONFIG_LITTLEFS_USE_MTIME=y, MTIME_USE_SECONDS
    h.stdio_buf = 128;
    return h;
}

static void fill_cfg(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.read = bd_read;
    g_cfg.prog = bd_prog;
    g_cfg.erase = bd_erase;
    g_cfg.sync = bd_sync;
    g_cfg.read_size = g_hcfg.read_size;
    g_cfg.prog_size = g_hcfg.prog_size;
    g_cfg.block_size = g_hcfg.block_size;
    g_cfg.block_count = g_hcfg.block_count;
    g_cfg.cache_size = g_hcfg.cache_size;
    g_cfg.lookahead_size = g_hcfg.lookahead_size;
    g_cfg.block_cycles = g_hcfg.block_cycles;
    g_cfg.metadata_max = g_hcfg.metadata_max;
    g_cfg.inline_max = g_hcfg.inline_max;
    g_cfg.name_max = g_hcfg.name_max;
}

int hfs_format_and_mount(const hfs_config_t *cfg) {
    if (g_mounted) hfs_unmount();
    g_hcfg = *cfg;
    free(g_ram);
    g_ram = malloc((size_t) cfg->block_size * cfg->block_count);
    memset(g_ram, 0xFF, (size_t) cfg->block_size * cfg->block_count);
    fill_cfg();
    int err = lfs_format(&g_lfs, &g_cfg);
    if (err) return err;
    err = lfs_mount(&g_lfs, &g_cfg);
    g_mounted = (err == 0);
    memset(&hfs_ctr, 0, sizeof(hfs_ctr));
    hfs_refresh_root_pairs();
    return err;
}

void hfs_unmount(void) {
    if (g_mounted) lfs_unmount(&g_lfs);
    g_mounted = false;
}

static void detach_open_files(void);
int hfs_remount(void) {
    // Every FILE still open belongs to the "previous boot": detach them so
    // later stdio calls on them (glibc flushes streams at exit) cannot reach
    // the new mount.
    detach_open_files();
    hfs_unmount();
    int err = lfs_mount(&g_lfs, &g_cfg);
    g_mounted = (err == 0);
    hfs_refresh_root_pairs();
    return err;
}

lfs_t *hfs_lfs(void) { return &g_lfs; }

// ---------------------------------------------------------------------------
// stdio mapping (fopencookie)
// ---------------------------------------------------------------------------

typedef struct hfs_file {
    lfs_file_t file;
    struct lfs_file_config fcfg;
    struct lfs_attr attr;
    int64_t mtime;              // time_t is 64-bit on ESP-IDF 5
    uint8_t *cache;
    char path[192];
    int fd;
    FILE *fp;
    bool writable;
    bool dead;                  // abandoned by hfs_remount (a power cut)
} hfs_file_t;

#define HFS_MAX_FILES 16
#define HFS_FD_BASE 0x4000
static hfs_file_t *g_files[HFS_MAX_FILES];

static void detach_open_files(void) {
    for (int i = 0; i < HFS_MAX_FILES; i++) {
        if (g_files[i] != NULL) { g_files[i]->dead = true; g_files[i] = NULL; }
    }
}

static const char *lfs_path(const char *p) {
    size_t n = strlen(HFS_MOUNT);
    if (p != NULL && strncmp(p, HFS_MOUNT, n) == 0 && (p[n] == '/' || p[n] == '\0')) {
        return p[n] ? p + n : "/";
    }
    return NULL;
}

uint64_t hfs_nospc;

static int lfs_to_errno(int e) {
    if (e == LFS_ERR_NOSPC) hfs_nospc++;
    switch (e) {
        case LFS_ERR_NOENT: return ENOENT;
        case LFS_ERR_EXIST: return EEXIST;
        case LFS_ERR_NOSPC: return ENOSPC;
        case LFS_ERR_ISDIR: return EISDIR;
        case LFS_ERR_NOTDIR: return ENOTDIR;
        case LFS_ERR_NOTEMPTY: return ENOTEMPTY;
        case LFS_ERR_NAMETOOLONG: return ENAMETOOLONG;
        case LFS_ERR_NOMEM: return ENOMEM;
        case LFS_ERR_INVAL: return EINVAL;
        default: return EIO;
    }
}

// esp_littlefs_file_sync(): refresh mtime (writable only), then lfs_file_sync
static int esp_sync(hfs_file_t *h) {
    if (g_hcfg.mtime && h->writable) h->mtime = (int64_t) time(NULL);
    return lfs_file_sync(&g_lfs, &h->file);
}

static ssize_t ck_read(void *ck, char *buf, size_t size) {
    hfs_file_t *h = ck;
    if (h->dead) { errno = EIO; return -1; }
    int prev = phase_begin("read", h->path);
    lfs_ssize_t r = lfs_file_read(&g_lfs, &h->file, buf, size);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return r;
}

static ssize_t ck_write(void *ck, const char *buf, size_t size) {
    hfs_file_t *h = ck;
    if (h->dead) { errno = EIO; return 0; }
    int prev = phase_begin("write", h->path);
    lfs_ssize_t r = lfs_file_write(&g_lfs, &h->file, buf, size);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return 0; }
    return r;
}

static int ck_seek(void *ck, off64_t *pos, int whence) {
    hfs_file_t *h = ck;
    if (h->dead) { errno = EIO; return -1; }
    int lw = whence == SEEK_SET ? LFS_SEEK_SET : whence == SEEK_CUR ? LFS_SEEK_CUR : LFS_SEEK_END;
    int prev = phase_begin("seek", h->path);
    lfs_soff_t r = lfs_file_seek(&g_lfs, &h->file, (lfs_soff_t) *pos, lw);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    *pos = r;
    return 0;
}

static int ck_close(void *ck) {
    hfs_file_t *h = ck;
    if (h->dead) { free(h->cache); free(h); return 0; }
    int prev = phase_begin("close", h->path);
    if (g_hcfg.mtime && h->writable) h->mtime = (int64_t) time(NULL);
    int r = lfs_file_close(&g_lfs, &h->file);
    phase_end(prev);
    g_files[h->fd - HFS_FD_BASE] = NULL;
    free(h->cache);
    free(h);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_fopen(path, mode);

    int flags;
    bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
        case 'r': flags = plus ? LFS_O_RDWR : LFS_O_RDONLY; break;
        case 'w': flags = (plus ? LFS_O_RDWR : LFS_O_WRONLY) | LFS_O_CREAT | LFS_O_TRUNC; break;
        case 'a': flags = (plus ? LFS_O_RDWR : LFS_O_WRONLY) | LFS_O_CREAT | LFS_O_APPEND; break;
        default: errno = EINVAL; return NULL;
    }

    int slot = -1;
    for (int i = 0; i < HFS_MAX_FILES; i++) if (g_files[i] == NULL) { slot = i; break; }
    if (slot < 0) { errno = EMFILE; return NULL; }

    hfs_file_t *h = calloc(1, sizeof(*h));
    h->cache = malloc(g_cfg.cache_size);
    snprintf(h->path, sizeof(h->path), "%s", path);
    h->fcfg.buffer = h->cache;
    if (g_hcfg.mtime) {
        h->attr.type = ATTR_MTIME;
        h->attr.buffer = &h->mtime;
        h->attr.size = sizeof(h->mtime);
        h->fcfg.attrs = &h->attr;
        h->fcfg.attr_count = 1;
    }
    h->writable = (flags & 3) != LFS_O_RDONLY;

    int prev = phase_begin("open", path);
    int err = lfs_file_opencfg(&g_lfs, &h->file, lp, flags, &h->fcfg);
    if (err == 0 && h->writable) {
        // esp_littlefs syncs right after every writable open
        esp_sync(h);
    }
    phase_end(prev);
    if (err < 0) {
        free(h->cache);
        free(h);
        errno = lfs_to_errno(err);
        return NULL;
    }

    cookie_io_functions_t io = { ck_read, ck_write, ck_seek, ck_close };
    char cmode[8];
    snprintf(cmode, sizeof(cmode), "%s", mode);
    FILE *fp = fopencookie(h, cmode, io);
    if (fp == NULL) {
        lfs_file_close(&g_lfs, &h->file);
        free(h->cache);
        free(h);
        return NULL;
    }
    if (g_hcfg.stdio_buf > 0) setvbuf(fp, NULL, _IOFBF, g_hcfg.stdio_buf);
    h->fd = HFS_FD_BASE + slot;
    h->fp = fp;
    g_files[slot] = h;
    return fp;
}

int __real_fileno(FILE *fp);
int __wrap_fileno(FILE *fp) {
    for (int i = 0; i < HFS_MAX_FILES; i++) {
        if (g_files[i] != NULL && g_files[i]->fp == fp) return g_files[i]->fd;
    }
    return __real_fileno(fp);
}

int __real_fsync(int fd);
int __wrap_fsync(int fd) {
    int i = fd - HFS_FD_BASE;
    if (i < 0 || i >= HFS_MAX_FILES || g_files[i] == NULL) return __real_fsync(fd);
    hfs_file_t *h = g_files[i];
    if (hfs_fail_fsyncs > 0) { hfs_fail_fsyncs--; errno = EIO; return -1; }   // injected
    int prev = phase_begin("fsync", h->path);
    int r = esp_sync(h);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

int __real_unlink(const char *path);
int __wrap_unlink(const char *path) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_unlink(path);
    int prev = phase_begin("unlink", path);
    int r = lfs_remove(&g_lfs, lp);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

int __real_rename(const char *a, const char *b);
int __wrap_rename(const char *a, const char *b) {
    const char *la = lfs_path(a), *lb = lfs_path(b);
    if (la == NULL && lb == NULL) return __real_rename(a, b);
    if (la == NULL || lb == NULL) { errno = EXDEV; return -1; }
    int prev = phase_begin("rename", b);
    int r = lfs_rename(&g_lfs, la, lb);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

int __real_mkdir(const char *path, mode_t mode);
int __wrap_mkdir(const char *path, mode_t mode) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_mkdir(path, mode);
    int prev = phase_begin("mkdir", path);
    int r = lfs_mkdir(&g_lfs, lp);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

int __real_rmdir(const char *path);
int __wrap_rmdir(const char *path) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_rmdir(path);
    int prev = phase_begin("rmdir", path);
    int r = lfs_remove(&g_lfs, lp);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

int __real_stat(const char *path, struct stat *st);
int __wrap_stat(const char *path, struct stat *st) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_stat(path, st);
    struct lfs_info info;
    int prev = phase_begin("stat", path);
    int r = lfs_stat(&g_lfs, lp, &info);
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = info.size;
    st->st_mode = info.type == LFS_TYPE_DIR ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_blksize = g_cfg.block_size;
    return 0;
}

int hfs_write_file(const char *path, const void *data, size_t len) {
    FILE *f = __wrap_fopen(path, "wb");
    if (f == NULL) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Directories (3.x scans for segment files) and truncate
// ---------------------------------------------------------------------------

#include <dirent.h>

typedef struct {
    lfs_dir_t dir;
    struct dirent ent;
    char path[192];
} hfs_dir_t;

#define HFS_MAX_DIRS 4
static hfs_dir_t *g_dirs[HFS_MAX_DIRS];

static int dir_slot(const void *p) {
    for (int i = 0; i < HFS_MAX_DIRS; i++) if (g_dirs[i] != NULL && (const void *) g_dirs[i] == p) return i;
    return -1;
}

DIR *__real_opendir(const char *path);
DIR *__wrap_opendir(const char *path) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_opendir(path);
    int slot = -1;
    for (int i = 0; i < HFS_MAX_DIRS; i++) if (g_dirs[i] == NULL) { slot = i; break; }
    if (slot < 0) { errno = EMFILE; return NULL; }
    hfs_dir_t *d = calloc(1, sizeof(*d));
    snprintf(d->path, sizeof(d->path), "%s", path);
    int prev = phase_begin("opendir", path);
    int r = lfs_dir_open(&g_lfs, &d->dir, lp);
    phase_end(prev);
    if (r < 0) { free(d); errno = lfs_to_errno(r); return NULL; }
    g_dirs[slot] = d;
    return (DIR *) d;
}

struct dirent *__real_readdir(DIR *dp);
struct dirent *__wrap_readdir(DIR *dp) {
    int slot = dir_slot(dp);
    if (slot < 0) return __real_readdir(dp);
    hfs_dir_t *d = g_dirs[slot];
    struct lfs_info info;
    int prev = phase_begin("readdir", d->path);
    for (;;) {
        int r = lfs_dir_read(&g_lfs, &d->dir, &info);
        if (r <= 0) { phase_end(prev); return NULL; }
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
        break;
    }
    phase_end(prev);
    memset(&d->ent, 0, sizeof(d->ent));
    snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", info.name);
    d->ent.d_type = info.type == LFS_TYPE_DIR ? DT_DIR : DT_REG;
    return &d->ent;
}

int __real_closedir(DIR *dp);
int __wrap_closedir(DIR *dp) {
    int slot = dir_slot(dp);
    if (slot < 0) return __real_closedir(dp);
    hfs_dir_t *d = g_dirs[slot];
    lfs_dir_close(&g_lfs, &d->dir);
    g_dirs[slot] = NULL;
    free(d);
    return 0;
}

// esp_littlefs vfs_littlefs_truncate(): open, lfs_file_truncate, close.
int __real_truncate(const char *path, off_t len);
int __wrap_truncate(const char *path, off_t len) {
    const char *lp = lfs_path(path);
    if (lp == NULL) return __real_truncate(path, len);
    int prev = phase_begin("truncate", path);
    lfs_file_t f;
    int r = lfs_file_open(&g_lfs, &f, lp, LFS_O_RDWR);
    if (r == 0) {
        r = lfs_file_truncate(&g_lfs, &f, (lfs_off_t) len);
        int c = lfs_file_close(&g_lfs, &f);
        if (r == 0) r = c;
    }
    phase_end(prev);
    if (r < 0) { errno = lfs_to_errno(r); return -1; }
    return 0;
}

// ---------------------------------------------------------------------------
// Free space and fault injection
// ---------------------------------------------------------------------------

int64_t hfs_free_bytes(void) {
    bool was = hfs_counting;
    hfs_counting = false;
    lfs_ssize_t used = lfs_fs_size(&g_lfs);
    hfs_counting = was;
    if (used < 0) return -1;
    return ((int64_t) g_cfg.block_count - used) * g_cfg.block_size;
}

int64_t hfs_tree_bytes(const char *dir_path, const char *prefix) {
    // Sum of file sizes under dir_path whose name starts with prefix.
    const char *lp = lfs_path(dir_path);
    if (lp == NULL) return -1;
    bool was = hfs_counting;
    hfs_counting = false;
    lfs_dir_t d;
    int64_t total = 0;
    if (lfs_dir_open(&g_lfs, &d, lp) == 0) {
        struct lfs_info info;
        while (lfs_dir_read(&g_lfs, &d, &info) > 0) {
            if (info.type == LFS_TYPE_REG && strncmp(info.name, prefix, strlen(prefix)) == 0) {
                total += info.size;
            }
        }
        lfs_dir_close(&g_lfs, &d);
    }
    hfs_counting = was;
    return total;
}

int hfs_fail_progs_after = -1;

