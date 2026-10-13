#!/bin/bash
# usage: build_common.sh <test_name>
set -e
cd "$(dirname "$0")"
gcc -g -O0 -Wall -Wno-unused -I shims -I ../include -I ../src \
  "$1.c" ../src/tsdb_core.c ../src/tsdb_write.c ../src/tsdb_query.c \
  ../src/tsdb_legacy.c ../src/tsdb_segment.c ../src/tsdb_buffer.c -o "$1"
