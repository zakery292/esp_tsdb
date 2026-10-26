#!/bin/bash
# Upgrade compatibility: databases written by the last 2.x releases (v2.4.3,
# v2.4.4), opened by this tree. "crash" = the old version exits with the
# database open, as a power cut would leave it: 2.4.3 keeps its newest state
# in <db>.h0/.h1, 2.4.4 in <db>.d/h0,h1 (with a <db>.h0/.h1 mirror up to 31
# records behind). Downgrading from 3.x is not supported and not tested.
cd "$(dirname "$0")"
./build_compat.sh || exit 1
DB=${COMPAT_DB:-/tmp/tsdb_compat_test.tsdb}
NEW=./test_compat_new
fails=0

clean() { rm -rf "$DB" "$DB.h0" "$DB.h1" "$DB.d" "$DB.mig" "$DB".s*; }
field() { "$1" "$DB" count | cut -d' ' -f"$2"; }
check() {   # label got want
  if [ "$2" = "$3" ]; then echo "  [PASS] $1: $2"; else echo "  [FAIL] $1: got $2, want $3"; fails=$((fails+1)); fi
}
leftovers() { ls -d "$DB" "$DB.h0" "$DB.h1" "$DB.d" 2>/dev/null | wc -l; }

for REF in ${COMPAT_REFS:-v2.4.3 v2.4.4}; do
  OLD=./test_compat_$REF
  echo "== from $REF"

  echo "S1 clean close"
  clean; $OLD "$DB" write 100 1000 close
  check "count" "$(field $NEW 1)" 100
  check "first..last" "$(field $NEW 2)..$(field $NEW 3)" "1000..1099"

  echo "S2 unclean shutdown, ring not wrapped (40 records: 2.4.4's mirror holds 33)"
  clean; $OLD "$DB" write 40 1000 crash
  check "count" "$(field $NEW 1)" 40
  check "last" "$(field $NEW 3)" 1039
  check "ordered + values" "$(field $NEW 4)" 1

  echo "S3 unclean shutdown across the wrap (350 into a ring of 300)"
  clean; $OLD "$DB" write 350 1000 crash
  check "count" "$(field $NEW 1)" 300
  check "first..last" "$(field $NEW 2)..$(field $NEW 3)" "1050..1349"
  check "ordered + values" "$(field $NEW 4)" 1

  echo "S4 3.x writes on, crashes, reopens"
  $NEW "$DB" write 50 1350 crash
  check "count (capacity 300)" "$(field $NEW 1)" 300
  check "first..last" "$(field $NEW 2)..$(field $NEW 3)" "1100..1399"
  check "ordered + values" "$(field $NEW 4)" 1

  echo "S5 once the window has passed the legacy records, its files are gone"
  $NEW "$DB" write 300 1400 close
  check "count" "$(field $NEW 1)" 300
  check "first..last" "$(field $NEW 2)..$(field $NEW 3)" "1400..1699"
  check "legacy file and sidecars (.h0 .h1 .d) left" "$(leftovers)" 0
done

clean
[ $fails -eq 0 ] && echo "ALL PASS" || echo "$fails FAILED"
exit $fails
