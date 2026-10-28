#!/bin/bash
# Regenerates host_test/fixtures/ from the 2.5.0 sources (git 570dd76), the
# last single-file release. The 3.0 legacy reader must dump each fixture
# identically to its .expected file (see test_legacy.c).
set -e
cd "$(dirname "$0")"
REF=${1:-570dd76}
T=$(mktemp -d)
mkdir -p "$T/src" "$T/include"
for f in src/tsdb_core.c src/tsdb_write.c src/tsdb_index.c src/tsdb_query.c \
         src/tsdb_migrate.c src/tsdb_buffer.c src/tsdb_internal.h include/esp_tsdb.h; do
  git show "$REF:$f" > "$T/$f"
done
gcc -O0 -g -Wno-unused -I shims -I "$T/include" -I "$T/src" fixture_gen.c \
  "$T"/src/tsdb_core.c "$T"/src/tsdb_write.c "$T"/src/tsdb_index.c \
  "$T"/src/tsdb_query.c "$T"/src/tsdb_migrate.c "$T"/src/tsdb_buffer.c -o "$T/fixture_gen"
mkdir -p fixtures
"$T/fixture_gen" fixtures
rm -rf "$T"
ls -la fixtures
