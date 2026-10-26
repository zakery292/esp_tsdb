#!/bin/bash
# Builds test_compat twice: test_compat_new from this tree, test_compat_old
# from an older release (COMPAT_REF, default v2.4.3). run_compat.sh drives them.
set -e
cd "$(dirname "$0")"
REF=${COMPAT_REF:-v2.4.3}
OLD=.compat_old
rm -rf "$OLD" && mkdir -p "$OLD"
git -C .. archive "$REF" src include | tar -x -C "$OLD"
SRCS="tsdb_core.c tsdb_write.c tsdb_index.c tsdb_query.c tsdb_migrate.c tsdb_buffer.c"
gcc -g -O0 -Wall -Wno-unused -I shims -I ../include -I ../src test_compat.c \
  $(for f in $SRCS; do echo ../src/$f; done) -o test_compat_new
gcc -g -O0 -w -I shims -I "$OLD/include" -I "$OLD/src" test_compat.c \
  $(for f in $SRCS; do echo "$OLD/src/$f"; done) -o test_compat_old
