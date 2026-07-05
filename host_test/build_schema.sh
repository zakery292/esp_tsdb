#!/bin/bash
set -e
gcc -g -O0 -Wall -Wno-unused -I shims -I ../include -I ../src \
  test_schema.c ../src/tsdb_core.c ../src/tsdb_write.c ../src/tsdb_index.c \
  ../src/tsdb_query.c ../src/tsdb_migrate.c ../src/tsdb_buffer.c -o test_schema
