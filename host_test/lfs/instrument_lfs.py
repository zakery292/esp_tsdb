#!/usr/bin/env python3
"""Copy littlefs lfs.c, inserting harness hooks at the entry of the functions
whose cost we want to attribute (commit, compaction, split, full-FS traversals).

Usage: instrument_lfs.py <lfs.c in> <lfs.c out>

Hook points are matched by function signature, so this is tied to littlefs
v2.11.x. It fails loudly if a signature is not found exactly once.
"""
import re
import sys

HOOKS = [
    # (regex for the definition's first line, hook expression)
    (r"^static int lfs_dir_relocatingcommit\(lfs_t \*lfs, lfs_mdir_t \*dir,$",
     "lfs_harness_hook(LFS_H_COMMIT, dir->pair);"),
    (r"^static int lfs_dir_compact\(lfs_t \*lfs,$",
     "lfs_harness_hook(LFS_H_COMPACT, dir->pair);"),
    (r"^static int lfs_dir_split\(lfs_t \*lfs,$",
     "lfs_harness_hook(LFS_H_SPLIT, dir->pair);"),
    (r"^static lfs_ssize_t lfs_fs_size_\(lfs_t \*lfs\) \{$",
     "lfs_harness_hook(LFS_H_FS_SIZE, NULL);"),
    (r"^static int lfs_alloc_scan\(lfs_t \*lfs\) \{$",
     "lfs_harness_hook(LFS_H_ALLOC_SCAN, NULL);"),
]

PRELUDE = """/* instrumented by host_test/lfs/instrument_lfs.py — do not edit */
#include "lfs_harness_hooks.h"
"""


def main():
    src, dst = sys.argv[1], sys.argv[2]
    lines = open(src).read().split("\n")
    out = []
    pending = None
    found = {i: 0 for i in range(len(HOOKS))}
    for line in lines:
        out.append(line)
        if pending is None:
            for i, (pat, _) in enumerate(HOOKS):
                if re.match(pat, line):
                    pending = i
                    break
        if pending is not None and line.rstrip().endswith(";"):
            pending = None  # a prototype, not the definition
        if pending is not None and line.rstrip().endswith("{"):
            out.append("    " + HOOKS[pending][1])
            found[pending] += 1
            pending = None
    for i, n in found.items():
        if n != 1:
            sys.exit("instrument_lfs: %r matched %d times (expected 1)"
                     % (HOOKS[i][0], n))
    open(dst, "w").write(PRELUDE + "\n".join(out))


if __name__ == "__main__":
    main()
