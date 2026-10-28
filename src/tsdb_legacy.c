/**
 * @file tsdb_legacy.c
 * @brief Read-only adoption of pre-3.0 single-file databases.
 *
 * A 1.x/2.x database is one file: 584-byte header, sparse index, 1 KB
 * columnar blocks arranged as a ring, optionally a sequential "overflow"
 * region for runtime extras, plus (2.4+) sidecar header files that may be
 * ahead of the in-file header after an unclean shutdown.
 *
 * 3.0 never writes into such a file. It is opened read-only, the pre-3.0
 * recovery steps are applied (sidecar adoption incl. 2.4.4's <path>.d/ slots,
 * header reconstruction on corruption, orphaned .mig adoption, V2 block
 * layout) and the result is exposed by RANK: rank r (0 = oldest retained
 * record) maps to a ring slot, a block and an offset. tsdb_convert.c then
 * moves the records into segments, shrinking the file from its end.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "TSDB_LEGACY";

// ---------------------------------------------------------------------------
// Header validation / I/O
// ---------------------------------------------------------------------------

static bool header_is_sane(const tsdb_header_t *h) {
    if (h == NULL) return false;
    if (h->num_params == 0 || h->num_params > TSDB_MAX_PARAMS) return false;
    if (h->num_params > 16 && h->version < TSDB_VERSION_WIDE) return false;
    if (h->base_params != 0 && h->base_params > h->num_params) return false;
    if (h->param_size != 0 && h->param_size != 2) return false;
    uint16_t expected_rs = 4 + (h->num_params * 2);
    if (h->record_size != 0 && h->record_size != expected_rs) return false;
    uint16_t max_rpb = (TSDB_BLOCK_SIZE - 8) / expected_rs;
    if (h->records_per_block == 0 || h->records_per_block > max_rpb) return false;
    if (h->index_offset < 512 || h->index_offset > 0x10000) return false;
    if (h->index_entries > 0x100000) return false;
    if (h->index_stride == 0) return false;
    if (h->max_records > 100000000UL) return false;
    // total_records is a LIFETIME counter; the ring cursors are what is bounded.
    if (h->max_records > 0) {
        if (h->oldest_record_idx >= h->max_records) return false;
        if (h->newest_record_idx >= h->max_records) return false;
    }
    if (h->extra_param_count > 0) {
        if (h->extra_param_count > 48) return false;
        if (h->overflow_record_size == 0 || h->overflow_record_size > TSDB_BLOCK_SIZE) return false;
    }
    return true;
}

static esp_err_t read_header(FILE *file, tsdb_header_t *header) {
    fseek(file, 0, SEEK_SET);
    return fread(header, sizeof(tsdb_header_t), 1, file) == 1 ? ESP_OK : ESP_FAIL;
}


static uint32_t data_offset_of(const tsdb_header_t *h) {
    return h->index_offset + h->index_entries * 8u;
}

// ---------------------------------------------------------------------------
// Sidecar headers
//
// 2.4.0-2.4.3 wrote the whole header to one of two alternating slots,
// <path>.h0 / <path>.h1 = {"TSH1", seq, crc32, header}, on every record.
// 2.4.4 writes a compact slot (the mutable header fields only) to
// <path>.d/h0 / <path>.d/h1 = {"TSH2", seq, crc32, legacy_seq, ...} on every
// record, and the TSH1 slots only as a mirror every 32 records — so after an
// unclean shutdown under 2.4.4 the TSH1 slots can be up to 31 records behind,
// and the TSH2 slots hold the real state. The selection below is 2.4.4's
// tsdb_sidecar_load(), read-only.
// ---------------------------------------------------------------------------

#define TSDB_SIDECAR_MAGIC  0x54534831u  /* "TSH1" */
#define TSDB_SIDECAR2_MAGIC 0x54534832u  /* "TSH2" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t header_crc;
    tsdb_header_t header;
} tsdb_sidecar_t;

#define TSDB_HDR_PRE_LEN  (offsetof(tsdb_header_t, param_names))
#define TSDB_HDR_POST_OFF (offsetof(tsdb_header_t, base_params))
#define TSDB_HDR_POST_LEN (sizeof(tsdb_header_t) - TSDB_HDR_POST_OFF)

typedef struct {
    uint32_t magic;       // TSDB_SIDECAR2_MAGIC
    uint32_t seq;         // shared sequence with the TSH1 mirror slots
    uint32_t crc;         // crc32 over legacy_seq..end
    uint32_t legacy_seq;  // seq of the last TSH1 mirror its writer made, 0 = none
    uint8_t  pre[TSDB_HDR_PRE_LEN];
    uint8_t  post[TSDB_HDR_POST_LEN];
} __attribute__((packed)) tsdb_sidecar2_t;

static uint32_t crc32_(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *) data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1)));
        }
    }
    return ~crc;
}

// Existence by stat(): an fopen() can also fail for lack of memory, and
// "absent" decides which header is adopted and what gets deleted.
static int path_state(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

// The authoritative sidecar state, starting from `base` (the in-file header:
// TSH2 slots do not carry param_names). Heap, not stack, for the slots: open
// runs on whatever task the caller has (a 4.6 KB main task on the dongle).
static esp_err_t sidecar_load(const char *filepath, const tsdb_header_t *base, tsdb_header_t *out) {
    typedef struct { tsdb_sidecar_t s1; tsdb_sidecar2_t s2, best2; char path[160]; } scratch_t;
    scratch_t *sc = malloc(sizeof(*sc));
    if (sc == NULL) return ESP_ERR_NO_MEM;

    bool found_r = false, found_d = false;
    uint32_t seq_r = 0, seq_d = 0;
    for (uint8_t slot = 0; slot < 2; slot++) {
        snprintf(sc->path, sizeof(sc->path), "%s.h%u", filepath, (unsigned) slot);
        int st = path_state(sc->path);
        if (st == 0) continue;
        FILE *f = st > 0 ? fopen(sc->path, "rb") : NULL;
        if (f == NULL) {
            // Present but unreadable now: deciding without it could pick a
            // stale slot. Report; the caller retries the adoption later.
            esp_err_t e = errno == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
            free(sc);
            return e;
        }
        size_t n = fread(&sc->s1, sizeof(sc->s1), 1, f);
        fclose(f);
        if (n != 1 || sc->s1.magic != TSDB_SIDECAR_MAGIC) continue;
        if (crc32_(&sc->s1.header, sizeof(sc->s1.header)) != sc->s1.header_crc) {
            ESP_LOGW(TAG, "sidecar slot %u CRC mismatch — ignoring", (unsigned) slot);
            continue;
        }
        if (!found_r || sc->s1.seq > seq_r) {
            found_r = true;
            seq_r = sc->s1.seq;
            *out = sc->s1.header;               // provisional; TSH2 may win below
        }
    }
    for (uint8_t slot = 0; slot < 2; slot++) {
        snprintf(sc->path, sizeof(sc->path), "%s.d/h%u", filepath, (unsigned) slot);
        int st = path_state(sc->path);
        if (st == 0) continue;
        FILE *f = st > 0 ? fopen(sc->path, "rb") : NULL;
        if (f == NULL) {
            // Present but unreadable now: deciding without it could pick a
            // stale slot. Report; the caller retries the adoption later.
            esp_err_t e = errno == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
            free(sc);
            return e;
        }
        size_t n = fread(&sc->s2, sizeof(sc->s2), 1, f);
        fclose(f);
        if (n != 1 || sc->s2.magic != TSDB_SIDECAR2_MAGIC) continue;
        if (crc32_(&sc->s2.legacy_seq, sizeof(sc->s2) - offsetof(tsdb_sidecar2_t, legacy_seq)) != sc->s2.crc) {
            ESP_LOGW(TAG, "sidecar slot d%u CRC mismatch — ignoring", (unsigned) slot);
            continue;
        }
        if (!found_d || sc->s2.seq > seq_d) {
            found_d = true;
            seq_d = sc->s2.seq;
            sc->best2 = sc->s2;
        }
    }

    // TSH2 is authoritative unless a TSH1 slot is newer than the last mirror
    // its writer knew of (an older version wrote after a downgrade). A TSH2
    // slot that references a mirror which is gone means an older version
    // closed the database cleanly: the in-file header is newer than both.
    bool use_d = false;
    if (found_d) {
        use_d = found_r ? (seq_r <= sc->best2.legacy_seq) : (sc->best2.legacy_seq == 0);
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (use_d) {
        *out = *base;
        memcpy(out, sc->best2.pre, TSDB_HDR_PRE_LEN);
        memcpy((uint8_t *) out + TSDB_HDR_POST_OFF, sc->best2.post, TSDB_HDR_POST_LEN);
        ret = ESP_OK;
    } else if (found_r) {
        ret = ESP_OK;
    }
    free(sc);
    return ret;
}

static void sidecar_remove(const char *filepath) {
    char path[160];
    // Subdirectory first (2.4.4's order): if this is interrupted, what is
    // left is a TSH1 slot, which the next open handles as before.
    for (uint8_t slot = 0; slot < 2; slot++) {
        snprintf(path, sizeof(path), "%s.d/h%u", filepath, (unsigned) slot);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s.d", filepath);
    rmdir(path);
    for (uint8_t slot = 0; slot < 2; slot++) {
        snprintf(path, sizeof(path), "%s.h%u", filepath, (unsigned) slot);
        unlink(path);
    }
}

// ---------------------------------------------------------------------------
// Orphaned .mig (2.2–2.5 schema migration crash window)
// ---------------------------------------------------------------------------

static esp_err_t migrate_recover(const char *filepath) {
    char mig_path[140];
    snprintf(mig_path, sizeof(mig_path), "%s.mig", filepath);
    int ms = path_state(mig_path);
    if (ms == 0) return ESP_OK;
    if (ms < 0) return ESP_FAIL;

    int main_state = path_state(filepath);
    if (main_state < 0) return ESP_FAIL;
    if (main_state == 1) {
        unlink(mig_path);
        ESP_LOGW(TAG, "Removed stale migration file %s", mig_path);
        return ESP_OK;
    }
    tsdb_header_t *hdr = malloc(sizeof(*hdr));
    if (hdr == NULL) return ESP_ERR_NO_MEM;
    FILE *mig = fopen(mig_path, "rb");
    if (mig == NULL) { free(hdr); return ESP_FAIL; }
    bool valid = (fread(hdr, sizeof(*hdr), 1, mig) == 1) && (hdr->magic == TSDB_MAGIC);
    free(hdr);
    fclose(mig);
    if (valid) {
        if (rename(mig_path, filepath) == 0) {
            ESP_LOGW(TAG, "Adopted completed migration file for %s", filepath);
        } else {
            ESP_LOGE(TAG, "Failed to adopt %s (errno=%d)", mig_path, errno);
        }
    } else {
        unlink(mig_path);
        ESP_LOGW(TAG, "Removed invalid orphaned migration file %s", mig_path);
    }
    return ESP_OK;
}


// ---------------------------------------------------------------------------
// Header reconstruction from data blocks (corrupt header)
//
// Scanning every block of a full 2.5 MB file costs ~7 s on an ESP32 (host
// cost model), more than the task watchdog allows, so tsdb_legacy_open only
// prepares the geometry (a few probe reads) and the conversion steps scan a
// bounded number of blocks each (tsdb_legacy_scan_step). The result is
// persisted in the conversion marker before anything else happens to the
// file, so it is computed once.
// ---------------------------------------------------------------------------

static void prepare_reconstruction(tsdb_legacy_t *l, const tsdb_config_t *config) {
    ESP_LOGW(TAG, "%s: header unreadable — reconstructing it from the data blocks (in steps)", l->path);
    tsdb_header_t *header = &l->header;
    memset(header, 0, sizeof(tsdb_header_t));
    l->reconstructed = true;
    header->magic = TSDB_MAGIC;
    header->version = config->num_params > 16 ? TSDB_VERSION_WIDE : TSDB_VERSION;
    header->num_params = config->num_params;
    header->base_params = config->num_params;
    header->param_size = sizeof(int16_t);
    header->record_size = 4 + (config->num_params * 2);
    header->records_per_block = (TSDB_BLOCK_SIZE - 8) / header->record_size;
    header->max_records = config->max_records;
    header->index_stride = 380;
    header->index_offset = 512;

    // Probe plausible index sizes / bases for a block magic at the data start.
    uint32_t cfg_entries = config->max_records > 0 ? (config->max_records / 380) + 1 : 256;
    uint32_t candidates[] = { cfg_entries, 256, 128, 138, 165, 200, 512, 1024, 2048 };
    const uint32_t bases[] = { 512, 1024, TSDB_V4_INDEX_OFFSET };
    uint32_t probed = 0;
    for (size_t b = 0; b < 3 && probed == 0; b++) {
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            uint32_t off = bases[b] + candidates[i] * 8u;
            if (off + TSDB_BLOCK_SIZE > l->length) continue;
            fseek(l->file, off, SEEK_SET);
            uint32_t magic = 0;
            if (fread(&magic, sizeof(magic), 1, l->file) == 1 && magic == TSDB_BLOCK_MAGIC_VALUE) {
                probed = candidates[i];
                header->index_offset = bases[b];
                break;
            }
        }
    }
    header->index_entries = probed > 0 ? probed : cfg_entries;

    if (config->param_names) {
        for (int i = 0; i < config->num_params && i < 16; i++) {
            strncpy(header->param_names[i], config->param_names[i], 31);
            header->param_names[i][31] = '\0';
        }
    }

    uint32_t data_offset = data_offset_of(header);
    tsdb_conv_t *c = &l->conv;
    c->scan_next = 0;
    c->scan_blocks = l->length > data_offset ? (l->length - data_offset) / TSDB_BLOCK_SIZE : 0;
    c->scan_oldest_ts = UINT32_MAX;
    c->scan_newest_ts = 0;
    c->scan_oldest_idx = c->scan_newest_idx = c->scan_total = 0;
    l->needs_scan = true;
}

bool tsdb_legacy_scan_step(tsdb_legacy_t *l, uint32_t max_blocks, void *buf) {
    tsdb_conv_t *c = &l->conv;
    tsdb_header_t *header = &l->header;
    uint32_t data_offset = data_offset_of(header);
    uint32_t end = c->scan_next + max_blocks;
    if (end > c->scan_blocks) end = c->scan_blocks;
    uint8_t *block = buf;
    // One seek, then sequential reads: a seek in a large littlefs file walks
    // its skip-list, a read that continues does not.
    if (c->scan_next < end) fseek(l->file, data_offset + c->scan_next * TSDB_BLOCK_SIZE, SEEK_SET);
    for (uint32_t b = c->scan_next; b < end; b++) {
        if (fread(block, TSDB_BLOCK_SIZE, 1, l->file) != 1) {
            fseek(l->file, data_offset + (b + 1) * TSDB_BLOCK_SIZE, SEEK_SET);
            continue;
        }
        if (TSDB_BLOCK_MAGIC(block) != TSDB_BLOCK_MAGIC_VALUE || TSDB_BLOCK_COUNT(block) == 0) continue;
        for (uint16_t i = 0; i < TSDB_BLOCK_COUNT(block) && i < header->records_per_block; i++) {
            uint32_t ts = TSDB_BLOCK_TS(block, i);
            if (ts == 0) continue;
            uint32_t idx = b * header->records_per_block + i;
            if (ts < c->scan_oldest_ts) { c->scan_oldest_ts = ts; c->scan_oldest_idx = idx; }
            if (ts > c->scan_newest_ts) { c->scan_newest_ts = ts; c->scan_newest_idx = idx; }
            c->scan_total++;
        }
    }
    c->scan_next = end;
    if (c->scan_next < c->scan_blocks) return false;

    // Done: the header the 2.x reconstruction would have produced.
    l->needs_scan = false;
    if (c->scan_total == 0) {
        ESP_LOGW(TAG, "Reconstruction: no valid records — empty database");
        header->total_records = header->total_writes = 0;
    } else {
        header->oldest_record_idx = c->scan_oldest_idx;
        header->newest_record_idx = c->scan_newest_idx;
        header->oldest_timestamp = c->scan_oldest_ts;
        header->newest_timestamp = c->scan_newest_ts;
        if (header->max_records > 0 && c->scan_total >= header->max_records) {
            uint32_t next_slot = (c->scan_newest_idx + 1) % header->max_records;
            header->total_records = header->max_records + next_slot;
            header->oldest_record_idx = next_slot;
            header->total_evictions = next_slot;
        } else {
            header->total_records = c->scan_total;
            header->total_evictions = 0;
        }
        header->total_writes = header->total_records;
        ESP_LOGW(TAG, "Header reconstructed: %lu records (%lu to %lu)", (unsigned long) c->scan_total,
                 (unsigned long) c->scan_oldest_ts, (unsigned long) c->scan_newest_ts);
    }
    l->unlimited = (header->max_records == 0);
    l->available = l->unlimited ? header->total_records :
                   (header->total_records < header->max_records ? header->total_records : header->max_records);
    l->first_slot = l->unlimited ? 0 : header->oldest_record_idx;
    return true;
}

// ---------------------------------------------------------------------------
// Conversion marker: <path>.cv holds the header as adopted (after sidecar
// selection or reconstruction) and the overflow column names. Written once,
// before the first cut of the file; from then on it is the only header
// trusted, because the file shrinks and its sidecars are removed.
// ---------------------------------------------------------------------------

#define TSDB_CONV_MARKER_MAGIC 0x31564354u   /* "TCV1" */
#define MARKER_RECONSTRUCTED 0x01             // names from the config, as the reconstruction took them
#define MARKER_NAMES         0x02             // base names follow (they differ from the in-file header's)

// Small enough to be stored inline in its directory entry on littlefs
// (< 512 bytes) in the usual case: the column names are the in-file
// header's, which is never cut; only the fields that change are kept.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc;                                    // crc32 over flags..end (variable)
    uint8_t  flags;
    uint8_t  extra_count;
    uint8_t  pre[TSDB_HDR_PRE_LEN];
    uint8_t  post[TSDB_HDR_POST_LEN];
    // then: [16][32] base names if MARKER_NAMES; extra_count x 20 overflow names
} __attribute__((packed)) tsdb_conv_marker_t;

#define MARKER_CRC_OFF offsetof(tsdb_conv_marker_t, flags)
#define MARKER_MAX (sizeof(tsdb_conv_marker_t) + 16 * 32 + TSDB_MAX_EXTRA_PARAMS * TSDB_NAME_LEN)

static void marker_path(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s.cv", path);
}

// ESP_OK: adopted from the marker; ESP_ERR_NOT_FOUND: no (valid) marker.
static esp_err_t read_marker(tsdb_legacy_t *l, const tsdb_config_t *config) {
    char p[160];
    marker_path(l->path, p, sizeof(p));
    int st = path_state(p);
    if (st == 0) return ESP_ERR_NOT_FOUND;
    uint8_t *buf = malloc(MARKER_MAX);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    FILE *f = st > 0 ? fopen(p, "rb") : NULL;
    if (f == NULL) {
        esp_err_t e = errno == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
        free(buf);
        return e;
    }
    size_t len = fread(buf, 1, MARKER_MAX, f);
    fclose(f);
    tsdb_conv_marker_t *m = (tsdb_conv_marker_t *) buf;
    size_t need = sizeof(*m);
    bool ok = len >= need && m->magic == TSDB_CONV_MARKER_MAGIC && m->version == 1 &&
              m->extra_count <= TSDB_MAX_EXTRA_PARAMS;
    if (ok) {
        need += ((m->flags & MARKER_NAMES) ? 16 * 32 : 0) + (size_t) m->extra_count * TSDB_NAME_LEN;
        ok = len == need && crc32_(buf + MARKER_CRC_OFF, len - MARKER_CRC_OFF) == m->crc;
    }
    tsdb_header_t h;
    if (ok) {
        // Names: the in-file header's (never cut), the config's for a
        // reconstructed header, or stored here.
        memset(&h, 0, sizeof(h));
        if (m->flags & MARKER_RECONSTRUCTED) {
            if (config && config->param_names) {
                for (int i = 0; i < config->num_params && i < 16; i++) {
                    strncpy(h.param_names[i], config->param_names[i], 31);
                }
            }
        } else if (read_header(l->file, &h) != ESP_OK) {
            memset(&h, 0, sizeof(h));
        }
        if (m->flags & MARKER_NAMES) memcpy(h.param_names, buf + sizeof(*m), 16 * 32);
        memcpy(&h, m->pre, TSDB_HDR_PRE_LEN);
        memcpy((uint8_t *) &h + TSDB_HDR_POST_OFF, m->post, TSDB_HDR_POST_LEN);
        ok = header_is_sane(&h);
    }
    if (!ok) {
        // Torn while being written (it is synced before the file is first
        // cut, so the file is still whole): ignored, rewritten by the
        // conversion's first step.
        ESP_LOGW(TAG, "Conversion marker %s invalid — ignored", p);
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }
    l->header = h;
    if (m->extra_count > 0) {
        l->extra_names = calloc(m->extra_count, TSDB_NAME_LEN);
        if (l->extra_names == NULL) { free(buf); return ESP_ERR_NO_MEM; }
        memcpy(l->extra_names, buf + need - (size_t) m->extra_count * TSDB_NAME_LEN,
               (size_t) m->extra_count * TSDB_NAME_LEN);
    }
    l->marker = true;
    free(buf);
    return ESP_OK;
}

esp_err_t tsdb_legacy_write_marker(tsdb_legacy_t *l) {
    uint8_t *buf = calloc(1, MARKER_MAX);
    tsdb_header_t *infile = malloc(sizeof(*infile));
    if (buf == NULL || infile == NULL) { free(buf); free(infile); return ESP_ERR_NO_MEM; }
    tsdb_conv_marker_t *m = (tsdb_conv_marker_t *) buf;
    m->magic = TSDB_CONV_MARKER_MAGIC;
    m->version = 1;
    m->flags = l->reconstructed ? MARKER_RECONSTRUCTED : 0;
    m->extra_count = l->extra_count;
    memcpy(m->pre, &l->header, TSDB_HDR_PRE_LEN);
    memcpy(m->post, (const uint8_t *) &l->header + TSDB_HDR_POST_OFF, TSDB_HDR_POST_LEN);
    size_t len = sizeof(*m);
    if (!l->reconstructed &&
        (read_header(l->file, infile) != ESP_OK ||
         memcmp(infile->param_names, l->header.param_names, sizeof(infile->param_names)) != 0)) {
        m->flags |= MARKER_NAMES;
        memcpy(buf + len, l->header.param_names, 16 * 32);
        len += 16 * 32;
    }
    free(infile);
    if (l->extra_count > 0 && l->extra_names) {
        memcpy(buf + len, l->extra_names, (size_t) l->extra_count * TSDB_NAME_LEN);
    }
    len += (size_t) l->extra_count * TSDB_NAME_LEN;
    m->crc = crc32_(buf + MARKER_CRC_OFF, len - MARKER_CRC_OFF);
    char p[160];
    marker_path(l->path, p, sizeof(p));
    errno = 0;
    FILE *f = fopen(p, "wb");
    esp_err_t ret = ESP_OK;
    if (f == NULL) {
        ret = errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    } else {
        int err = 0;
        if (fwrite(buf, len, 1, f) != 1) err = errno ? errno : EIO;
        if (fflush(f) != 0 && err == 0) err = errno ? errno : EIO;
        if (fsync(fileno(f)) != 0 && err == 0) err = errno ? errno : EIO;
        if (fclose(f) != 0 && err == 0) err = errno ? errno : EIO;
        if (err != 0) {
            unlink(p);
            ret = err == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }
    free(buf);
    if (ret == ESP_OK) l->marker = true;
    return ret;
}

// ---------------------------------------------------------------------------
// Open / close / remove
// ---------------------------------------------------------------------------

void tsdb_legacy_remove_sidecars(const char *path) {
    sidecar_remove(path);
}

bool tsdb_legacy_remove_one_sidecar(const char *path) {
    // sidecar_remove()'s order: the 2.4.4 subdirectory first.
    static const char *const sfx[] = { ".d/h0", ".d/h1", ".d", ".h0", ".h1" };
    char p[160];
    for (size_t i = 0; i < sizeof(sfx) / sizeof(sfx[0]); i++) {
        snprintf(p, sizeof(p), "%s%s", path, sfx[i]);
        if (path_state(p) != 1) continue;
        if (i == 2) rmdir(p); else unlink(p);
        return true;
    }
    return false;
}

void tsdb_legacy_remove(const char *path) {
    char p[160];
    // Order matters for a power cut part-way. The sidecars go before the
    // main file, so none can outlive it (nothing would ever remove them:
    // ~16-20 KB for good). But a main file whose sidecars are gone would be
    // adopted with its in-file header, up to 31 records (2.4.4) or a whole
    // session (2.4.3) stale, and out of order on a wrapped ring — so it is
    // emptied first: one commit, and the next open discards a file too short
    // to hold a header. The conversion marker goes last: while it exists,
    // the next open takes the (possibly emptied) file as fully converted.
    if (truncate(path, 0) != 0) { /* absent: nothing to neutralise */ }
    sidecar_remove(path);
    snprintf(p, sizeof(p), "%s.mig", path);
    unlink(p);
    unlink(path);
    marker_path(path, p, sizeof(p));
    unlink(p);
}

void tsdb_legacy_close(tsdb_legacy_t *l) {
    if (l == NULL) return;
    if (l->file) fclose(l->file);
    if (l->conv.out) fclose(l->conv.out);
    free(l->extra_names);
    free(l->v2_tmp);
    free(l);
}

static void read_overflow_names(tsdb_legacy_t *l) {
    if (l->extra_count == 0 || l->extra_names != NULL) return;
    l->extra_names = calloc(l->extra_count, TSDB_NAME_LEN);
    if (l->extra_names == NULL) return;
    for (uint8_t i = 0; i < l->extra_count; i++) {
        fseek(l->file, l->header.overflow_offset + offsetof(tsdb_overflow_header_t, param_names) + i * 20u,
              SEEK_SET);
        if (fread(l->extra_names[i], 20, 1, l->file) != 1) memset(l->extra_names[i], 0, TSDB_NAME_LEN);
        l->extra_names[i][TSDB_NAME_LEN - 1] = '\0';
    }
}

esp_err_t tsdb_legacy_open(const char *path, const tsdb_config_t *config, tsdb_legacy_t **out) {
    *out = NULL;
    esp_err_t mr0 = migrate_recover(path);
    if (mr0 != ESP_OK) return mr0;

    // Read-only: 3.0 only ever shrinks the adopted file from its end (see
    // tsdb_convert.c); a writable open would be a metadata commit on
    // esp_littlefs at every boot.
    int present = path_state(path);
    if (present == 0) {
        // Sidecars without a main file are orphans of a removal cut short by
        // a power loss (pre-3.0 order: main file first); a marker without
        // one is what a finished conversion leaves if cut before its last
        // unlink. Nothing reads them; remove them (no-ops, without a commit,
        // when they are absent).
        sidecar_remove(path);
        char p[160];
        marker_path(path, p, sizeof(p));
        if (path_state(p) == 1) unlink(p);
        return ESP_ERR_NOT_FOUND;
    }
    FILE *f = present < 0 ? NULL : fopen(path, "rb");
    if (f == NULL) {
        // Present (or unknown) but not openable now: out of memory, too many
        // open files. Nothing is decided on it; the caller retries.
        return errno == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    tsdb_legacy_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    l->file = f;
    strncpy(l->path, path, sizeof(l->path) - 1);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    l->length = size > 0 ? (uint32_t) size : 0;

    // 1. A conversion in progress: its marker holds the adopted header.
    esp_err_t mk = read_marker(l, config);
    if (mk != ESP_OK && mk != ESP_ERR_NOT_FOUND) {
        tsdb_legacy_close(l);
        return mk;
    }

    if (mk != ESP_OK) {
        bool needs_reconstruction = false;
        if (read_header(f, &l->header) != ESP_OK || l->header.magic != TSDB_MAGIC) {
            ESP_LOGW(TAG, "Legacy file %s: bad/unreadable header", path);
            needs_reconstruction = true;
        } else if (l->header.version == 99) {
            ESP_LOGE(TAG, "Legacy file %s: torn V2->V3 migration — discarding", path);
            tsdb_legacy_close(l);
            tsdb_legacy_remove(path);
            return ESP_ERR_INVALID_RESPONSE;
        } else if (!header_is_sane(&l->header)) {
            ESP_LOGW(TAG, "Legacy file %s: header fields implausible — reconstructing", path);
            needs_reconstruction = true;
        }

        if (!needs_reconstruction) {
            // A sidecar that is valid, geometrically identical and ahead of
            // the in-file header holds the records an unclean shutdown would
            // lose. Out of memory here must not fall back to the in-file
            // header: for a 2.4.x database closed by a power cut that is
            // stale (records lost, a wrapped ring read out of order, an
            // empty-looking file deleted).
            tsdb_header_t *sc = malloc(sizeof(*sc));
            esp_err_t sr = sc == NULL ? ESP_ERR_NO_MEM : sidecar_load(path, &l->header, sc);
            if (sr != ESP_OK && sr != ESP_ERR_NOT_FOUND) {
                free(sc);
                tsdb_legacy_close(l);
                return sr;
            }
            if (sr == ESP_OK) {
                if (sc->magic == TSDB_MAGIC && header_is_sane(sc) &&
                    sc->num_params == l->header.num_params &&
                    sc->records_per_block == l->header.records_per_block &&
                    sc->index_offset == l->header.index_offset &&
                    sc->total_records >= l->header.total_records) {
                    ESP_LOGI(TAG, "Legacy sidecar ahead of in-file header (%lu -> %lu records)",
                             (unsigned long) l->header.total_records, (unsigned long) sc->total_records);
                    l->header = *sc;
                }
            }
            free(sc);
        } else if (l->length < 512) {
            ESP_LOGE(TAG, "Legacy file %s too small to reconstruct — discarding", path);
            tsdb_legacy_close(l);
            tsdb_legacy_remove(path);
            return ESP_ERR_INVALID_RESPONSE;
        } else {
            prepare_reconstruction(l, config);
        }
    }

    if (l->header.base_params == 0) {
        l->header.base_params = l->header.num_params;
    }
    // Pre-2.0.1 (V2) block layout: the column offsets of the old struct.
    // 2.x rewrote every block in place at open; 3.0 transcodes each block as
    // it is read (identical result, nothing written).
    l->v2_layout = l->header.version < 3 && l->header.records_per_block > 38;
    l->data_offset = data_offset_of(&l->header);
    if (l->v2_layout) {
        l->v2_tmp = malloc(TSDB_BLOCK_SIZE);
        if (l->v2_tmp == NULL) {
            tsdb_legacy_close(l);
            return ESP_ERR_NO_MEM;
        }
    }

    // Overflow extras
    if (l->header.extra_param_count > 0 && l->header.overflow_offset > 0) {
        l->extra_count = l->header.extra_param_count;
        l->overflow_record_size = l->header.overflow_record_size;
        l->first_overflow_record_idx = l->header.first_overflow_record_idx;
        l->overflow_data_offset = l->header.overflow_offset + TSDB_OVERFLOW_HEADER_SIZE;
        read_overflow_names(l);
        if (l->extra_names == NULL) {
            tsdb_legacy_close(l);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!l->needs_scan) {
        l->unlimited = (l->header.max_records == 0);
        l->available = l->unlimited ? l->header.total_records :
                       (l->header.total_records < l->header.max_records ?
                        l->header.total_records : l->header.max_records);
        l->first_slot = l->unlimited ? 0 : l->header.oldest_record_idx;
    }

    ESP_LOGI(TAG, "Adopted legacy database %s: %lu records, %u columns (+%u extras)%s",
             path, (unsigned long) l->available, l->header.num_params, l->extra_count,
             l->marker ? " (conversion in progress)" : "");
    *out = l;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Read-side accessors (by rank: 0 = oldest retained record)
// ---------------------------------------------------------------------------

uint8_t tsdb_legacy_columns(const tsdb_legacy_t *l) {
    return l->header.num_params + l->extra_count;
}

void tsdb_legacy_column_name(tsdb_legacy_t *l, uint8_t idx, char *out) {
    memset(out, 0, TSDB_NAME_LEN);
    if (idx < l->header.num_params) {
        if (idx < 16) {
            strncpy(out, l->header.param_names[idx], TSDB_NAME_LEN - 1);
        } else {
            // V4 names 16..63 live in the header region, which is never cut.
            fseek(l->file, TSDB_V4_EXT_NAMES_OFFSET + (uint32_t)(idx - 16) * TSDB_V4_EXT_NAME_LEN, SEEK_SET);
            if (fread(out, TSDB_V4_EXT_NAME_LEN, 1, l->file) != 1) memset(out, 0, TSDB_NAME_LEN);
            out[TSDB_NAME_LEN - 1] = '\0';
        }
        return;
    }
    uint8_t extra = idx - l->header.num_params;
    if (extra >= l->extra_count || l->extra_names == NULL) return;
    memcpy(out, l->extra_names[extra], TSDB_NAME_LEN);
    out[TSDB_NAME_LEN - 1] = '\0';
}

static inline uint32_t slot_of(const tsdb_legacy_t *l, uint32_t r) {
    return l->unlimited ? r : ((l->first_slot + r) % l->header.max_records);
}

void tsdb_legacy_locate(const tsdb_legacy_t *l, uint32_t r, uint32_t *block, uint16_t *off) {
    uint32_t slot = slot_of(l, r);
    *block = slot / l->header.records_per_block;
    *off = (uint16_t)(slot % l->header.records_per_block);
}

esp_err_t tsdb_legacy_read_block(tsdb_legacy_t *l, uint32_t block, void *buf) {
    if (l->file == NULL) return ESP_FAIL;
    uint32_t off = l->data_offset + block * TSDB_BLOCK_SIZE;
    if (off + TSDB_BLOCK_SIZE > l->length) return ESP_ERR_NOT_FOUND;
    fseek(l->file, off, SEEK_SET);
    if (fread(buf, TSDB_BLOCK_SIZE, 1, l->file) != 1) return ESP_ERR_NOT_FOUND;
    if (!l->v2_layout) return ESP_OK;

    // V2 -> V3 transcode, exactly what 2.x's migrate_v2_blocks wrote back:
    // a zeroed block, the count, timestamps at the same offsets, params from
    // the old struct offsets (which overlap the new layout: read from a copy).
    uint8_t *b = buf, *old = l->v2_tmp;
    if (TSDB_BLOCK_MAGIC(b) != TSDB_BLOCK_MAGIC_VALUE || TSDB_BLOCK_COUNT(b) == 0) return ESP_OK;
    uint16_t rpb = l->header.records_per_block;
    uint8_t np = l->header.num_params;
    uint16_t count = TSDB_BLOCK_COUNT(b);
    memcpy(old, b, TSDB_BLOCK_SIZE);
    memset(b, 0, TSDB_BLOCK_SIZE);
    TSDB_BLOCK_MAGIC(b) = TSDB_BLOCK_MAGIC_VALUE;
    TSDB_BLOCK_COUNT(b) = count;
    for (uint16_t r = 0; r < count && r < rpb; r++) {
        TSDB_BLOCK_TS(b, r) = *(uint32_t *)(old + 8 + r * 4);
        for (uint8_t p = 0; p < np; p++) {
            uint32_t o = 160 + p * 76 + r * 2;
            TSDB_BLOCK_PARAM(b, rpb, p, r) = (o + 2 <= TSDB_BLOCK_SIZE) ? *(int16_t *)(old + o) : 0;
        }
    }
    return ESP_OK;
}

uint32_t tsdb_legacy_run(const tsdb_legacy_t *l, uint32_t r) {
    uint32_t slot = slot_of(l, r);
    uint32_t run = l->header.records_per_block - (slot % l->header.records_per_block);
    if (!l->unlimited) {
        uint32_t to_ring_end = l->header.max_records - slot;
        if (run > to_ring_end) run = to_ring_end;
    }
    uint32_t left = l->available - r;
    return run < left ? run : left;
}

void tsdb_legacy_read_extras_n(tsdb_legacy_t *l, uint32_t r, uint32_t n, int16_t *out) {
    memset(out, 0, (size_t) n * l->extra_count * sizeof(int16_t));
    if (l->extra_count == 0 || l->file == NULL) return;
    uint32_t abs_idx = (l->header.total_records - l->available) + r;
    uint32_t skip = 0;
    if (abs_idx < l->first_overflow_record_idx) {
        skip = l->first_overflow_record_idx - abs_idx;   // before the region existed: zeros
        if (skip >= n) return;
    }
    uint32_t off = l->overflow_data_offset +
                   (abs_idx + skip - l->first_overflow_record_idx) * l->overflow_record_size;
    fseek(l->file, off, SEEK_SET);
    size_t want = (size_t)(n - skip) * l->extra_count;
    if (fread(out + (size_t) skip * l->extra_count, sizeof(int16_t), want, l->file) != want) {
        memset(out, 0, (size_t) n * l->extra_count * sizeof(int16_t));
    }
}

void tsdb_legacy_read_extras(tsdb_legacy_t *l, uint32_t r, int16_t *out) {
    tsdb_legacy_read_extras_n(l, r, 1, out);
}

uint32_t tsdb_legacy_ts_at(tsdb_legacy_t *l, uint32_t r, void *buf) {
    uint32_t block; uint16_t off;
    tsdb_legacy_locate(l, r, &block, &off);
    if (tsdb_legacy_read_block(l, block, buf) != ESP_OK) return 0;
    return TSDB_BLOCK_TS(buf, off);
}

uint8_t tsdb_legacy_present(const tsdb_legacy_t *l, uint32_t len, uint32_t iv[2][2]) {
    uint32_t avail = l->available;
    if (avail == 0 || l->needs_scan) return 0;
    uint32_t rpb = l->header.records_per_block;
    // (1) the record's ring block lies wholly below len
    uint64_t blocks = len > l->data_offset ? (len - l->data_offset) / TSDB_BLOCK_SIZE : 0;
    uint64_t slots = l->unlimited ? avail : l->header.max_records;
    uint64_t cut = blocks * rpb;
    if (cut > slots) cut = slots;
    // (2) its overflow extras, if it has any, lie wholly below len
    uint64_t rhi = avail;
    if (l->extra_count > 0) {
        uint64_t n_present = len > l->overflow_data_offset ?
                             (len - l->overflow_data_offset) / l->overflow_record_size : 0;
        int64_t lim = (int64_t) l->first_overflow_record_idx + (int64_t) n_present -
                      (int64_t) (l->header.total_records - avail);
        rhi = lim < 0 ? 0 : (lim > avail ? avail : (uint64_t) lim);
    }
    uint64_t a[2], b[2];
    uint8_t n = 0;
    if (l->unlimited) {
        a[0] = 0; b[0] = cut; n = 1;
    } else {
        uint64_t S = l->header.max_records, f = l->first_slot;
        // ranks [0, S-f) sit at slots f.., ranks [S-f, S) at slots 0..
        a[0] = 0;     b[0] = cut > f ? cut - f : 0;
        if (b[0] > S - f) b[0] = S - f;
        a[1] = S - f; b[1] = S - f + (cut < f ? cut : f);
        n = 2;
    }
    uint8_t out = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint64_t lo = a[i], hi = b[i];
        if (hi > avail) hi = avail;
        if (hi > rhi) hi = rhi;
        if (lo >= hi) continue;
        iv[out][0] = (uint32_t) lo;
        iv[out][1] = (uint32_t) hi;
        out++;
    }
    return out;
}

uint32_t tsdb_legacy_lifetime_end(const tsdb_legacy_t *l) {
    uint32_t w = l->header.total_writes;
    return w >= l->available ? w : l->available;
}

long tsdb_legacy_file_size(const tsdb_legacy_t *l) {
    return (long) l->length;
}
