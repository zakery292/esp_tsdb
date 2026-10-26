#!/bin/bash
# Cross-version compatibility: this tree ("new") against an older release
# ("old", default v2.4.3), alternating on one database. "crash" = the process
# exits with the database open, as a power cut would leave it.
cd "$(dirname "$0")"
./build_compat.sh || exit 1
DB=${COMPAT_DB:-/tmp/tsdb_compat_test.tsdb}
NEW=./test_compat_new
OLD=./test_compat_old
fails=0

clean() { rm -rf "$DB" "$DB.h0" "$DB.h1" "$DB.d" "$1" "$1.h0" "$1.h1" "$1.d"; }
count() { "$1" "$DB" count | cut -d' ' -f1; }
last()  { "$1" "$DB" count | cut -d' ' -f3; }
ordered() { "$1" "$DB" count | cut -d' ' -f4; }
check() {   # label got want
  if [ "$2" = "$3" ]; then echo "  [PASS] $1: $2"; else echo "  [FAIL] $1: got $2, want $3"; fails=$((fails+1)); fi
}
next_ts() { echo $(( $(last "$1") + 1 )); }

echo "S1 upgrade after a clean close"
clean; $OLD "$DB" write 100 1000 close
check "new reads old's clean DB" "$(count $NEW)" 100

echo "S2 upgrade after an unclean shutdown (old's sidecar only)"
clean; $OLD "$DB" write 100 1000 crash
check "new adopts old's sidecar" "$(count $NEW)" 100
$NEW "$DB" write 10 $(next_ts $NEW) crash
check "new after its own crash" "$(count $NEW)" 110

echo "S3 downgrade after an unclean shutdown"
# The mirror was written on new's first write after open: old sees 101 of 110.
check "old sees new's mirror" "$(count $OLD)" 101
check "old view ordered" "$(ordered $OLD)" 1

echo "S4 ...old writes on, crashes, then upgrade again"
$OLD "$DB" write 5 $(next_ts $OLD) crash
check "old after its own crash" "$(count $OLD)" 106
check "new prefers old's newer slot" "$(count $NEW)" 106
check "new view ordered" "$(ordered $NEW)" 1
$NEW "$DB" write 1 $(next_ts $NEW) crash
check "new continues" "$(count $NEW)" 107

echo "S5 old closes cleanly while new's subdirectory slots remain"
clean; $NEW "$DB" write 40 1000 crash            # mirror after writes 1 and 33
check "old sees mirror" "$(count $OLD)" 33
$OLD "$DB" write 3 $(next_ts $OLD) close        # removes only <db>.h0/.h1
check "new trusts old's clean close, not stale slots" "$(count $NEW)" 36
check "new view ordered" "$(ordered $NEW)" 1

echo "S6 clean close produces the same file as the old version"
clean "$DB.old"
$OLD "$DB.old" write 350 1000 close
$NEW "$DB" write 350 1000 close
if cmp -s "$DB" "$DB.old"; then echo "  [PASS] byte-identical after 350 writes (ring of 300, wrapped)"; else echo "  [FAIL] files differ"; cmp "$DB" "$DB.old" | head -3; fails=$((fails+1)); fi
check "no slots left" "$(ls -d "$DB".h0 "$DB".h1 "$DB".d 2>/dev/null | wc -l)" 0
check "old reads new's clean DB" "$(count $OLD)" 300

echo "S7 new across the ring wrap with a crash"
clean; $NEW "$DB" write 350 1000 crash
check "new after crash, wrapped" "$(count $NEW)" 300
check "ordered" "$(ordered $NEW)" 1
# Old adopts the mirror (written at record 321): it opens, and sees all 300
# slots. The up-to-31 records new wrote after the mirror sit in slots old
# believes hold the OLDEST records, so its scan is out of order there until
# old overwrites them — the bounded cost of a downgrade after a power cut.
check "old after new crashed, wrapped" "$(count $OLD)" 300

clean "$DB.old"
[ $fails -eq 0 ] && echo "ALL PASS" || echo "$fails FAILED"
exit $fails
