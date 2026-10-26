#pragma once
// Hook points inserted into an instrumented copy of littlefs lfs.c.
#include <stdint.h>
enum {
    LFS_H_COMMIT = 0,     // lfs_dir_relocatingcommit: any metadata commit
    LFS_H_COMPACT,        // lfs_dir_compact: metadata pair rewritten
    LFS_H_SPLIT,          // lfs_dir_split: metadata pair split in two
    LFS_H_FS_SIZE,        // lfs_fs_size_: full filesystem traversal
    LFS_H_ALLOC_SCAN,     // lfs_alloc_scan: lookahead refill (full traversal)
    LFS_H_COUNT
};
void lfs_harness_hook(int kind, const uint32_t *pair);
