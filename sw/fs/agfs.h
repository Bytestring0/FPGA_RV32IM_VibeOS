#ifndef AGFS_H
#define AGFS_H

/*
 * AGFS v1 -- a minimal read-only filesystem layered on the SD SPI block
 * device (sw/drivers/sd_spi.h). It is intentionally NOT FAT/FAT32 and does
 * not do wear leveling, journals, directories-in-directories, or writes.
 *
 * On-disk layout (little-endian, 512-byte sectors):
 *   LBA 0         superblock (see below)
 *   dir_start_lba .. +ceil(file_count/8)-1   directory, 64-byte entries
 *   per-file      contiguous data sectors [start_lba, start_lba+ceil(size/512))
 *
 * Superblock (512 bytes):
 *   +0  "AGFSv001" (8 ASCII bytes)
 *   +8  version u8 == 1
 *   +9  flags u8 (0 = read-only)
 *   +10 reserved u16
 *   +12 file_count u32 LE
 *   +16 dir_start_lba u32 LE
 *   +20 total_sectors u32 LE
 *   +24 reserved 8 bytes
 *   +32 padding to 512 (zero)
 *
 * Directory entry (64 bytes):
 *   +0  name[24] ASCII, NUL padded
 *   +24 size_bytes u32 LE
 *   +28 start_lba u32 LE
 *   +32 flags u32 LE (bit0 AGFS_F_CONTIGUOUS)
 *   +36 reserved 28 bytes (zero)
 */

#include "../platform.h"
#include "../drivers/sd_spi.h"

#define AGFS_MAGIC "AGFSv001"
#define AGFS_FS_VERSION 1u
#define AGFS_NAME_MAX 24u
#define AGFS_DIR_ENTRY_SIZE 64u
#define AGFS_DIR_ENTRIES_PER_SECTOR (SD_SPI_SECTOR_SIZE / AGFS_DIR_ENTRY_SIZE)
#define AGFS_F_CONTIGUOUS 0x00000001u

typedef enum {
    AGFS_OK = 0,
    AGFS_IO = -1,        /* underlying SD sector read failed */
    AGFS_BAD_MAGIC = -2,
    AGFS_VERSION = -3,
    AGFS_NOT_FOUND = -4,
    AGFS_RANGE = -5,
    AGFS_DIR = -6        /* malformed superblock/directory */
} agfs_result;

typedef struct {
    uint32_t size_bytes;
    uint32_t start_lba;
    uint32_t flags;
} agfs_file;

/* Validate the superblock and cache the directory location and file count. */
agfs_result agfs_init(void);

/* Locate a file by name and return its size/location. */
agfs_result agfs_open(const char *name, agfs_file *out);

/* Read [offset, offset+len) from a file into buf, spanning sectors as needed. */
agfs_result agfs_read(const agfs_file *file, uint32_t offset, uint8_t *buf, uint32_t len);

#endif
