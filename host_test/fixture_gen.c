// Legacy-format fixture generator. Compiled by make_fixtures.sh against the
// 2.5.0 sources (the last single-file release) to produce databases in every
// on-disk variant the 3.0 legacy reader must adopt, plus the expected dump of
// each as 2.5.0 itself reads it. test_legacy.c re-dumps them under 3.0 and
// diffs. Uses only the 2.5.0 public API.
#include "esp_tsdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0 100000u
#define DT 10u

static const char *dir;
static char pbuf[512];
static const char *P(const char *name) { snprintf(pbuf, sizeof pbuf, "%s/%s", dir, name); return pbuf; }

static void wipe(const char *name) {
    char b[600]; const char *p = P(name);
    remove(p);
    snprintf(b, sizeof b, "%s.h0", p); remove(b);
    snprintf(b, sizeof b, "%s.h1", p); remove(b);
    snprintf(b, sizeof b, "%s.mig", p); remove(b);
}

static tsdb_config_t cfg(const char *name, uint8_t np, const char **names, uint32_t maxrec) {
    tsdb_config_t c = {0};
    c.filepath = strdup(P(name)); c.num_params = np; c.param_names = names;
    c.max_records = maxrec; c.index_stride = 10;
    c.buffer_pool_size = 16 * 1024; c.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
    return c;
}

static void row(int i, int np, int16_t *v) {
    for (int p = 0; p < np; p++) v[p] = (int16_t)((i * (p + 3) + p * 1000) % 30000 - 15000);
}

// Dump: names, stats, then every retained record (all columns).
static void dump(tsdb_t *db, const char *name) {
    char out[600]; snprintf(out, sizeof out, "%s.expected", P(name));
    FILE *f = fopen(out, "w");
    uint8_t total = tsdb_get_total_params_h(db);
    fprintf(f, "columns:");
    for (uint8_t i = 0; i < total; i++) { const char *n = tsdb_get_param_name_h(db, i); fprintf(f, "%s%s", i ? "," : "", n ? n : ""); }
    fprintf(f, "\n");
    tsdb_stats_t s; tsdb_get_stats_h(db, &s);
    fprintf(f, "stats: records=%u max=%u oldest=%u newest=%u writes=%u evictions=%u params=%u extras=%u\n",
            s.total_records, s.max_records, s.oldest_timestamp, s.newest_timestamp,
            s.total_writes, s.total_evictions, s.num_params, s.extra_params);
    tsdb_query_t q; tsdb_query_init_h(db, &q, 0, UINT32_MAX, NULL, 0);
    uint32_t ts; int16_t v[64];
    while (tsdb_query_next(&q, &ts, v) == ESP_OK) {
        fprintf(f, "%u", ts);
        for (uint8_t i = 0; i < total; i++) fprintf(f, ",%d", v[i]);
        fprintf(f, "\n");
    }
    tsdb_query_close(&q);
    fclose(f);
}

int main(int argc, char **argv) {
    dir = argc > 1 ? argv[1] : "fixtures";
    const char *n3[] = {"soc", "vbat", "pload"};
    const char *n2[] = {"soc", "vbat"};

    // 1. wrapped twice: 3 cols, ring 150, 400 writes, clean close
    wipe("wrapped.tsdb");
    { tsdb_config_t c = cfg("wrapped.tsdb", 3, n3, 150); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 400; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c); dump(db, "wrapped.tsdb"); tsdb_close_h(db); }

    // 2. overflow extras: 2 base + 1 extra at write 100, 260 writes, ring 1000
    wipe("extras.tsdb");
    { tsdb_config_t c = cfg("extras.tsdb", 2, n2, 1000); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 260; i++) {
          if (i == 100) { const char *x[] = {"tbat"}; tsdb_add_extra_params_h(db, x, 1); }
          int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c); dump(db, "extras.tsdb"); tsdb_close_h(db); }

    // 3. V4 wide: 20 cols, ring 100, 50 writes
    wipe("wide.tsdb");
    { static char nb[20][8]; const char *w[20]; for (int i = 0; i < 20; i++) { snprintf(nb[i], 8, "w%02d", i); w[i] = nb[i]; }
      tsdb_config_t c = cfg("wide.tsdb", 20, w, 100); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 50; i++) { int16_t v[20]; row(i, 20, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c); dump(db, "wide.tsdb"); tsdb_close_h(db); }

    // 4. legacy index at 512 (pre-2.2 layout), 5 writes
    wipe("legacy512.tsdb");
    { tsdb_header_t h; memset(&h, 0, sizeof h);
      h.magic = TSDB_MAGIC; h.version = TSDB_VERSION; h.num_params = 2; h.base_params = 2;
      h.param_size = 2; h.record_size = 8; h.records_per_block = (TSDB_BLOCK_SIZE - 8) / 8;
      h.max_records = 100; h.index_stride = 10; h.index_offset = 512; h.index_entries = 11;
      strcpy(h.param_names[0], "soc"); strcpy(h.param_names[1], "vbat");
      FILE *f = fopen(P("legacy512.tsdb"), "wb"); fwrite(&h, sizeof h, 1, f);
      fseek(f, 512 + 11 * 8, SEEK_SET); uint8_t z[TSDB_BLOCK_SIZE] = {0}; fwrite(z, sizeof z, 1, f); fclose(f);
      tsdb_config_t c = cfg("legacy512.tsdb", 2, n2, 100); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 5; i++) { int16_t v[2]; row(i, 2, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c); dump(db, "legacy512.tsdb"); tsdb_close_h(db); }

    // 5. sidecar ahead of the in-file header: 50 writes + close, 30 more, NO close
    wipe("sidecar.tsdb");
    { tsdb_config_t c = cfg("sidecar.tsdb", 3, n3, 500); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 50; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c);
      for (int i = 50; i < 80; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
      dump(db, "sidecar.tsdb");   // what 2.5 sees with the sidecar ahead; handle leaked on purpose
    }

    // 6. unlimited: max 0, 300 writes
    wipe("unlimited.tsdb");
    { tsdb_config_t c = cfg("unlimited.tsdb", 3, n3, 0); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 300; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c); dump(db, "unlimited.tsdb"); tsdb_close_h(db); }

    // 7. not wrapped, partial last block: 3 cols, ring 1000, 70 writes
    wipe("partial.tsdb");
    { tsdb_config_t c = cfg("partial.tsdb", 3, n3, 1000); tsdb_t *db = tsdb_open(&c);
      for (int i = 0; i < 70; i++) { int16_t v[3]; row(i, 3, v); tsdb_write_h(db, T0 + i * DT, v); }
      tsdb_close_h(db); db = tsdb_open(&c); dump(db, "partial.tsdb"); tsdb_close_h(db); }

    printf("fixtures written to %s\n", dir);
    return 0;
}
