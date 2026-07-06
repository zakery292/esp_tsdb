/**
 * @file tsdb_migrate.c
 * @brief Full schema migration: change base columns via a streaming rewrite.
 *
 * MySQL-style ALTER TABLE for the ring buffer: the block geometry
 * (records_per_block, per-column offsets) is derived from num_params, so
 * changing the base column set requires rewriting every block. This module
 * does that as a streaming copy — one record in, one record out — into a
 * sibling `<filepath>.mig` file with the new geometry, then atomically swaps
 * it into place. Constant memory (~2 block buffers) regardless of database
 * size.
 *
 * Columns are matched BY NAME against the old base columns AND the old
 * overflow extras — so a migration folds any overflow region into first-class
 * base columns (the new file never has one). Old columns absent from the new
 * schema are dropped; new names are backfilled with 0.
 *
 * Crash safety: the original file is never modified. A crash mid-migration
 * leaves a stale `.mig` that tsdb_open() cleans up (or adopts, if the crash
 * hit the tiny window where the original was already unlinked but the rename
 * hadn't landed).
 *
 * Concurrency: runs under the handle mutex with db->migrating set, so
 * concurrent writers/queriers fail fast with ESP_ERR_INVALID_STATE instead of
 * stalling. On a 5-minute write cadence a migration that completes within the
 * interval loses no samples; callers may hold one record and retry.
 *
 * Task placement (ESP32): the rewrite runs on the CALLER's task. On littlefs/
 * SPI-flash targets that means real flash-bus ops — the calling task must
 * have an internal-RAM stack (same rule as any flash writer). SD-over-SPI
 * never disables the flash cache, so PSRAM-stacked tasks may migrate /sdcard
 * databases freely.
 */

#include "tsdb_internal.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "TSDB_MIGRATE";

// Keep some filesystem headroom out of the caller's free-space figure so the
// migration never runs the FS to zero (littlefs needs slack for its own
// metadata/compaction).
#define TSDB_MIGRATE_FS_RESERVE_PCT 5

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

/**
 * @brief Size of a database file with the given geometry (header area +
 *        index + data blocks; no overflow — migrated files never have one).
 */
static uint32_t migrate_file_size(uint32_t index_offset, uint16_t rpb,
                                  uint32_t index_entries, uint32_t n_records) {
    uint32_t blocks = rpb ? ((n_records + rpb - 1) / rpb) : 0;
    return index_offset + (index_entries * 8) + (blocks * TSDB_BLOCK_SIZE);
}

/**
 * @brief Read one retained record (base + extras) by logical position.
 *
 * @param k Logical position: 0 = oldest retained record.
 * @param blk_buf Caller's block buffer; *cached_blk tracks its content.
 * @param out_ts / out_vals Full row out (num_params base + extra_count).
 * @return ESP_OK, or an error on unreadable block.
 */
static esp_err_t migrate_read_record(tsdb_t *db, uint32_t k, uint32_t available,
                                     uint8_t *blk_buf, uint32_t *cached_blk,
                                     uint32_t *out_ts, int16_t *out_vals) {
    uint16_t rpb = db->header.records_per_block;
    bool unlimited = (db->header.max_records == 0);
    uint32_t slot = unlimited ? k :
                    ((db->header.oldest_record_idx + k) % db->header.max_records);
    uint32_t blk = slot / rpb;

    if (*cached_blk != blk) {
        esp_err_t ret = tsdb_read_block(db, blk, (tsdb_block_t *)blk_buf);
        if (ret != ESP_OK) {
            return ret;
        }
        *cached_blk = blk;
    }

    *out_ts = TSDB_BLOCK_TS(blk_buf, slot % rpb);
    for (uint8_t p = 0; p < db->header.num_params; p++) {
        out_vals[p] = TSDB_BLOCK_PARAM(blk_buf, rpb, p, slot % rpb);
    }

    // Overflow extras: sequential region indexed by absolute record number.
    if (db->extra_param_count > 0) {
        uint32_t abs_idx = (db->header.total_records - available) + k;
        if (abs_idx >= db->first_overflow_record_idx) {
            uint32_t ovf_offset = db->overflow_data_offset +
                ((abs_idx - db->first_overflow_record_idx) * db->overflow_record_size);
            fseek(db->file, ovf_offset, SEEK_SET);
            if (fread(&out_vals[db->header.num_params], sizeof(int16_t),
                      db->extra_param_count, db->file) != db->extra_param_count) {
                memset(&out_vals[db->header.num_params], 0,
                       db->extra_param_count * sizeof(int16_t));
            }
        } else {
            memset(&out_vals[db->header.num_params], 0,
                   db->extra_param_count * sizeof(int16_t));
        }
    }

    return ESP_OK;
}

// ============================================================================
// STALE / ORPHANED .mig HANDLING (called from tsdb_open)
// ============================================================================

void tsdb_migrate_recover(const char *filepath) {
    char mig_path[140];
    snprintf(mig_path, sizeof(mig_path), "%s.mig", filepath);

    FILE *mig = fopen(mig_path, "rb");
    if (mig == NULL) {
        return;                     // no .mig — nothing to do (common case)
    }

    FILE *main_f = fopen(filepath, "rb");
    if (main_f != NULL) {
        // Original still present: the migration never reached the swap, so
        // the .mig is an incomplete leftover. The original is authoritative.
        fclose(main_f);
        fclose(mig);
        unlink(mig_path);
        ESP_LOGW(TAG, "Removed stale migration file %s", mig_path);
        return;
    }

    // Original missing: crash hit the unlink->rename window. The .mig was
    // fully written and fsynced before that window, so adopt it — but only
    // if its header checks out.
    tsdb_header_t hdr;
    bool valid = (fread(&hdr, sizeof(hdr), 1, mig) == 1) &&
                 (hdr.magic == TSDB_MAGIC);
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

// ============================================================================
// SCHEMA MIGRATION
// ============================================================================

/**
 * @brief Resolve the i-th NEW column name: from the caller's list, or (in
 *        same-schema/resize mode) from the current file's name stores.
 *        May return tsdb_get_param_name_h's static buffer — copy immediately.
 */
static const char *engine_new_name(tsdb_t *db, const char **new_names, uint8_t i) {
    return new_names != NULL ? new_names[i] : tsdb_get_param_name_h(db, i);
}

/**
 * @brief Shared streaming-rewrite engine behind tsdb_migrate_schema_h and
 *        tsdb_resize_h.
 *
 * @param new_names New column names, or NULL for same-schema mode (keep the
 *        current columns — base + folded extras — identity-mapped).
 * @param new_count Ignored when new_names == NULL.
 * @param set_max / new_max_override Ring-capacity override (tsdb_resize);
 *        when !set_max the current max_records is kept (schema migration).
 */
static esp_err_t tsdb_migrate_engine(tsdb_t *db, const char **new_names,
                                     uint8_t new_count,
                                     const tsdb_migrate_opts_t *opts,
                                     bool set_max, uint32_t new_max_override) {
    TSDB_LOCK_OR_RETURN(db, 30000, ESP_ERR_TIMEOUT);

    uint8_t old_base = db->header.num_params;
    uint8_t old_total = old_base + db->extra_param_count;

    // ---- 1. Column mapping: new[i] <- old row index, or -1 (zeros).
    //         Same-schema mode keeps every current column (base + extras,
    //         which get folded into base) identity-mapped. Named mode
    //         resolves old columns via tsdb_get_param_name_h, which covers
    //         all three name stores (fixed header <16, V4 extension 16..63,
    //         overflow extras). NOTE: names of columns at index >= 16 are
    //         stored 19 chars max; keep param names short so cross-slot
    //         matches never truncate.
    int8_t col_map[TSDB_MAX_PARAMS];
    bool schemas_identical;
    if (new_names == NULL) {
        new_count = old_total;
        for (uint8_t i = 0; i < new_count; i++) col_map[i] = (int8_t)i;
        schemas_identical = (db->extra_param_count == 0);
    } else {
        schemas_identical = (new_count == old_base && db->extra_param_count == 0);
        for (uint8_t i = 0; i < new_count; i++) {
            col_map[i] = -1;
            for (uint8_t j = 0; j < old_total; j++) {
                // Static-buffer return: compare immediately, don't hold.
                const char *old_name = tsdb_get_param_name_h(db, j);
                if (old_name == NULL) continue;
                bool match = (j < 16) ? (strcmp(new_names[i], old_name) == 0)
                                      : (strncmp(new_names[i], old_name,
                                                 TSDB_V4_EXT_NAME_LEN - 1) == 0);
                if (match) {
                    col_map[i] = (int8_t)j;
                    break;
                }
            }
            if (col_map[i] != (int8_t)i) schemas_identical = false;
            ESP_LOGI(TAG, "  %-20s <- %s", new_names[i],
                     col_map[i] < 0 ? "(new, zeros)" : "old column");
        }
    }
    bool max_changes = set_max && (new_max_override != db->header.max_records);
    if (schemas_identical && !max_changes) {
        ESP_LOGI(TAG, "Schema already matches — nothing to migrate");
        tsdb_unlock(db);
        return ESP_OK;
    }

    // ---- 2. New geometry + retained-record budget (trim-to-fit).
    bool wide = (new_count > 16);   // V4 layout: ext names region, index at 2048
    uint32_t new_index_offset = wide ? TSDB_V4_INDEX_OFFSET : 1024;
    uint16_t new_rpb = (TSDB_BLOCK_SIZE - 8) / (4 + new_count * 2);
    bool unlimited = (db->header.max_records == 0);
    uint32_t available = unlimited ? db->header.total_records :
                         ((db->header.total_records < db->header.max_records) ?
                          db->header.total_records : db->header.max_records);
    uint32_t retain = available;

    uint32_t new_max = set_max ? new_max_override :
                       db->header.max_records;    // keep capacity intent
    // Shrinking below the retained count: newest records win.
    if (new_max > 0 && retain > new_max) {
        ESP_LOGW(TAG, "Resize below retained count: keeping newest %lu of %lu",
                 (unsigned long)new_max, (unsigned long)retain);
        retain = new_max;
    }
    uint32_t index_stride = db->header.index_stride;
    uint32_t new_index_entries = new_max > 0 ?
        (new_max / index_stride) + 1 : 256;

    if (opts != NULL && opts->free_space_bytes > 0) {
        uint32_t budget = opts->free_space_bytes -
                          (opts->free_space_bytes / 100 * TSDB_MIGRATE_FS_RESERVE_PCT);
        uint32_t needed = migrate_file_size(new_index_offset, new_rpb, new_index_entries, retain);
        if (needed > budget) {
            if (opts == NULL || !opts->allow_trim) {
                ESP_LOGE(TAG, "Migration needs %lu bytes, budget %lu — refusing (allow_trim not set)",
                         (unsigned long)needed, (unsigned long)budget);
                tsdb_unlock(db);
                return ESP_ERR_NO_MEM;
            }
            // Shrink the retained window (newest records win) until it fits.
            uint32_t fixed = new_index_offset + (new_index_entries * 8);
            uint32_t blocks_budget = (budget > fixed) ?
                                     ((budget - fixed) / TSDB_BLOCK_SIZE) : 0;
            uint32_t fit = blocks_budget * new_rpb;
            if (fit == 0) {
                ESP_LOGE(TAG, "Budget %lu bytes fits zero records — aborting",
                         (unsigned long)budget);
                tsdb_unlock(db);
                return ESP_ERR_NO_MEM;
            }
            if (fit < retain) {
                ESP_LOGW(TAG, "Trim-to-fit: keeping newest %lu of %lu records",
                         (unsigned long)fit, (unsigned long)retain);
                retain = fit;
                // The ring must not outgrow the space we proved we have.
                if (new_max == 0 || new_max > fit) {
                    new_max = fit;
                    new_index_entries = (new_max / index_stride) + 1;
                }
            }
        }
    }

    // ---- 3. Migration begins: block concurrent writers/queriers fast.
    db->migrating = true;
    ESP_LOGI(TAG, "Schema migration: %u base + %u extra -> %u base, %lu records",
             old_base, db->extra_param_count, new_count, (unsigned long)retain);

    char mig_path[140];
    snprintf(mig_path, sizeof(mig_path), "%s.mig", db->filepath);
    unlink(mig_path);                       // clear any stale leftover

    uint8_t *rd_blk = malloc(TSDB_BLOCK_SIZE);
    uint8_t *wr_blk = malloc(TSDB_BLOCK_SIZE);
    FILE *out = fopen(mig_path, "w+b");
    if (rd_blk == NULL || wr_blk == NULL || out == NULL) {
        ESP_LOGE(TAG, "Migration setup failed (buffers/file)");
        free(rd_blk); free(wr_blk);
        if (out) { fclose(out); unlink(mig_path); }
        db->migrating = false;
        tsdb_unlock(db);
        return ESP_ERR_NO_MEM;
    }

    // ---- 4. New header + zeroed index.
    tsdb_header_t nh;
    memset(&nh, 0, sizeof(nh));
    nh.magic = TSDB_MAGIC;
    nh.version = wide ? TSDB_VERSION_WIDE : TSDB_VERSION;
    nh.num_params = new_count;
    nh.base_params = new_count;
    nh.param_size = sizeof(int16_t);
    nh.record_size = 4 + new_count * 2;
    nh.records_per_block = new_rpb;
    nh.max_records = new_max;
    nh.index_stride = index_stride;
    // 1024 (V3) / 2048 (V4), not the legacy 512: sizeof(tsdb_header_t) is 584,
    // so a 512 base would let the header rewrite clobber the index (and, with
    // a small index, the first data block).
    nh.index_offset = new_index_offset;
    nh.index_entries = new_index_entries;
    for (uint8_t i = 0; i < new_count && i < 16; i++) {
        const char *nm = engine_new_name(db, new_names, i);
        strncpy(nh.param_names[i], nm ? nm : "", 31);
        nh.param_names[i][31] = '\0';
    }

    esp_err_t err = ESP_OK;
    if (fwrite(&nh, sizeof(nh), 1, out) != 1) err = ESP_FAIL;
    if (err == ESP_OK && wide) {
        // V4 extended names region (params 16..63, 20 bytes each). Written
        // once here; header rewrites (0..584) never touch it.
        char ext_name[TSDB_V4_EXT_NAME_LEN];
        fseek(out, TSDB_V4_EXT_NAMES_OFFSET, SEEK_SET);
        for (uint8_t i = 16; i < new_count && err == ESP_OK; i++) {
            const char *nm = engine_new_name(db, new_names, i);
            memset(ext_name, 0, sizeof(ext_name));
            if (nm) strncpy(ext_name, nm, TSDB_V4_EXT_NAME_LEN - 1);
            if (fwrite(ext_name, TSDB_V4_EXT_NAME_LEN, 1, out) != 1) err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        tsdb_index_entry_t zero = {0};
        fseek(out, nh.index_offset, SEEK_SET);
        for (uint32_t i = 0; i < nh.index_entries && err == ESP_OK; i++) {
            if (fwrite(&zero, sizeof(zero), 1, out) != 1) err = ESP_FAIL;
        }
    }

    // ---- 5. Stream records oldest->newest, skipping trimmed ones. One
    //         record in, one record out; no per-block fsync (one at the end).
    uint32_t skip = available - retain;     // oldest records dropped by trim
    uint32_t cached_blk = UINT32_MAX;
    int16_t row[TSDB_MAX_PARAMS];
    uint32_t out_written = 0;
    uint32_t data_offset = nh.index_offset + nh.index_entries * sizeof(tsdb_index_entry_t);

    // Progress reporting: start callback now, then every progress_every
    // records (auto = ~1% of the copy), and once more after the loop.
    tsdb_migrate_progress_cb progress = (opts != NULL) ? opts->progress : NULL;
    void *progress_ctx = (opts != NULL) ? opts->progress_ctx : NULL;
    uint32_t progress_every = (opts != NULL && opts->progress_every > 0) ?
                              opts->progress_every :
                              (retain / 100 > 0 ? retain / 100 : 1);
    if (progress != NULL) {
        progress(0, retain, progress_ctx);
    }

    memset(wr_blk, 0, TSDB_BLOCK_SIZE);
    TSDB_BLOCK_MAGIC(wr_blk) = 0x424C4B54;

    for (uint32_t k = skip; k < available && err == ESP_OK; k++) {
        uint32_t ts;
        if (migrate_read_record(db, k, available, rd_blk, &cached_blk,
                                &ts, row) != ESP_OK) {
            ESP_LOGW(TAG, "Unreadable source block at logical %lu — stopping copy early",
                     (unsigned long)k);
            break;      // keep what we have; better than losing the rest
        }

        uint16_t off = out_written % new_rpb;
        TSDB_BLOCK_TS(wr_blk, off) = ts;
        for (uint8_t i = 0; i < new_count; i++) {
            TSDB_BLOCK_PARAM(wr_blk, new_rpb, i, off) =
                (col_map[i] >= 0 && col_map[i] < old_total) ? row[(uint8_t)col_map[i]] : 0;
        }
        TSDB_BLOCK_COUNT(wr_blk) = off + 1;

        // Sparse index entry at stride boundaries (same scheme as tsdb_write).
        if (out_written % index_stride == 0) {
            uint32_t entry_num = out_written / index_stride;
            if (entry_num < nh.index_entries) {
                tsdb_index_entry_t e = { .timestamp = ts,
                                         .block_number = out_written / new_rpb };
                fseek(out, nh.index_offset + entry_num * sizeof(e), SEEK_SET);
                fwrite(&e, sizeof(e), 1, out);
            }
        }

        if (out_written == 0) nh.oldest_timestamp = ts;
        nh.newest_timestamp = ts;
        out_written++;

        // Flush the output block when full (or on the final record below).
        if (off == new_rpb - 1 || k == available - 1) {
            uint32_t blk_num = (out_written - 1) / new_rpb;
            fseek(out, data_offset + blk_num * TSDB_BLOCK_SIZE, SEEK_SET);
            errno = 0;
            if (fwrite(wr_blk, TSDB_BLOCK_SIZE, 1, out) != 1) {
                ESP_LOGE(TAG, "Migration write failed at block %lu (errno=%d)",
                         (unsigned long)blk_num, errno);
                err = (errno == ENOSPC) ? ESP_ERR_NO_MEM : ESP_FAIL;
                break;
            }
            memset(wr_blk, 0, TSDB_BLOCK_SIZE);
            TSDB_BLOCK_MAGIC(wr_blk) = 0x424C4B54;
        }

        if (progress != NULL && out_written < retain &&
            out_written % progress_every == 0) {
            progress(out_written, retain, progress_ctx);
        }
        if (out_written % 10000 == 0) {
            ESP_LOGI(TAG, "  migrated %lu / %lu records",
                     (unsigned long)out_written, (unsigned long)retain);
        }
    }

    if (progress != NULL) {
        progress(out_written, retain, progress_ctx);   // completion (or early stop)
    }

    // ---- 6. Finalize the new header and durably commit the .mig.
    if (err == ESP_OK) {
        nh.total_records = out_written;
        nh.total_writes = out_written;
        nh.oldest_record_idx = 0;
        nh.newest_record_idx = out_written > 0 ? out_written - 1 : 0;
        nh.total_evictions = 0;
        fseek(out, 0, SEEK_SET);
        if (fwrite(&nh, sizeof(nh), 1, out) != 1) err = ESP_FAIL;
        fflush(out);
        fsync(fileno(out));
    }

    free(rd_blk);
    free(wr_blk);
    fclose(out);

    if (err != ESP_OK) {
        unlink(mig_path);
        db->migrating = false;
        tsdb_unlock(db);
        return err;
    }

    // ---- 7. Swap into place. Close the original first; FATFS (SD) cannot
    //         rename over an existing target, so unlink-then-rename. A crash
    //         inside this window is recovered by tsdb_migrate_recover() at
    //         next open (the fsynced .mig gets adopted).
    fclose(db->file);
    db->file = NULL;
    unlink(db->filepath);
    if (rename(mig_path, db->filepath) != 0) {
        ESP_LOGE(TAG, "Rename %s -> %s failed (errno=%d) — reopen will adopt the .mig",
                 mig_path, db->filepath, errno);
        db->is_open = false;
        db->migrating = false;
        tsdb_unlock(db);
        return ESP_FAIL;
    }

    db->file = fopen(db->filepath, "r+b");
    if (db->file == NULL) {
        ESP_LOGE(TAG, "Reopen after migration failed");
        db->is_open = false;
        db->migrating = false;
        tsdb_unlock(db);
        return ESP_FAIL;
    }

    // ---- 8. Adopt the new schema in the live handle. No overflow anymore.
    memcpy(&db->header, &nh, sizeof(nh));
    db->extra_param_count = 0;
    db->overflow_record_size = 0;
    db->overflow_data_offset = 0;
    db->first_overflow_record_idx = 0;
    db->capacity_adapted = false;
    db->migrating = false;

    ESP_LOGI(TAG, "Schema migration complete: %lu records, %u params, rpb=%u",
             (unsigned long)out_written, new_count, new_rpb);

    tsdb_unlock(db);
    return ESP_OK;
}

// ============================================================================
// PUBLIC API
// ============================================================================

esp_err_t tsdb_migrate_schema_h(tsdb_t *db, const char **new_names,
                                uint8_t new_count,
                                const tsdb_migrate_opts_t *opts) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;
    if (new_names == NULL || new_count == 0 || new_count > TSDB_MAX_PARAMS) {
        return ESP_ERR_INVALID_ARG;
    }
    return tsdb_migrate_engine(db, new_names, new_count, opts, false, 0);
}

esp_err_t tsdb_resize_h(tsdb_t *db, uint32_t new_max_records,
                        const tsdb_migrate_opts_t *opts) {
    if (db == NULL || !db->is_open) return ESP_ERR_INVALID_STATE;

    TSDB_LOCK_OR_RETURN(db, 30000, ESP_ERR_TIMEOUT);

    if (new_max_records == db->header.max_records) {
        tsdb_unlock(db);
        return ESP_OK;
    }

    // Fast path: the layout is still linear (never wrapped) and the new
    // capacity covers every existing record, so slot == record % new_max
    // holds for all data — a header-only change, milliseconds. Excluded when
    // an overflow region exists: growing the base data region would extend
    // into the overflow appended after it (the slow path folds extras into
    // base columns instead). Relies on the index-write bounds guard in
    // tsdb_write.c — index entries past the preallocated region are skipped,
    // never written into data blocks.
    bool linear = (db->header.oldest_record_idx == 0) &&
                  (db->header.max_records == 0 ||
                   db->header.total_records <= db->header.max_records);
    bool covers = (new_max_records == 0) ||
                  (new_max_records >= db->header.total_records);
    if (linear && covers && db->extra_param_count == 0) {
        uint32_t old_max = db->header.max_records;
        db->header.max_records = new_max_records;
        esp_err_t ret = tsdb_write_header(db->file, &db->header);
        if (ret != ESP_OK) {
            db->header.max_records = old_max;   // keep RAM/disk consistent
        } else {
            db->capacity_adapted = false;       // capacity changed: re-arm ENOSPC adapt
            ESP_LOGI(TAG, "Resize (fast): max_records %lu -> %lu",
                     (unsigned long)old_max, (unsigned long)new_max_records);
        }
        tsdb_unlock(db);
        return ret;
    }

    // Slow path: wrapped ring, shrink below the retained count, or overflow
    // present — same-schema streaming rewrite through the migration engine
    // (recursive lock: the engine takes and releases its own count).
    esp_err_t ret = tsdb_migrate_engine(db, NULL, 0, opts, true, new_max_records);
    tsdb_unlock(db);
    return ret;
}

// ============================================================================
// LEGACY GLOBAL API
// ============================================================================

esp_err_t tsdb_migrate_schema(const char **new_names, uint8_t new_count,
                              const tsdb_migrate_opts_t *opts) {
    return tsdb_migrate_schema_h(g_default_handle, new_names, new_count, opts);
}

esp_err_t tsdb_resize(uint32_t new_max_records, const tsdb_migrate_opts_t *opts) {
    return tsdb_resize_h(g_default_handle, new_max_records, opts);
}
