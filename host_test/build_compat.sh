#!/bin/bash
# Builds test_compat from this tree (test_compat_new) and from the last 2.x
# releases (test_compat_v2.4.3, test_compat_v2.4.4; COMPAT_REFS overrides).
# run_compat.sh drives them.
set -e
cd "$(dirname "$0")"
REFS=${COMPAT_REFS:-"v2.4.3 v2.4.4"}
gcc -g -O0 -Wall -Wno-unused -I shims -I ../include -I ../src test_compat.c \
  ../src/tsdb_core.c ../src/tsdb_write.c ../src/tsdb_query.c ../src/tsdb_buffer.c \
  ../src/tsdb_legacy.c ../src/tsdb_segment.c -o test_compat_new
for REF in $REFS; do
  OLD=.compat_old/$REF
  rm -rf "$OLD" && mkdir -p "$OLD"
  git -C .. archive "$REF" src include | tar -x -C "$OLD"
  SRCS="tsdb_core.c tsdb_write.c tsdb_index.c tsdb_query.c tsdb_migrate.c tsdb_buffer.c"
  gcc -g -O0 -w -I shims -I "$OLD/include" -I "$OLD/src" test_compat.c \
    $(for f in $SRCS; do echo "$OLD/src/$f"; done) -o "test_compat_$REF"
done
