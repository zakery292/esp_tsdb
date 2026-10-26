/**
 * @file tsdb_segment.c
 * @brief Segment files: naming, scanning, creation, block I/O, eviction.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "TSDB_SEG";

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

void tsdb_seg_path(const tsdb_t *db, uint32_t seq, char *out, size_t out_len) {
    snprintf(out, out_len, "%s.s%010lu", db->filepath, (unsigned long) seq);
}

// Split "/littlefs/solar.tsdb" into dir "/littlefs" and base "solar.tsdb".
static void split_path(const char *path, char *dir, size_t dir_len,
                       char *base, size_t base_len) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(dir, dir_len, ".");
        snprintf(base, base_len, "%s", path);
    } else {
        size_t n = (size_t)(slash - path);
        if (n == 0) n = 1;                      // root
        if (n >= dir_len) n = dir_len - 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
        snprintf(base, base_len, "%s", slash + 1);
    }
}

// ---------------------------------------------------------------------------
// Block I/O
// ---------------------------------------------------------------------------

esp_err_t tsdb_seg_read_block(FILE *f, uint32_t data_offset, uint32_t block, void *buf) {
    if (f == NULL || buf == NULL) return ESP_ERR_INVALID_ARG;
    fseek(f, data_offset + block * TSDB_BLOCK_SIZE, SEEK_SET);
    return fread(buf, TSDB_BLOCK_SIZE, 1, f) == 1 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tsdb_seg_write_block(FILE *f, uint32_t data_offset, uint32_t block, const void *buf) {
    if (f == NULL || buf == NULL) return ESP_ERR_INVALID_ARG;
    fseek(f, data_offset + block * TSDB_BLOCK_SIZE, SEEK_SET);
    errno = 0;
    size_t written = fwrite(buf, TSDB_BLOCK_SIZE, 1, f);
    int werr = errno;
    fflush(f);
    fsync(fileno(f));
    if (written != 1) {
        clearerr(f);
        if (werr == ENOSPC) {
            ESP_LOGW(TAG, "Block %lu write: filesystem full (ENOSPC)", (unsigned long) block);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGE(TAG, "Block %lu write failed (errno=%d)", (unsigned long) block, werr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

esp_err_t tsdb_seg_table_push(tsdb_t *db, const tsdb_seg_t *seg) {
    if (db->nsegs == db->segs_cap) {
        uint16_t ncap = db->segs_cap ? db->segs_cap * 2 : 8;
        tsdb_seg_t *n = realloc(db->segs, ncap * sizeof(tsdb_seg_t));
        if (n == NULL) return ESP_ERR_NO_MEM;
        db->segs = n;
        db->segs_cap = ncap;
    }
    // Keep ascending by seq (scan order is arbitrary; creation appends).
    uint16_t i = db->nsegs;
    while (i > 0 && db->segs[i - 1].seq > seg->seq) {
        db->segs[i] = db->segs[i - 1];
        i--;
    }
    db->segs[i] = *seg;
    db->nsegs++;
    db->total_physical += seg->records;
    if (seg->seq >= db->next_seq) db->next_seq = seg->seq + 1;
    return ESP_OK;
}

void tsdb_seg_table_pop_front(tsdb_t *db) {
    if (db->nsegs == 0) return;
    db->total_physical -= db->segs[0].records;
    memmove(&db->segs[0], &db->segs[1], (db->nsegs - 1) * sizeof(tsdb_seg_t));
    db->nsegs--;
}

// ---------------------------------------------------------------------------
// Tail validation: records in the last block, from the block content.
// ---------------------------------------------------------------------------

// Returns the number of valid records in `blk` (0 if the block is not ours).
// A record is valid if its timestamp is non-zero and not below the previous
// one; the count field is trusted only as an upper bound.
static uint16_t valid_records_in_block(const uint8_t *blk, uint16_t rpb) {
    if (TSDB_BLOCK_MAGIC(blk) != TSDB_BLOCK_MAGIC_VALUE) return 0;
    uint16_t count = TSDB_BLOCK_COUNT(blk);
    if (count > rpb) count = rpb;
    uint32_t prev = 0;
    uint16_t n = 0;
    for (; n < count; n++) {
        uint32_t ts = TSDB_BLOCK_TS(blk, n);
        if (ts == 0 || ts < prev) break;
        prev = ts;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

static bool parse_seq(const char *name, const char *base, uint32_t *seq) {
    size_t bl = strlen(base);
    if (strncmp(name, base, bl) != 0) return false;
    const char *p = name + bl;
    if (p[0] != '.' || p[1] != 's') return false;
    p += 2;
    if (*p == '\0') return false;
    unsigned long v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned)(*p - '0');
    }
    if (v == 0 || v > 0xFFFFFFFFul) return false;
    *seq = (uint32_t) v;
    return true;
}

// Open one segment file, validate, and fill *seg. Returns ESP_OK, or
// ESP_ERR_INVALID_RESPONSE when the file should be removed (empty/corrupt).
static esp_err_t inspect_segment(tsdb_t *db, uint32_t seq, tsdb_seg_t *seg, uint8_t *blk) {
    char path[160];
    tsdb_seg_path(db, seq, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_ERR_NOT_FOUND;

    tsdb_seg_header_t h;
    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != TSDB_SEG_MAGIC ||
        h.version != TSDB_SEG_VERSION || h.seq != seq ||
        h.num_params == 0 || h.num_params > TSDB_MAX_PARAMS ||
        h.records_per_block == 0 ||
        h.records_per_block != (TSDB_BLOCK_SIZE - 8) / (4 + 2u * h.num_params) ||
        h.data_offset < TSDB_SEG_HEADER_SIZE || h.data_offset > 16 * TSDB_BLOCK_SIZE ||
        h.blocks_cap == 0) {
        ESP_LOGW(TAG, "Segment %s: invalid header — removing", path);
        goto out;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < (long) h.data_offset) {
        ESP_LOGW(TAG, "Segment %s: truncated header — removing", path);
        goto out;
    }
    uint32_t blocks = (uint32_t)((size - h.data_offset) / TSDB_BLOCK_SIZE);

    // Walk back from the last block until one validates. On FAT the size can
    // advance before the data lands, so the tail is judged by content.
    uint32_t records = 0, last_ts = 0, first_ts = 0;
    while (blocks > 0) {
        if (tsdb_seg_read_block(f, h.data_offset, blocks - 1, blk) != ESP_OK) { blocks--; continue; }
        uint16_t n = valid_records_in_block(blk, h.records_per_block);
        if (n == 0) { blocks--; continue; }
        records = (blocks - 1) * h.records_per_block + n;
        last_ts = TSDB_BLOCK_TS(blk, n - 1);
        break;
    }
    if (records == 0) {
        if ((size - h.data_offset) / TSDB_BLOCK_SIZE > 0) {
            ESP_LOGW(TAG, "Segment %s: no valid records — removing", path);
            goto out;
        }
        // Header-only segment: created by a schema change (or a crash right
        // after creation). Valid and empty; its time bounds are inherited
        // from the segment before it via created_ts.
        first_ts = last_ts = h.created_ts;
    } else {
        // created_ts is the first record's timestamp, written with the
        // header, so block 0 need not be read here (the scan is boot-time
        // cost: one open + two reads per segment). Only the OLDEST segment's
        // first_ts has to be exact; tsdb_seg_scan refines it after sorting.
        first_ts = (blocks == 1) ? TSDB_BLOCK_TS(blk, 0) : h.created_ts;
    }

    memset(seg, 0, sizeof(*seg));
    seg->seq = seq;
    seg->records = records;
    seg->first_ts = first_ts;
    seg->last_ts = last_ts;
    seg->blocks_cap = h.blocks_cap;
    seg->data_offset = h.data_offset;
    seg->schema_gen = h.schema_gen;
    seg->lifetime_base = h.lifetime_base;
    seg->rpb = h.records_per_block;
    seg->np = h.num_params;
    ret = ESP_OK;
out:
    fclose(f);
    return ret;
}

esp_err_t tsdb_seg_scan(tsdb_t *db) {
    char dir[128], base[128];
    split_path(db->filepath, dir, sizeof(dir), base, sizeof(base));

    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGE(TAG, "opendir(%s) failed (errno=%d)", dir, errno);
        return ESP_FAIL;
    }

    uint8_t *blk = malloc(TSDB_BLOCK_SIZE);
    if (blk == NULL) { closedir(d); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t seq;
        if (!parse_seq(e->d_name, base, &seq)) continue;
        tsdb_seg_t seg;
        esp_err_t r = inspect_segment(db, seq, &seg, blk);
        if (r == ESP_OK) {
            ret = tsdb_seg_table_push(db, &seg);
            if (ret != ESP_OK) break;
        } else if (r == ESP_ERR_INVALID_RESPONSE) {
            char path[160];
            tsdb_seg_path(db, seq, path, sizeof(path));
            unlink(path);
            if (seq >= db->next_seq) db->next_seq = seq + 1;   // never reuse a seq
        }
    }
    closedir(d);
    free(blk);

    // An empty segment is only meaningful as the newest one.
    for (uint16_t i = 0; ret == ESP_OK && db->nsegs > 0 && i + 1 < db->nsegs; ) {
        if (db->segs[i].seq != 0 && db->segs[i].records == 0) {
            char path[160];
            tsdb_seg_path(db, db->segs[i].seq, path, sizeof(path));
            unlink(path);
            memmove(&db->segs[i], &db->segs[i + 1], (db->nsegs - i - 1) * sizeof(tsdb_seg_t));
            db->nsegs--;
        } else {
            i++;
        }
    }

    // Exact first timestamp for the oldest segment (its created_ts may be a
    // predecessor's last_ts when it was created by a schema change).
    if (ret == ESP_OK && db->nsegs > 0 && db->segs[0].seq != 0 && db->segs[0].records > 0) {
        tsdb_seg_t *o = &db->segs[0];
        char path[160];
        tsdb_seg_path(db, o->seq, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        uint8_t *b = malloc(TSDB_BLOCK_SIZE);
        if (f && b && tsdb_seg_read_block(f, o->data_offset, 0, b) == ESP_OK &&
            TSDB_BLOCK_MAGIC(b) == TSDB_BLOCK_MAGIC_VALUE) {
            o->first_ts = TSDB_BLOCK_TS(b, 0);
        }
        if (f) fclose(f);
        free(b);
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Schema read / column mapping
// ---------------------------------------------------------------------------

esp_err_t tsdb_seg_read_schema(tsdb_t *db, const tsdb_seg_t *seg, FILE *f,
                               char names[][TSDB_NAME_LEN], char aliases[][TSDB_NAME_LEN],
                               uint8_t *base_params) {
    bool own = false;
    if (f == NULL) {
        char path[160];
        tsdb_seg_path(db, seg->seq, path, sizeof(path));
        f = fopen(path, "rb");
        if (f == NULL) return ESP_FAIL;
        own = true;
    }
    esp_err_t ret = ESP_OK;
    tsdb_seg_header_t h;
    fseek(f, 0, SEEK_SET);
    if (fread(&h, sizeof(h), 1, f) != 1) { ret = ESP_FAIL; goto out; }
    if (base_params) *base_params = h.base_params;
    if (names) {
        fseek(f, TSDB_SEG_HEADER_SIZE, SEEK_SET);
        if (fread(names, TSDB_NAME_LEN, h.num_params, f) != h.num_params) { ret = ESP_FAIL; goto out; }
    }
    if (aliases) {
        fseek(f, TSDB_SEG_HEADER_SIZE + (long) h.num_params * TSDB_NAME_LEN, SEEK_SET);
        if (fread(aliases, TSDB_NAME_LEN, h.num_params, f) != h.num_params) { ret = ESP_FAIL; goto out; }
    }
out:
    if (own) fclose(f);
    return ret;
}

void tsdb_map_columns(const tsdb_schema_t *cur, uint8_t seg_np,
                      char seg_names[][TSDB_NAME_LEN], int8_t *map) {
    // A segment with no names at all (config passed NULL names) maps by position.
    bool positional = (seg_names == NULL);
    if (!positional) {
        positional = true;
        for (uint8_t j = 0; j < seg_np; j++) {
            if (seg_names[j][0] != '\0') { positional = false; break; }
        }
    }
    for (uint8_t c = 0; c < cur->count; c++) {
        map[c] = -1;
        if (positional || cur->names[c][0] == '\0') {
            if (c < seg_np) map[c] = (int8_t) c;
            continue;
        }
        for (uint8_t j = 0; j < seg_np; j++) {
            if (strncmp(seg_names[j], cur->names[c], TSDB_NAME_LEN - 1) == 0 ||
                (cur->aliases[c][0] != '\0' &&
                 strncmp(seg_names[j], cur->aliases[c], TSDB_NAME_LEN - 1) == 0)) {
                map[c] = (int8_t) j;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

esp_err_t tsdb_seg_create(tsdb_t *db, uint32_t first_ts) {
    const tsdb_schema_t *s = &db->schema;
    uint8_t np = s->count;
    uint16_t rpb = (TSDB_BLOCK_SIZE - 8) / (4 + 2u * np);
    uint32_t prefix = TSDB_SEG_HEADER_SIZE + 2u * np * TSDB_NAME_LEN;
    uint32_t data_offset = (prefix + TSDB_BLOCK_SIZE - 1) / TSDB_BLOCK_SIZE * TSDB_BLOCK_SIZE;

    tsdb_seg_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = TSDB_SEG_MAGIC;
    h.version = TSDB_SEG_VERSION;
    h.seq = db->next_seq;
    h.num_params = np;
    h.base_params = s->base_count;
    h.records_per_block = rpb;
    h.blocks_cap = db->seg_blocks;
    h.data_offset = data_offset;
    h.schema_gen = s->gen;
    h.lifetime_base = tsdb_total_writes(db);
    h.created_ts = first_ts;

    char path[160];
    tsdb_seg_path(db, h.seq, path, sizeof(path));
    errno = 0;
    FILE *f = fopen(path, "w+b");
    if (f == NULL) {
        int e = errno;
        ESP_LOGE(TAG, "Create %s failed (errno=%d)", path, e);
        return e == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    // Header + names + aliases + zero padding up to data_offset, one block
    // of I/O. Reuse the pool's read buffer as scratch when it is contiguous.
    uint8_t *heapbuf = NULL;
    uint8_t *buf = tsdb_get_buffer_ptr(&db->pool, db->read_buffer_offset, TSDB_BLOCK_SIZE);
    if (buf == NULL) {
        heapbuf = malloc(TSDB_BLOCK_SIZE);   // never a 1 KB stack buffer
        if (heapbuf == NULL) { fclose(f); unlink(path); return ESP_ERR_NO_MEM; }
        buf = heapbuf;
    }

    bool ok = true;
    uint32_t written = 0;
    while (written < data_offset && ok) {
        uint32_t chunk = data_offset - written;
        if (chunk > TSDB_BLOCK_SIZE) chunk = TSDB_BLOCK_SIZE;
        memset(buf, 0, chunk);
        // Lay the prefix bytes that fall in this chunk.
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t off = written + i;
            if (off < TSDB_SEG_HEADER_SIZE) {
                buf[i] = ((const uint8_t *) &h)[off];
            } else if (off < TSDB_SEG_HEADER_SIZE + (uint32_t) np * TSDB_NAME_LEN) {
                uint32_t o = off - TSDB_SEG_HEADER_SIZE;
                buf[i] = (uint8_t) s->names[o / TSDB_NAME_LEN][o % TSDB_NAME_LEN];
            } else if (off < prefix) {
                uint32_t o = off - TSDB_SEG_HEADER_SIZE - (uint32_t) np * TSDB_NAME_LEN;
                buf[i] = (uint8_t) s->aliases[o / TSDB_NAME_LEN][o % TSDB_NAME_LEN];
            }
        }
        errno = 0;
        ok = fwrite(buf, chunk, 1, f) == 1;
        written += chunk;
    }
    int werr = errno;
    free(heapbuf);
    fflush(f);
    fsync(fileno(f));
    if (!ok) {
        fclose(f);
        unlink(path);
        ESP_LOGE(TAG, "Create %s: header write failed (errno=%d)", path, werr);
        return werr == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    tsdb_seg_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.seq = h.seq;
    seg.records = 0;
    seg.first_ts = first_ts;
    seg.last_ts = first_ts;
    seg.blocks_cap = h.blocks_cap;
    seg.data_offset = data_offset;
    seg.schema_gen = h.schema_gen;
    seg.lifetime_base = h.lifetime_base;
    seg.rpb = rpb;
    seg.np = np;
    esp_err_t ret = tsdb_seg_table_push(db, &seg);
    if (ret != ESP_OK) {
        fclose(f);
        unlink(path);
        return ret;
    }

    if (db->active) fclose(db->active);
    db->active = f;
    tsdb_write_cache_invalidate(db);
    ESP_LOGI(TAG, "Segment %lu created: %u columns, %u rec/block, %lu blocks",
             (unsigned long) seg.seq, np, rpb, (unsigned long) seg.blocks_cap);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

bool tsdb_evict_oldest(tsdb_t *db) {
    if (db->nsegs == 0) return false;
    tsdb_seg_t *o = &db->segs[0];
    if (o->seq != 0 && db->nsegs == 1) {
        return false;                       // the newest data segment is never unlinked
    }
    if (o->seq == 0) {
        if (db->legacy) {
            char path[128];
            strncpy(path, db->legacy->path, sizeof(path));
            tsdb_legacy_close(db->legacy);
            db->legacy = NULL;
            tsdb_legacy_remove(path);
            ESP_LOGI(TAG, "Evicted legacy database file (%lu records)", (unsigned long) o->records);
        }
    } else {
        char path[160];
        tsdb_seg_path(db, o->seq, path, sizeof(path));
        unlink(path);
        ESP_LOGI(TAG, "Evicted segment %lu (%lu records)",
                 (unsigned long) o->seq, (unsigned long) o->records);
    }
    tsdb_seg_table_pop_front(db);
    if (db->head_skip > db->total_physical) db->head_skip = db->total_physical;
    return true;
}

void tsdb_apply_capacity(tsdb_t *db) {
    if (db->max_records == 0) {
        db->head_skip = 0;
        return;
    }
    for (;;) {
        db->head_skip = (db->total_physical > db->max_records) ?
                        (db->total_physical - db->max_records) : 0;
        if (db->nsegs == 0 || db->head_skip < db->segs[0].records) break;
        if (!tsdb_evict_oldest(db)) {
            // Only the active segment left and it exceeds the capacity
            // (capacity shrunk below one segment): keep skipping logically.
            break;
        }
    }
}

void tsdb_remove_all_files(tsdb_t *db) {
    if (db->active) { fclose(db->active); db->active = NULL; }
    tsdb_write_cache_invalidate(db);
    while (db->nsegs > 0) {
        tsdb_seg_t *o = &db->segs[0];
        if (o->seq == 0) {
            if (db->legacy) { tsdb_legacy_close(db->legacy); db->legacy = NULL; }
            tsdb_legacy_remove(db->filepath);
        } else {
            char path[160];
            tsdb_seg_path(db, o->seq, path, sizeof(path));
            unlink(path);
        }
        tsdb_seg_table_pop_front(db);
    }
    db->total_physical = 0;
    db->head_skip = 0;
}

// ---------------------------------------------------------------------------
// Path-level operations (no handle)
// ---------------------------------------------------------------------------

// First segment file for `base` in `dir`, or false. Callers that unlink the
// result and rescan avoid mutating a directory while iterating it (littlefs
// does not promise a stable readdir across unlink).
static bool find_one_segment(const char *dir, const char *base, char *out, size_t out_len) {
    DIR *d = opendir(dir);
    if (d == NULL) return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t seq;
        if (parse_seq(e->d_name, base, &seq)) {
            int n = snprintf(out, out_len, "%s/%s", dir, e->d_name);
            if (n < 0 || (size_t) n >= out_len) continue;   // name does not fit: skip it
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

// Scratch for the path-level helpers: on the heap, because they are called
// before any handle exists, typically from a small boot task.
typedef struct { char dir[128]; char base[128]; char p[300]; } path_scratch_t;

esp_err_t tsdb_remove(const char *filepath) {
    if (filepath == NULL) return ESP_ERR_INVALID_ARG;
    path_scratch_t *s = malloc(sizeof(*s));
    if (s == NULL) return ESP_ERR_NO_MEM;
    split_path(filepath, s->dir, sizeof(s->dir), s->base, sizeof(s->base));
    tsdb_legacy_remove(filepath);
    // Bounded: every iteration removes one file or stops.
    for (int guard = 0; guard < 4096 && find_one_segment(s->dir, s->base, s->p, sizeof(s->p)); guard++) {
        if (unlink(s->p) != 0) {
            ESP_LOGW(TAG, "unlink(%s) failed (errno=%d)", s->p, errno);
            break;
        }
    }
    free(s);
    return ESP_OK;
}

esp_err_t tsdb_disk_usage(const char *filepath, uint64_t *bytes) {
    if (filepath == NULL || bytes == NULL) return ESP_ERR_INVALID_ARG;
    path_scratch_t *s = malloc(sizeof(*s));
    if (s == NULL) return ESP_ERR_NO_MEM;
    split_path(filepath, s->dir, sizeof(s->dir), s->base, sizeof(s->base));
    uint64_t total = 0;
    struct stat st;
    static const char *const legacy_suffix[] = { "", ".h0", ".h1", ".d/h0", ".d/h1", ".mig" };
    for (size_t i = 0; i < sizeof(legacy_suffix) / sizeof(legacy_suffix[0]); i++) {
        snprintf(s->p, sizeof(s->p), "%s%s", filepath, legacy_suffix[i]);
        if (stat(s->p, &st) == 0) total += (uint64_t) st.st_size;
    }
    DIR *d = opendir(s->dir);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            uint32_t seq;
            if (!parse_seq(e->d_name, s->base, &seq)) continue;
            int n = snprintf(s->p, sizeof(s->p), "%s/%s", s->dir, e->d_name);
            if (n < 0 || (size_t) n >= sizeof(s->p)) continue;
            if (stat(s->p, &st) == 0) total += (uint64_t) st.st_size;
        }
        closedir(d);
    }
    free(s);
    *bytes = total;
    return ESP_OK;
}
