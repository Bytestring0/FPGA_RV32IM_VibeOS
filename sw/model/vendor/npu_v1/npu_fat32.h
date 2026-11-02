#ifndef AG_NPU_FAT32_H
#define AG_NPU_FAT32_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NPU_FAT32_SECTOR_BYTES 512u
/* Each callback must fill count * 512 bytes or return nonzero. No short success.
 * All I/O is read-only; this implementation requests one sector at a time. */
typedef int (*npu_fat32_read_blocks)(void *ctx, uint32_t lba,
                                    uint32_t count, void *dst);
enum {
    NPU_FAT32_OK = 0, NPU_FAT32_E_ARG = -1, NPU_FAT32_E_IO = -2,
    NPU_FAT32_E_FORMAT = -3, NPU_FAT32_E_RANGE = -4,
    NPU_FAT32_E_NOT_FOUND = -5, NPU_FAT32_E_CHAIN = -6
};
/* Caller-owned, no heap, non-reentrant. Scratch buffers are uint32_t-aligned for
 * existing SD drivers; caller/destination alignment is otherwise unrestricted.
 * Do not alter these fields after mount or remount while a file is open. */
typedef struct {
    npu_fat32_read_blocks read_blocks;
    void *ctx;
    uint64_t capacity_sectors;
    uint32_t volume_lba, total_sectors, fat_lba, fat_sectors;
    uint32_t data_lba, cluster_count, root_cluster;
    uint32_t sectors_per_cluster, mounted;
    uint32_t scratch[256]; /* exactly 1024 bytes: data sector + FAT sector */
} npu_fat32_fs;
typedef struct {
    npu_fat32_fs *fs;
    uint64_t size_bytes; /* immutable directory size snapshot */
    uint32_t first_cluster;
    uint32_t cluster_length;
} npu_fat32_file;
/* capacity_sectors=0 means unknown; BPB/partition supply the logical bounds.
 * Supports superfloppy or one of four MBR primary FAT32 0x0b/0x0c partitions.
 * 512-byte sectors only; FAT32 cluster_count must be >=65525. */
int npu_fat32_mount(npu_fat32_fs *fs, npu_fat32_read_blocks read_blocks,
                    void *ctx, uint64_t capacity_sectors);
/* name83 is eleven uppercase, space-padded DOS name bytes (no dot).
 * Files only, ordinary root directory, no subdirectories or LFN matching.
 * Open validates the ENTIRE chain, so loops/early EOF fail before publication. */
int npu_fat32_open(npu_fat32_fs *fs, const char name83[11], npu_fat32_file *file);
int npu_fat32_open_params(npu_fat32_fs *fs, npu_fat32_file *file);
/* Signature matches npu_storage_read_exact_at. Assign ctx=&file and
 * source.size_bytes=file.size_bytes. Arbitrary offsets and unaligned dst work.
 * Failure may leave partial/untrusted bytes; never consume those as a package. */
int npu_fat32_read_exact_at(void *file, uint64_t offset, void *dst, size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
