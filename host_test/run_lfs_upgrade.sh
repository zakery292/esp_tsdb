#!/bin/bash
# Upgrading firmware units that hold a full (or nearly full) 2.x database to
# 3.0, on the real littlefs with the firmware's partition and capacity
# formula — plus a long fresh run and the failure-reporting cases.
#
# The 2.x databases are built by the released sources themselves (v2.4.3 and
# v2.4.4, via git archive) on the host, then copied onto the littlefs image:
# a clean close, and an unclean shutdown (the 2.4.4 one leaves its newest
# state only in <db>.d/h0,h1). The same release then reads every record back
# (records.bin): the reference every check compares against. Geometries: the
# 16 MB build (3.375 MB LittleFS, ~400 KB of other files) and the 4 MB build
# (128 KB LittleFS).
#
#   upgrade  writes 1.3 x capacity with no tsdb_maintain() calls (the
#            conversion runs one step per write)
#   convert  the conversion driven by tsdb_maintain(): cost, lock holds, free
#            space, the exact history after every cut; with --crash-stride a
#            power cut before every (stride-th) mutating operation, and a
#            clean close after every (stride-th) step
#   legacy   clear under power cuts (no orphaned sidecars), out of memory at
#            open, an unreadable header (reconstruction in bounded steps, once)
#
#   ./run_lfs_upgrade.sh            all scenarios
#   QUICK=1 ./run_lfs_upgrade.sh    16 MB: only the 2.4.4 crash + fresh
#   STRIDE=n                        16 MB power-cut sweeps: every n-th operation (default 1 = all)
cd "$(dirname "$0")"
set -e
./build_lfs_upgrade.sh
GEN=.legacy_gen
mkdir -p "$GEN"
for ref in v2.4.3 v2.4.4; do
  if [ ! -x "$GEN/gen_$ref" ]; then
    rm -rf "$GEN/src_$ref" && mkdir -p "$GEN/src_$ref"
    git -C .. archive "$ref" src include | tar -x -C "$GEN/src_$ref"
    S="$GEN/src_$ref/src"
    gcc -O2 -w -I shims -I "$GEN/src_$ref/include" -I "$S" gen_legacy.c \
      "$S"/tsdb_core.c "$S"/tsdb_write.c "$S"/tsdb_index.c "$S"/tsdb_query.c \
      "$S"/tsdb_migrate.c "$S"/tsdb_buffer.c -Wl,--wrap=fsync -o "$GEN/gen_$ref"
  fi
done
set +e

# The firmware's capacity for a partition (src/tsdb_energy_logger.cpp).
cap() { local fs=$1 r=$((384*1024)) b=$((2500*1024)) fb
        if [ $fs -gt $r ]; then fb=$((fs - r)); else fb=$((fs / 4)); fi
        [ $b -gt $fb ] && b=$fb; echo $(( (b - 2048) / 18 )); }

fails=0
gen() {   # ref max n close|crash -> dir
  local d="$GEN/db_$1_$2_$3_$4"
  if [ ! -f "$d/records.bin" ]; then
    rm -rf "$d" && mkdir -p "$d"
    "$GEN/gen_$1" "$d" "$2" "$3" 1600000000 300 "$4" > "$d/expected" || return 1
  fi
  echo "$d"
}
run() { echo; echo "== $*"; ./test_lfs_upgrade "$@" | grep -v '^$'; [ ${PIPESTATUS[0]} -eq 0 ] || fails=$((fails+1)); }

BIG=$(cap $((864*4096)))
SMALL=$(cap $((32*4096)))
STRIDE=${STRIDE:-1}
if [ -n "$QUICK" ]; then
  run upgrade "$(gen v2.4.4 $BIG $((BIG + 60000)) crash)"
  run convert "$(gen v2.4.4 $BIG $((BIG + 60000)) crash)" --crash-stride 7
  run fresh
else
  for ref in v2.4.3 v2.4.4; do
    for how in close crash; do
      run upgrade "$(gen $ref $BIG $((BIG + 60000)) $how)"          # wrapped, full
      run convert "$(gen $ref $BIG $((BIG + 60000)) $how)" --crash-stride $STRIDE
    done
  done
  run upgrade "$(gen v2.4.4 $BIG 130000 crash)"                     # nearly full, not wrapped
  run convert "$(gen v2.4.4 $BIG 130000 crash)" --crash-stride $STRIDE
  run upgrade "$(gen v2.4.4 $BIG $((2 * BIG)) close)"               # wrapped exactly at slot 0
  run convert "$(gen v2.4.4 $BIG $((2 * BIG)) close)" --crash-stride $STRIDE
  run legacy "$(gen v2.4.4 $BIG $((BIG + 60000)) crash)"
  run fresh
  for ref in v2.4.3 v2.4.4; do
    for how in close crash; do
      run upgrade "$(gen $ref $SMALL $((SMALL + 700)) $how)" --fs-blocks 32
      run convert "$(gen $ref $SMALL $((SMALL + 700)) $how)" --fs-blocks 32 --crash-stride 1
      run legacy "$(gen $ref $SMALL $((SMALL + 700)) $how)" --fs-blocks 32
    done
  done
  run convert "$(gen v2.4.4 $SMALL 900 crash)" --fs-blocks 32 --crash-stride 1          # partly full
  run convert "$(gen v2.4.4 $SMALL $((2 * SMALL)) close)" --fs-blocks 32 --crash-stride 1 # slot 0
  run fresh --fs-blocks 32
  run faults
fi
echo
[ $fails -eq 0 ] && echo "ALL PASS" || echo "$fails FAILED"
exit $fails
