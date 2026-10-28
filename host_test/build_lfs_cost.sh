#!/bin/bash
# Builds test_lfs_cost: esp_tsdb on the real littlefs (the version esp_littlefs
# ships) over a RAM block device. LFS_DIR may point at a littlefs checkout;
# otherwise littlefs is cloned into host_test/.littlefs (git-ignored).
# TSDB_SRC/TSDB_INC select the esp_tsdb sources (default: this tree), e.g. to
# measure a release: git archive v2.4.3 src include | tar -x -C /tmp/old &&
#   TSDB_SRC=/tmp/old/src TSDB_INC=/tmp/old/include OUT=test_lfs_cost_old ./build_lfs_cost.sh
set -e
cd "$(dirname "$0")"
LFS_TAG=${LFS_TAG:-v2.11.3}   # joltwallet/esp_littlefs v1.23.1 ships v2.11.3
if [ -z "$LFS_DIR" ]; then
  LFS_DIR=.littlefs
  if [ ! -f "$LFS_DIR/lfs.c" ]; then
    git clone -q --depth 1 -b "$LFS_TAG" https://github.com/littlefs-project/littlefs "$LFS_DIR"
  fi
fi
TSDB_SRC=${TSDB_SRC:-../src}
TSDB_INC=${TSDB_INC:-../include}
OUT=${OUT:-test_lfs_cost}
# 3.x source set, or the 2.x one when TSDB_SRC points at an older release.
SRCS=""
for f in tsdb_core.c tsdb_write.c tsdb_query.c tsdb_buffer.c \
         tsdb_legacy.c tsdb_segment.c tsdb_convert.c tsdb_index.c tsdb_migrate.c; do
  [ -f "$TSDB_SRC/$f" ] && SRCS="$SRCS $TSDB_SRC/$f"
done
mkdir -p .lfs_build
python3 lfs/instrument_lfs.py "$LFS_DIR/lfs.c" .lfs_build/lfs.c
gcc -O2 -g -Wall -Wno-unused -I shims -I "$TSDB_INC" -I "$TSDB_SRC" -I lfs -I "$LFS_DIR" \
  -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  test_lfs_cost.c lfs/lfs_stdio.c .lfs_build/lfs.c "$LFS_DIR/lfs_util.c" \
  $SRCS \
  -Wl,--wrap=fopen,--wrap=fileno,--wrap=fsync,--wrap=unlink,--wrap=rename,--wrap=stat,--wrap=mkdir,--wrap=rmdir \
  -Wl,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=truncate \
  -o "$OUT"
