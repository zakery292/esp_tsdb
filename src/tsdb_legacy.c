/**
 * @file tsdb_legacy.c
 * @brief Read-only adoption of pre-3.0 single-file databases.
 *
 * A 1.x/2.x database is one file: 584-byte header, sparse index, 1 KB
 * columnar blocks arranged as a ring, optionally a sequential "overflow"
 * region for runtime extras, plus (2.4+) sidecar header files that may be
 * ahead of the in-file header after an unclean shutdown.
 *
 * 3.0 never writes to such a file. It is opened once, the pre-3.0 recovery
 * steps are applied (sidecar adoption, header reconstruction on corruption,
 * V2->V3 block-layout migration, orphaned .mig adoption — all unchanged from
 * 2.5), and the result is exposed as the oldest segment: physical record k
 * (0 = oldest retained) maps to a ring slot, a block and an offset. When the
 * capacity window moves past its last record the file is unlinked.
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

static esp_err_t write_header(FILE *file, const tsdb_header_t *header) {
    fseek(file, 0, SEEK_SET);
    size_t written = fwrite(header, sizeof(tsdb_header_t), 1, file);
    fflush(file);
    fsync(fileno(file));
    return written == 1 ? ESP_OK : ESP_FAIL;
}

static uint32_t data_offset_of(const tsdb_header_t *h) {
    return h->index_offset + h->index_entries * 8u;
}

// ---------------------------------------------------------------------------
// Sidecar header (2.4+): <path>.h0 / <path>.h1 = {magic, seq, crc32, header}
// ---------------------------------------------------------------------------

#define TSDB_SIDECAR_MAGIC 0x54534831u  /* "TSH1" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t header_crc;
    tsdb_header_t header;
} tsdb_sidecar_t;

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

// Heap, not stack, for the ~600-byte sidecar: open runs on whatever task
// the caller has (a 4.6 KB main task on the dongle firmware).
static esp_err_t sidecar_load(const char *filepath, tsdb_header_t *out) {
    tsdb_sidecar_t *sc = malloc(sizeof(*sc));
    if (sc == NULL) return ESP_ERR_NO_MEM;
    bool found = false;
    uint32_t best_seq = 0;
    for (uint8_t slot = 0; slot < 2; slot++) {
        char path[160];
        snprintf(path, sizeof(path), "%s.h%u", filepath, (unsigned) slot);
        FILE *f = fopen(path, "rb");
        if (f == NULL) continue;
        size_t n = fread(sc, sizeof(*sc), 1, f);
        fclose(f);
        if (n != 1 || sc->magic != TSDB_SIDECAR_MAGIC) continue;
        if (crc32_(&sc->header, sizeof(sc->header)) != sc->header_crc) {
            ESP_LOGW(TAG, "sidecar slot %u CRC mismatch — ignoring", (unsigned) slot);
            continue;
        }
        if (!found || sc->seq > best_seq) {
            found = true;
            best_seq = sc->seq;
            *out = sc->header;
        }
    }
    free(sc);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void sidecar_remove(const char *filepath) {
    for (uint8_t slot = 0; slot < 2; slot++) {
        char path[160];
        snprintf(path, sizeof(path), "%s.h%u", filepath, (unsigned) slot);
        unlink(path);
    }
}

// ---------------------------------------------------------------------------
// Orphaned .mig (2.2–2.5 schema migration crash window)
// ---------------------------------------------------------------------------

static void migrate_recover(const char *filepath) {
    char mig_path[140];
    snprintf(mig_path, sizeof(mig_path), "%s.mig", filepath);
    FILE *mig = fopen(mig_path, "rb");
    if (mig == NULL) return;

    FILE *main_f = fopen(filepath, "rb");
    if (main_f != NULL) {
        fclose(main_f);
        fclose(mig);
        unlink(mig_path);
        ESP_LOGW(TAG, "Removed stale migration file %s", mig_path);
        return;
    }
    tsdb_header_t *hdr = malloc(sizeof(*hdr));
    bool valid = hdr != NULL && (fread(hdr, sizeof(*hdr), 1, mig) == 1) && (hdr->magic == TSDB_MAGIC);
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
}

// ---------------------------------------------------------------------------
// Header reconstruction from data blocks (corrupt header)
// ---------------------------------------------------------------------------

static esp_err_t reconstruct_header(FILE *file, tsdb_header_t *header,
                                    const tsdb_config_t *config) {
    ESP_LOGW(TAG, "Attempting header reconstruction from data blocks");

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if (file_size < 512) {
        ESP_LOGE(TAG, "File too small to reconstruct");
        return ESP_FAIL;
    }

    memset(header, 0, sizeof(tsdb_header_t));
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
            if (off + TSDB_BLOCK_SIZE > (uint32_t) file_size) continue;
            fseek(file, off, SEEK_SET);
            uint32_t magic = 0;
            if (fread(&magic, sizeof(magic), 1, file) == 1 && magic == TSDB_BLOCK_MAGIC_VALUE) {
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
    uint32_t max_blocks = (file_size - data_offset) / TSDB_BLOCK_SIZE;
    uint8_t *block = malloc(TSDB_BLOCK_SIZE);
    if (block == NULL) return ESP_ERR_NO_MEM;

    uint32_t oldest_ts = UINT32_MAX, newest_ts = 0, oldest_idx = 0, newest_idx = 0, total = 0;
    for (uint32_t b = 0; b < max_blocks; b++) {
        fseek(file, data_offset + b * TSDB_BLOCK_SIZE, SEEK_SET);
        if (fread(block, TSDB_BLOCK_SIZE, 1, file) != 1) continue;
        if (TSDB_BLOCK_MAGIC(block) != TSDB_BLOCK_MAGIC_VALUE || TSDB_BLOCK_COUNT(block) == 0) continue;
        for (uint16_t i = 0; i < TSDB_BLOCK_COUNT(block) && i < header->records_per_block; i++) {
            uint32_t ts = TSDB_BLOCK_TS(block, i);
            if (ts == 0) continue;
            uint32_t idx = b * header->records_per_block + i;
            if (ts < oldest_ts) { oldest_ts = ts; oldest_idx = idx; }
            if (ts > newest_ts) { newest_ts = ts; newest_idx = idx; }
            total++;
        }
    }
    free(block);

    if (total == 0) {
        ESP_LOGW(TAG, "No valid records found in blocks - empty database");
        return ESP_OK;
    }

    header->oldest_record_idx = oldest_idx;
    header->newest_record_idx = newest_idx;
    header->oldest_timestamp = oldest_ts;
    header->newest_timestamp = newest_ts;
    if (header->max_records > 0 && total >= header->max_records) {
        uint32_t next_slot = (newest_idx + 1) % header->max_records;
        header->total_records = header->max_records + next_slot;
        header->oldest_record_idx = next_slot;
        header->total_evictions = next_slot;
    } else {
        header->total_records = total;
        header->total_evictions = 0;
    }
    header->total_writes = header->total_records;

    ESP_LOGW(TAG, "Header reconstructed: %lu records (%lu to %lu)",
             (unsigned long) total, (unsigned long) oldest_ts, (unsigned long) newest_ts);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// V2 -> V3 block layout migration (records_per_block > 38 with struct offsets)
// ---------------------------------------------------------------------------

static esp_err_t migrate_v2_blocks(tsdb_legacy_t *l) {
    tsdb_header_t *h = &l->header;
    uint16_t rpb = h->records_per_block;
    uint8_t np = h->num_params;
    uint32_t max_recs = h->max_records > 0 ? h->max_records : h->total_records;
    uint32_t total_blocks = (max_recs + rpb - 1) / rpb;

    #define OLD_TS_OFFSET(r)       (8 + (r) * 4)
    #define OLD_PARAM_OFFSET(p, r) (160 + (p) * 76 + (r) * 2)

    uint8_t *disk_block = malloc(TSDB_BLOCK_SIZE);
    uint8_t *new_block = malloc(TSDB_BLOCK_SIZE);
    if (!disk_block || !new_block) {
        free(disk_block); free(new_block);
        ESP_LOGE(TAG, "V2->V3 migration alloc failed — will retry next boot");
        return ESP_ERR_NO_MEM;
    }

    // Sentinel: version 99 while blocks are mixed; a crash here makes the
    // next open discard the file.
    h->version = 99;
    write_header(l->file, h);

    for (uint32_t b = 0; b < total_blocks; b++) {
        uint32_t off = tsdb_legacy_block_offset(l, b);
        fseek(l->file, off, SEEK_SET);
        if (fread(disk_block, TSDB_BLOCK_SIZE, 1, l->file) != 1) continue;
        if (TSDB_BLOCK_MAGIC(disk_block) != TSDB_BLOCK_MAGIC_VALUE) continue;
        uint16_t count = TSDB_BLOCK_COUNT(disk_block);
        if (count == 0) continue;
        memset(new_block, 0, TSDB_BLOCK_SIZE);
        TSDB_BLOCK_MAGIC(new_block) = TSDB_BLOCK_MAGIC_VALUE;
        TSDB_BLOCK_COUNT(new_block) = count;
        for (uint16_t r = 0; r < count && r < rpb; r++) {
            TSDB_BLOCK_TS(new_block, r) = *(uint32_t *)(disk_block + OLD_TS_OFFSET(r));
            for (uint8_t p = 0; p < np; p++) {
                uint32_t o = OLD_PARAM_OFFSET(p, r);
                int16_t val = (o + 2 <= TSDB_BLOCK_SIZE) ? *(int16_t *)(disk_block + o) : 0;
                TSDB_BLOCK_PARAM(new_block, rpb, p, r) = val;
            }
        }
        fseek(l->file, off, SEEK_SET);
        fwrite(new_block, TSDB_BLOCK_SIZE, 1, l->file);
    }
    #undef OLD_TS_OFFSET
    #undef OLD_PARAM_OFFSET
    free(disk_block);
    free(new_block);
    fflush(l->file);
    fsync(fileno(l->file));
    h->version = 3;
    write_header(l->file, h);
    ESP_LOGI(TAG, "V2->V3 block layout migration complete (%lu blocks)", (unsigned long) total_blocks);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Open / close / remove
// ---------------------------------------------------------------------------

void tsdb_legacy_remove(const char *path) {
    char p[160];
    unlink(path);
    sidecar_remove(path);
    snprintf(p, sizeof(p), "%s.mig", path);
    unlink(p);
}

void tsdb_legacy_close(tsdb_legacy_t *l) {
    if (l == NULL) return;
    if (l->file) fclose(l->file);
    free(l);
}

esp_err_t tsdb_legacy_open(const char *path, const tsdb_config_t *config, tsdb_legacy_t **out) {
    *out = NULL;
    migrate_recover(path);

    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    tsdb_legacy_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    l->file = f;
    strncpy(l->path, path, sizeof(l->path) - 1);

    bool needs_reconstruction = false;
    if (read_header(f, &l->header) != ESP_OK || l->header.magic != TSDB_MAGIC) {
        ESP_LOGW(TAG, "Legacy file %s: bad/unreadable header", path);
        needs_reconstruction = true;
    } else if (l->header.version == 99) {
        ESP_LOGE(TAG, "Legacy file %s: torn V2->V3 migration — discarding", path);
        tsdb_legacy_close(l);
        tsdb_legacy_remove(path);
        return ESP_FAIL;
    } else if (!header_is_sane(&l->header)) {
        ESP_LOGW(TAG, "Legacy file %s: header fields implausible — reconstructing", path);
        needs_reconstruction = true;
    }

    if (!needs_reconstruction) {
        // A sidecar that is valid, geometrically identical and ahead of the
        // in-file header holds the records an unclean shutdown would lose.
        tsdb_header_t *sc = malloc(sizeof(*sc));
        if (sc != NULL && sidecar_load(path, sc) == ESP_OK) {
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
    } else {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size < 512 || reconstruct_header(f, &l->header, config) != ESP_OK) {
            ESP_LOGE(TAG, "Legacy file %s unusable — discarding", path);
            tsdb_legacy_close(l);
            tsdb_legacy_remove(path);
            return ESP_FAIL;
        }
        // Persist so the next boot does not rescan (one-time in-place write).
        write_header(f, &l->header);
    }

    if (l->header.base_params == 0) {
        l->header.base_params = l->header.num_params;
    }

    if (l->header.version < 3 && l->header.records_per_block > 38) {
        if (migrate_v2_blocks(l) != ESP_OK) {
            tsdb_legacy_close(l);
            return ESP_FAIL;   // leave the file; retry next boot
        }
    }

    // Overflow extras
    if (l->header.extra_param_count > 0 && l->header.overflow_offset > 0) {
        l->extra_count = l->header.extra_param_count;
        l->overflow_record_size = l->header.overflow_record_size;
        l->first_overflow_record_idx = l->header.first_overflow_record_idx;
        l->overflow_data_offset = l->header.overflow_offset + TSDB_OVERFLOW_HEADER_SIZE;
    }

    l->unlimited = (l->header.max_records == 0);
    l->available = l->unlimited ? l->header.total_records :
                   (l->header.total_records < l->header.max_records ?
                    l->header.total_records : l->header.max_records);
    l->first_slot = l->unlimited ? 0 : l->header.oldest_record_idx;

    ESP_LOGI(TAG, "Adopted legacy database %s: %lu records, %u columns (+%u extras)",
             path, (unsigned long) l->available, l->header.num_params, l->extra_count);
    *out = l;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Read-side accessors
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
            fseek(l->file, TSDB_V4_EXT_NAMES_OFFSET + (uint32_t)(idx - 16) * TSDB_V4_EXT_NAME_LEN, SEEK_SET);
            if (fread(out, TSDB_V4_EXT_NAME_LEN, 1, l->file) != 1) memset(out, 0, TSDB_NAME_LEN);
            out[TSDB_NAME_LEN - 1] = '\0';
        }
        return;
    }
    uint8_t extra = idx - l->header.num_params;
    if (extra >= l->extra_count) return;
    fseek(l->file, l->header.overflow_offset + offsetof(tsdb_overflow_header_t, param_names) + extra * 20u, SEEK_SET);
    if (fread(out, 20, 1, l->file) != 1) memset(out, 0, TSDB_NAME_LEN);
    out[TSDB_NAME_LEN - 1] = '\0';
}

void tsdb_legacy_locate(const tsdb_legacy_t *l, uint32_t k, uint32_t *block, uint16_t *off) {
    uint32_t slot = l->unlimited ? k : ((l->first_slot + k) % l->header.max_records);
    *block = slot / l->header.records_per_block;
    *off = (uint16_t)(slot % l->header.records_per_block);
}

uint32_t tsdb_legacy_block_offset(const tsdb_legacy_t *l, uint32_t block) {
    return data_offset_of(&l->header) + block * TSDB_BLOCK_SIZE;
}

uint32_t tsdb_legacy_run(const tsdb_legacy_t *l, uint32_t k) {
    uint32_t slot = l->unlimited ? k : ((l->first_slot + k) % l->header.max_records);
    uint32_t run = l->header.records_per_block - (slot % l->header.records_per_block);
    if (!l->unlimited) {
        uint32_t to_ring_end = l->header.max_records - slot;
        if (run > to_ring_end) run = to_ring_end;
    }
    uint32_t left = l->available - k;
    return run < left ? run : left;
}

void tsdb_legacy_read_extras(tsdb_legacy_t *l, uint32_t k, int16_t *out) {
    memset(out, 0, l->extra_count * sizeof(int16_t));
    if (l->extra_count == 0) return;
    uint32_t abs_idx = (l->header.total_records - l->available) + k;
    if (abs_idx < l->first_overflow_record_idx) return;
    uint32_t off = l->overflow_data_offset +
                   (abs_idx - l->first_overflow_record_idx) * l->overflow_record_size;
    fseek(l->file, off, SEEK_SET);
    if (fread(out, sizeof(int16_t), l->extra_count, l->file) != l->extra_count) {
        memset(out, 0, l->extra_count * sizeof(int16_t));
    }
}

long tsdb_legacy_file_size(const tsdb_legacy_t *l) {
    struct stat st;
    return stat(l->path, &st) == 0 ? (long) st.st_size : 0;
}
