/**
 * @file tsdb_convert.c
 * @brief Moving an adopted pre-3.0 file into segments, in bounded steps.
 *
 * A full 2.x database is one 2.5 MB file. It cannot be deleted piece by piece
 * from the front (where its oldest records may be), but littlefs truncates a
 * file from its END cheaply — one metadata commit, measured at 1.6 ms for an
 * 83 KB cut of a 2.5 MB file (host_test/test_lfs_cost --truncate-probe). So
 * each round:
 *
 *   PLAN   pick the cut: the file's last ~one segment of blocks;
 *          the records to keep are those wholly below it
 *          (tsdb_legacy_present), the ones to copy are the live records
 *          above it — at most two rank intervals (a wrapped ring's tail
 *          block can hold its newest and its oldest records);
 *   COPY   write each interval to <path>.c<rank>, a few blocks per step,
 *          committed once, when it is complete;
 *   TRUNC  cut the file; the copied ranks now live only in the new files.
 *
 * until nothing live is left in it (FINAL: the file, its sidecars and the
 * marker are removed). Extra space at any time: about one segment.
 *
 * ORDER. A wrapped ring's tail is not its newest data, so converted segments
 * are created out of time order. They are named and sorted by legacy RANK
 * (0 = the file's oldest retained record), and everything from the file
 * sorts before every 3.0 segment (tsdb_seg_order). The part still in the
 * file appears in the table as LEGACY entries — the rank intervals it still
 * holds — interleaved with the converted segments by rank.
 *
 * CRASH SAFETY. Before the first cut, START writes <path>.cv with the header
 * as adopted (after sidecar selection, or reconstruction) and only then
 * SIDECARS removes the sidecars: the file's own header can be stale and the
 * reconstruction depends on blocks that will be cut, so from then on the
 * marker is the header. Ranks are stable because that header never changes.
 * At open, what the file holds is computed from its physical length
 * (tsdb_legacy_present) LESS every rank a converted segment already has: a
 * segment completed before its cut holds the same records the file does, so
 * they are read from the segment and never twice, and the next PLAN copies
 * only what is still missing. A segment cut short by a power cut was never
 * synced: littlefs leaves it empty and the scan discards it (on a filesystem
 * that keeps a prefix, the prefix is a valid segment and the same rule
 * applies). Nothing is ever lost: the file is cut only after the copies are
 * synced and closed.
 *
 * PACING. Nothing heavy runs in tsdb_open. Each tsdb_maintain_h() call (and
 * each write, one step after it, outside its lock hold) does ONE bounded unit
 * of work under the handle lock: TSDB_CONV_STEP_BLOCKS converted blocks, one
 * truncate, the marker, or TSDB_CONV_SCAN_BLOCKS blocks of a header
 * reconstruction.
 *
 * CAPACITY. Converted records count toward max_records like any others; the
 * oldest converted segments are evicted one by one. Records still in the
 * file that the window passes are dropped from the table and skipped by the
 * copy.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "TSDB_CONV";

// Converted data blocks written per step: one 4 KB flash block.
#ifndef TSDB_CONV_STEP_BLOCKS
#define TSDB_CONV_STEP_BLOCKS 4
#endif
// Blocks scanned per header-reconstruction step.
#ifndef TSDB_CONV_SCAN_BLOCKS
#define TSDB_CONV_SCAN_BLOCKS 96
#endif

// ---------------------------------------------------------------------------
// Table helpers
// ---------------------------------------------------------------------------

static void remove_legacy_entries(tsdb_t *db) {
    uint16_t w = 0;
    for (uint16_t i = 0; i < db->nsegs; i++) {
        if (db->segs[i].kind == TSDB_SEG_LEGACY) {
            db->total_physical -= db->segs[i].records;
            continue;
        }
        db->segs[w++] = db->segs[i];
    }
    db->nsegs = w;
    if (db->head_skip > db->total_physical) db->head_skip = db->total_physical;
}

static esp_err_t table_reserve(tsdb_t *db, uint16_t extra) {
    uint32_t need = (uint32_t) db->nsegs + extra;
    if (need <= db->segs_cap) return ESP_OK;
    uint16_t ncap = db->segs_cap ? db->segs_cap : 8;
    while (ncap < need) ncap *= 2;
    tsdb_seg_t *n = realloc(db->segs, ncap * sizeof(tsdb_seg_t));
    if (n == NULL) return ESP_ERR_NO_MEM;
    db->segs = n;
    db->segs_cap = ncap;
    return ESP_OK;
}

static void piece_fill(tsdb_t *db, tsdb_seg_t *e, uint32_t a, uint32_t b, void *buf) {
    tsdb_legacy_t *l = db->legacy;
    memset(e, 0, sizeof(*e));
    e->kind = TSDB_SEG_LEGACY;
    e->rank = a;
    e->records = b - a;
    e->first_ts = tsdb_legacy_ts_at(l, a, buf);
    e->last_ts = tsdb_legacy_ts_at(l, b - 1, buf);
    e->rpb = l->header.records_per_block;
    e->np = tsdb_legacy_columns(l);
    e->lifetime_base = tsdb_legacy_lifetime_end(l) - l->available + a;
}

// [a, b) minus every CONV entry of the table -> out (appended, up to max).
static uint8_t minus_converted(const tsdb_t *db, uint32_t a, uint32_t b, uint32_t out[][2], uint8_t n,
                               uint8_t max) {
    uint32_t cur = a;
    for (uint16_t i = 0; i < db->nsegs && cur < b; i++) {
        const tsdb_seg_t *c = &db->segs[i];
        if (c->kind != TSDB_SEG_CONV) continue;
        uint32_t ca = c->rank, cb = c->rank + c->records;
        if (cb <= cur || ca >= b) continue;
        if (ca > cur && n < max) { out[n][0] = cur; out[n][1] = ca; n++; }
        if (cb > cur) cur = cb;
    }
    if (cur < b && n < max) { out[n][0] = cur; out[n][1] = b; n++; }
    return n;
}

esp_err_t tsdb_conv_adopt(tsdb_t *db) {
    tsdb_legacy_t *l = db->legacy;
    uint32_t pres[2][2];
    uint8_t np = tsdb_legacy_present(l, l->length, pres);
    uint32_t pieces[TSDB_CONV_MAX_IV][2];
    uint8_t n = 0;
    for (uint8_t i = 0; i < np; i++) n = minus_converted(db, pres[i][0], pres[i][1], pieces, n, TSDB_CONV_MAX_IV);
    if (n == 0) return ESP_OK;
    if (table_reserve(db, n) != ESP_OK) return ESP_ERR_NO_MEM;
    void *heap;
    void *buf = tsdb_scratch_block(db, &heap);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    for (uint8_t i = 0; i < n; i++) {
        tsdb_seg_t e;
        piece_fill(db, &e, pieces[i][0], pieces[i][1], buf);
        tsdb_seg_table_push(db, &e);            // reserved: cannot fail
    }
    free(heap);
    ESP_LOGI(TAG, "2.x file %s: %u live interval(s) still in it (%lu bytes)", l->path, n, (unsigned long) l->length);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Abort / drop
// ---------------------------------------------------------------------------

void tsdb_conv_abort_chunk(tsdb_t *db) {
    tsdb_legacy_t *l = db->legacy;
    if (l == NULL) return;
    tsdb_conv_t *c = &l->conv;
    char path[160];
    if (c->out) {
        fclose(c->out);
        c->out = NULL;
        tsdb_conv_path(db, c->iv[c->cur][0], path, sizeof(path));
        unlink(path);
    }
    // Completed copies of this round are not in the table yet: as files they
    // would come back at the next open, so they go too.
    for (uint8_t i = 0; i < c->cur && i < c->niv; i++) {
        tsdb_conv_path(db, c->iv[i][0], path, sizeof(path));
        unlink(path);
    }
    c->niv = c->cur = 0;
    c->written = 0;
}

void tsdb_conv_drop_legacy(tsdb_t *db) {
    tsdb_legacy_t *l = db->legacy;
    if (l == NULL) return;
    tsdb_conv_abort_chunk(db);
    remove_legacy_entries(db);
    char path[128];
    strncpy(path, l->path, sizeof(path));
    tsdb_legacy_close(l);
    db->legacy = NULL;
    tsdb_legacy_remove(path);
}

// ---------------------------------------------------------------------------
// PLAN
// ---------------------------------------------------------------------------

static uint32_t out_rpb(const tsdb_legacy_t *l) {
    return (TSDB_BLOCK_SIZE - 8) / (4 + 2u * tsdb_legacy_columns(l));
}

// The length the file is cut to next.
static uint32_t plan_target(const tsdb_t *db, const tsdb_legacy_t *l) {
    uint32_t L = l->length;
    uint32_t chunk_records = db->seg_blocks * out_rpb(l);
    if (l->extra_count > 0 && L > l->overflow_data_offset) {
        // Overflow region (extras of the newest records, in record order).
        uint32_t rs = l->overflow_record_size;
        uint32_t n = (L - l->overflow_data_offset) / rs;
        uint32_t aligned = l->overflow_data_offset + n * rs;
        if (aligned < L) return aligned;
        uint32_t keep = n > chunk_records ? n - chunk_records : 0;
        return l->overflow_data_offset + keep * rs;
    }
    if (l->extra_count > 0 && L > l->header.overflow_offset && l->header.overflow_offset > l->data_offset) {
        return l->header.overflow_offset;                 // the overflow header
    }
    if (L <= l->data_offset) return l->data_offset;
    uint32_t rpb = l->header.records_per_block;
    uint32_t slots = l->unlimited ? l->available : l->header.max_records;
    uint32_t ring_end = l->data_offset + ((slots + rpb - 1) / rpb) * TSDB_BLOCK_SIZE;
    if (L > ring_end) return ring_end;                     // nothing but slack past the ring
    uint32_t nb = (L - l->data_offset + TSDB_BLOCK_SIZE - 1) / TSDB_BLOCK_SIZE;
    uint32_t chunk_blocks = chunk_records / rpb;
    if (chunk_blocks == 0) chunk_blocks = 1;
    uint32_t keep = nb > chunk_blocks ? nb - chunk_blocks : 0;
    return l->data_offset + keep * TSDB_BLOCK_SIZE;
}

static void plan(tsdb_t *db) {
    tsdb_legacy_t *l = db->legacy;
    tsdb_conv_t *c = &l->conv;

    // Live: the LEGACY entries, less the head the window has passed.
    uint32_t live[TSDB_CONV_MAX_IV][2];
    uint8_t nl = 0;
    for (uint16_t i = 0; i < db->nsegs && nl < TSDB_CONV_MAX_IV; i++) {
        const tsdb_seg_t *e = &db->segs[i];
        if (e->kind != TSDB_SEG_LEGACY) continue;
        uint32_t skip = (i == 0) ? db->head_skip : 0;
        if (skip >= e->records) continue;
        live[nl][0] = e->rank + skip;
        live[nl][1] = e->rank + e->records;
        nl++;
    }
    if (nl == 0) {
        c->phase = TSDB_CONV_FINAL;
        return;
    }

    c->target = plan_target(db, l);
    c->final_cut = c->target <= l->data_offset;
    uint32_t keep[2][2];
    uint8_t nk = c->final_cut ? 0 : tsdb_legacy_present(l, c->target, keep);

    // To copy: live minus what the cut file keeps.
    c->niv = c->cur = 0;
    c->written = 0;
    for (uint8_t i = 0; i < nl; i++) {
        uint32_t cur = live[i][0], end = live[i][1];
        for (uint8_t k = 0; k < nk && cur < end; k++) {
            if (keep[k][1] <= cur || keep[k][0] >= end) continue;
            if (keep[k][0] > cur) {
                if (c->niv == TSDB_CONV_MAX_IV) goto overflow;
                c->iv[c->niv][0] = cur; c->iv[c->niv][1] = keep[k][0]; c->niv++;
            }
            cur = keep[k][1];
        }
        if (cur < end) {
            if (c->niv == TSDB_CONV_MAX_IV) goto overflow;
            c->iv[c->niv][0] = cur; c->iv[c->niv][1] = end; c->niv++;
        }
    }
    c->phase = c->niv > 0 ? TSDB_CONV_COPY : TSDB_CONV_TRUNC;
    return;
overflow:
    // Cannot happen with the table's invariants (at most two intervals
    // present, split only by converted segments a power cut left).
    ESP_LOGE(TAG, "Conversion of %s too fragmented — stopped; the file stays readable", l->path);
    c->niv = 0;
    c->stalled = true;
}

// ---------------------------------------------------------------------------
// COPY
// ---------------------------------------------------------------------------

static esp_err_t copy_step(tsdb_t *db, uint8_t *lbuf) {
    tsdb_legacy_t *l = db->legacy;
    tsdb_conv_t *c = &l->conv;
    uint32_t a = c->iv[c->cur][0], n = c->iv[c->cur][1] - a;
    uint8_t np = tsdb_legacy_columns(l), base = l->header.num_params, ne = l->extra_count;
    uint16_t rpb = (uint16_t) out_rpb(l), lrpb = l->header.records_per_block;
    esp_err_t ret = ESP_OK;
    int err = 0;
    char path[160];

    void *oheap = NULL;
    uint8_t *obuf = tsdb_get_buffer_ptr(&db->pool, db->query_buffer_offset, TSDB_BLOCK_SIZE);
    if (obuf == NULL) obuf = oheap = malloc(TSDB_BLOCK_SIZE);
    int16_t *ext = ne ? malloc((size_t) rpb * ne * sizeof(int16_t)) : NULL;
    if (obuf == NULL || (ne && ext == NULL)) { ret = ESP_ERR_NO_MEM; goto out; }

    if (c->out == NULL) {
        // Create <path>.c<rank>: header (schema = the 2.x columns) + names.
        uint32_t prefix = TSDB_SEG_HEADER_SIZE + 2u * np * TSDB_NAME_LEN;
        uint32_t data_offset = (prefix + TSDB_BLOCK_SIZE - 1) / TSDB_BLOCK_SIZE * TSDB_BLOCK_SIZE;
        tsdb_seg_header_t h;
        memset(&h, 0, sizeof(h));
        h.magic = TSDB_SEG_MAGIC;
        h.version = TSDB_SEG_VERSION;
        h.seq = a;
        h.num_params = np;
        h.base_params = base;
        h.records_per_block = rpb;
        h.blocks_cap = (n + rpb - 1) / rpb;
        h.data_offset = data_offset;
        h.schema_gen = 0;
        h.lifetime_base = tsdb_legacy_lifetime_end(l) - l->available + a;
        h.created_ts = tsdb_legacy_ts_at(l, a, lbuf);
        tsdb_conv_path(db, a, path, sizeof(path));
        errno = 0;
        c->out = fopen(path, "w+b");
        if (c->out == NULL) { err = errno ? errno : EIO; ret = ESP_FAIL; goto out; }
        c->written = 0;
        c->out_rpb = rpb;
        c->out_data_offset = data_offset;
        c->done[c->cur].first_ts = h.created_ts;
        for (uint32_t w = 0; w < data_offset && ret == ESP_OK; w += TSDB_BLOCK_SIZE) {
            memset(obuf, 0, TSDB_BLOCK_SIZE);
            for (uint32_t i = 0; i < TSDB_BLOCK_SIZE && w + i < prefix; i++) {
                uint32_t off = w + i;
                if (off < TSDB_SEG_HEADER_SIZE) obuf[i] = ((const uint8_t *) &h)[off];
            }
            // names (aliases stay zero)
            for (uint8_t j = 0; j < np; j++) {
                char nm[TSDB_NAME_LEN];
                tsdb_legacy_column_name(l, j, nm);
                for (uint32_t k = 0; k < TSDB_NAME_LEN; k++) {
                    uint32_t off = TSDB_SEG_HEADER_SIZE + j * TSDB_NAME_LEN + k;
                    if (off >= w && off < w + TSDB_BLOCK_SIZE) obuf[off - w] = (uint8_t) nm[k];
                }
            }
            errno = 0;
            if (fwrite(obuf, TSDB_BLOCK_SIZE, 1, c->out) != 1) { err = errno ? errno : EIO; ret = ESP_FAIL; }
        }
        if (ret != ESP_OK) goto out;
    }

    // The step that writes the 1 KB header writes one data block less: each
    // step then fills about one 4 KB flash block (one erase).
    int budget = (c->written == 0) ? TSDB_CONV_STEP_BLOCKS - 1 : TSDB_CONV_STEP_BLOCKS;
    for (int k = 0; k < budget && c->written < n; k++) {
        uint32_t r0 = a + c->written;
        uint32_t m = n - c->written;
        if (m > rpb) m = rpb;
        memset(obuf, 0, TSDB_BLOCK_SIZE);
        TSDB_BLOCK_MAGIC(obuf) = TSDB_BLOCK_MAGIC_VALUE;
        TSDB_BLOCK_COUNT(obuf) = (uint16_t) m;
        if (ne) tsdb_legacy_read_extras_n(l, r0, m, ext);
        uint32_t cached = UINT32_MAX;
        for (uint32_t j = 0; j < m; j++) {
            uint32_t blk; uint16_t off;
            tsdb_legacy_locate(l, r0 + j, &blk, &off);
            if (blk != cached) {
                if (tsdb_legacy_read_block(l, blk, lbuf) != ESP_OK) { ret = ESP_FAIL; goto out; }
                cached = blk;
            }
            TSDB_BLOCK_TS(obuf, j) = TSDB_BLOCK_TS(lbuf, off);
            for (uint8_t p = 0; p < base; p++) {
                TSDB_BLOCK_PARAM(obuf, rpb, p, j) = TSDB_BLOCK_PARAM(lbuf, lrpb, p, off);
            }
            for (uint8_t e = 0; e < ne; e++) {
                TSDB_BLOCK_PARAM(obuf, rpb, base + e, j) = ext[j * ne + e];
            }
        }
        // Sequential appends, no seek: on littlefs a seek flushes a file
        // being written, and the next append then copies its last block.
        errno = 0;
        if (fwrite(obuf, TSDB_BLOCK_SIZE, 1, c->out) != 1) {
            err = errno ? errno : EIO;
            ret = ESP_FAIL;
            goto out;
        }
        c->written += m;
        c->done[c->cur].last_ts = TSDB_BLOCK_TS(obuf, m - 1);
    }
    // No sync between steps: littlefs would then copy the half-filled flash
    // block at the next append (an erase per step). Unsynced, a power cut
    // leaves an empty file, discarded at the next open; the 2.x file still
    // holds every record, since it is cut only after this file is closed.
    if (fflush(c->out) != 0) { err = errno ? errno : EIO; ret = ESP_FAIL; goto out; }

    if (c->written == n) {
        errno = 0;
        int cr = fsync(fileno(c->out));
        if (cr != 0) err = errno ? errno : EIO;
        if (fclose(c->out) != 0 && cr == 0) { cr = -1; err = errno ? errno : EIO; }
        c->out = NULL;
        if (cr != 0) { err = errno ? errno : EIO; ret = ESP_FAIL; tsdb_conv_path(db, a, path, sizeof(path)); unlink(path); goto out; }
        tsdb_seg_t *d = &c->done[c->cur];
        d->kind = TSDB_SEG_CONV;
        d->seq = d->rank = a;
        d->records = n;
        d->blocks_cap = (n + rpb - 1) / rpb;
        d->data_offset = c->out_data_offset;
        d->schema_gen = 0;
        d->lifetime_base = tsdb_legacy_lifetime_end(l) - l->available + a;
        d->rpb = rpb;
        d->np = np;
        c->cur++;
        c->written = 0;
        if (c->cur == c->niv) c->phase = TSDB_CONV_TRUNC;
    }

out:
    free(oheap);
    free(ext);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL && err == ENOSPC) ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Converting ranks %lu..: write failed (%d, errno %d) — retried later",
                 (unsigned long) a, ret, err);
        tsdb_conv_abort_chunk(db);
        c->phase = TSDB_CONV_PLAN;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// TRUNC / FINAL
// ---------------------------------------------------------------------------

static int cut_file(const char *path, uint32_t len) {
    if (truncate(path, (off_t) len) == 0) return 0;
    // Filesystems whose VFS has no path truncate: through a handle.
    FILE *w = fopen(path, "r+b");
    if (w == NULL) return -1;
    int r = ftruncate(fileno(w), (off_t) len);
    if (fclose(w) != 0) r = -1;
    return r;
}

static esp_err_t trunc_step(tsdb_t *db, uint8_t *buf) {
    tsdb_legacy_t *l = db->legacy;
    tsdb_conv_t *c = &l->conv;
    // Room for the converted entries and the clipped pieces, before the cut.
    if (table_reserve(db, (uint16_t) (c->niv + 4)) != ESP_OK) return ESP_ERR_NO_MEM;
    tsdb_seg_t *old = malloc((size_t) db->nsegs * sizeof(tsdb_seg_t) + 1);
    if (old == NULL) return ESP_ERR_NO_MEM;

    if (!c->final_cut) {
        if (l->file) { fclose(l->file); l->file = NULL; }
        int r = cut_file(l->path, c->target);
        l->file = fopen(l->path, "rb");
        if (r != 0) {
            ESP_LOGE(TAG, "Cannot truncate %s (errno %d) — conversion stopped; the file stays readable",
                     l->path, errno);
            free(old);
            tsdb_conv_abort_chunk(db);
            c->stalled = true;
            return ESP_FAIL;
        }
        l->length = c->target;
    }

    // The table: LEGACY entries clipped to what the file still holds, the
    // converted segments added. Everything stays in rank order.
    uint32_t keep[2][2];
    uint8_t nk = c->final_cut ? 0 : tsdb_legacy_present(l, l->length, keep);
    uint16_t n_old = db->nsegs;
    memcpy(old, db->segs, (size_t) n_old * sizeof(tsdb_seg_t));
    db->nsegs = 0;
    db->total_physical = 0;
    for (uint16_t i = 0; i < n_old; i++) {
        tsdb_seg_t *e = &old[i];
        if (e->kind != TSDB_SEG_LEGACY) { tsdb_seg_table_push(db, e); continue; }
        for (uint8_t k = 0; k < nk; k++) {
            uint32_t lo = e->rank > keep[k][0] ? e->rank : keep[k][0];
            uint32_t hi = e->rank + e->records < keep[k][1] ? e->rank + e->records : keep[k][1];
            if (lo >= hi) continue;
            tsdb_seg_t p = *e;
            if (lo != e->rank || hi != e->rank + e->records) piece_fill(db, &p, lo, hi, buf);
            tsdb_seg_table_push(db, &p);
        }
    }
    for (uint8_t i = 0; i < c->niv; i++) tsdb_seg_table_push(db, &c->done[i]);
    free(old);
    ESP_LOGI(TAG, "2.x file cut to %lu bytes: %u converted segment(s) added", (unsigned long) l->length, c->niv);
    c->niv = c->cur = 0;
    c->phase = c->final_cut ? TSDB_CONV_FINAL : TSDB_CONV_PLAN;
    tsdb_apply_capacity(db);
    return ESP_OK;
}

static esp_err_t final_step(tsdb_t *db) {
    ESP_LOGI(TAG, "2.x file %s fully converted — removing it", db->legacy->path);
    tsdb_conv_drop_legacy(db);
    tsdb_apply_capacity(db);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Retry an adoption that failed at open (out of memory)
// ---------------------------------------------------------------------------

static esp_err_t retry_adopt(tsdb_t *db) {
    const char *names[TSDB_MAX_PARAMS];
    for (uint8_t i = 0; i < db->schema.base_count; i++) names[i] = db->schema.names[i];
    tsdb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.filepath = db->filepath;
    cfg.num_params = db->schema.base_count;
    cfg.param_names = names;
    cfg.max_records = db->cfg_max_records;
    tsdb_legacy_t *l = NULL;
    esp_err_t r = tsdb_legacy_open(db->filepath, &cfg, &l);
    if (r == ESP_ERR_NOT_FOUND || r == ESP_ERR_INVALID_RESPONSE) {
        db->legacy_retry = false;
        return ESP_OK;
    }
    if (r != ESP_OK) return r;
    if (!l->needs_scan && l->available == 0 && !l->marker) {
        char path[128];
        strncpy(path, l->path, sizeof(path));
        tsdb_legacy_close(l);
        tsdb_legacy_remove(path);
        db->legacy_retry = false;
        return ESP_OK;
    }
    db->legacy = l;
    l->conv.phase = l->needs_scan ? TSDB_CONV_SCAN : TSDB_CONV_START;
    if (!l->needs_scan) {
        r = tsdb_conv_adopt(db);
        if (r != ESP_OK) {
            remove_legacy_entries(db);
            tsdb_legacy_close(l);
            db->legacy = NULL;
            return r;
        }
        tsdb_apply_capacity(db);
    }
    db->legacy_retry = false;
    ESP_LOGI(TAG, "Adopted the pre-3.0 database on retry");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Step
// ---------------------------------------------------------------------------

esp_err_t tsdb_conv_step(tsdb_t *db) {
    if (db->legacy == NULL) return db->legacy_retry ? retry_adopt(db) : ESP_OK;
    tsdb_legacy_t *l = db->legacy;
    tsdb_conv_t *c = &l->conv;
    if (c->stalled) return ESP_OK;
    if (l->file == NULL) {
        l->file = fopen(l->path, "rb");
        if (l->file == NULL) return ESP_FAIL;
    }
    void *heap;
    uint8_t *buf = tsdb_scratch_block(db, &heap);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_OK;

    switch (c->phase) {
    case TSDB_CONV_SCAN:
        if (tsdb_legacy_scan_step(l, TSDB_CONV_SCAN_BLOCKS, buf)) {
            if (l->available == 0) {
                c->phase = TSDB_CONV_FINAL;
            } else if ((ret = tsdb_conv_adopt(db)) == ESP_OK) {
                tsdb_apply_capacity(db);
                // Persist the reconstruction at once, so no later boot scans
                // again (if this fails, START retries it).
                if (tsdb_legacy_write_marker(l) != ESP_OK) {
                    ESP_LOGW(TAG, "Reconstructed header not persisted yet — retried");
                }
                c->phase = TSDB_CONV_START;
            } else {
                remove_legacy_entries(db);
                // Out of memory adopting what the scan found: scan again later.
                l->needs_scan = true;
                c->scan_next = 0;
                c->scan_oldest_ts = UINT32_MAX;
                c->scan_newest_ts = c->scan_oldest_idx = c->scan_newest_idx = c->scan_total = 0;
            }
        }
        break;
    case TSDB_CONV_START:
        // The header as adopted becomes the only one trusted, before the file
        // is first cut.
        if (!l->marker) ret = tsdb_legacy_write_marker(l);
        if (ret == ESP_OK) c->phase = TSDB_CONV_SIDECARS;
        break;
    case TSDB_CONV_SIDECARS:
        // From here on the sidecars are redundant. One per step: each unlink
        // is a metadata commit, and any commit can land on a compaction.
        if (!tsdb_legacy_remove_one_sidecar(l->path)) {
            c->phase = TSDB_CONV_PLAN;
            plan(db);
            if (!c->stalled && c->phase == TSDB_CONV_COPY) ret = copy_step(db, buf);
        }
        break;
    default:
        if (c->phase == TSDB_CONV_PLAN) plan(db);
        if (c->stalled) break;
        if (c->phase == TSDB_CONV_COPY) ret = copy_step(db, buf);
        else if (c->phase == TSDB_CONV_TRUNC) ret = trunc_step(db, buf);
        else if (c->phase == TSDB_CONV_FINAL) ret = final_step(db);
        break;
    }
    free(heap);
    return ret;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

esp_err_t tsdb_maintain_h(tsdb_t *db, bool *more) {
    if (more) *more = false;
    if (db == NULL || !db->is_open || db->closing) return ESP_ERR_INVALID_STATE;
    TSDB_LOCK_OPEN_OR_RETURN(db, 5000, ESP_ERR_TIMEOUT);
    if (tsdb_refuse_if_query_open(db, __func__)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = tsdb_conv_step(db);
    // A retry of a failed adoption is not reported as pending work: it is
    // attempted once per write, not in a loop.
    if (more) *more = db->legacy != NULL && !db->legacy->conv.stalled;
    tsdb_unlock(db);
    return ret;
}

esp_err_t tsdb_maintain(bool *more) {
    return tsdb_maintain_h(g_default_handle, more);
}
