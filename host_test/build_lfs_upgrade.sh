#!/bin/bash
# Builds test_lfs_upgrade on the real littlefs (see build_lfs_cost.sh).
set -e
cd "$(dirname "$0")"
LFS_TAG=${LFS_TAG:-v2.11.3}
if [ -z "$LFS_DIR" ]; then
  LFS_DIR=.littlefs
  if [ ! -f "$LFS_DIR/lfs.c" ]; then
    git clone -q --depth 1 -b "$LFS_TAG" https://github.com/littlefs-project/littlefs "$LFS_DIR"
  fi
fi
mkdir -p .lfs_build
python3 lfs/instrument_lfs.py "$LFS_DIR/lfs.c" .lfs_build/lfs.c
gcc -O2 -g -Wall -Wno-unused -I shims -I ../include -I ../src -I lfs -I "$LFS_DIR" \
  -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  test_lfs_upgrade.c lfs/lfs_stdio.c .lfs_build/lfs.c "$LFS_DIR/lfs_util.c" \
  ../src/tsdb_core.c ../src/tsdb_write.c ../src/tsdb_query.c ../src/tsdb_buffer.c \
  ../src/tsdb_legacy.c ../src/tsdb_segment.c ../src/tsdb_convert.c \
  -Wl,--wrap=fopen,--wrap=fileno,--wrap=fsync,--wrap=unlink,--wrap=rename,--wrap=stat,--wrap=mkdir,--wrap=rmdir \
  -Wl,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=truncate \
  -Wl,--wrap=malloc,--wrap=calloc \
  -o test_lfs_upgrade
