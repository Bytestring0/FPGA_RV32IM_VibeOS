#include "agfs.h"

#define SB_MAGIC_OFF        0u
#define SB_VERSION_OFF      8u
#define SB_FLAGS_OFF        9u
#define SB_FILE_COUNT_OFF   12u
#define SB_DIR_LBA_OFF      16u
#define SB_TOTAL_OFF        20u

#define DE_NAME_OFF         0u
#define DE_SIZE_OFF         24u
#define DE_START_LBA_OFF    28u
#define DE_FLAGS_OFF        32u

static uint8_t sector_buf[SD_SPI_SECTOR_SIZE];
static uint32_t cached_file_count;
static uint32_t cached_dir_lba;
static uint32_t cached_total_sectors;
static uint32_t cached_dir_sectors;   /* ceil(file_count / entries_per_sector) */
static uint32_t cached_data_start;    /* first LBA after the directory */
static int initialized;

static uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Case-sensitive, NUL-padded 24-byte name comparison. */
static int names_equal(const uint8_t *entry_name, const char *name) {
    uint8_t expected[AGFS_NAME_MAX];
    unsigned i;
    for (i = 0; i < AGFS_NAME_MAX; ++i) expected[i] = 0;
    for (i = 0; i < AGFS_NAME_MAX; ++i) {
        if (name[i] == 0) break;
        expected[i] = (uint8_t)name[i];
    }
    if (i == AGFS_NAME_MAX && name[AGFS_NAME_MAX] != 0) return 0;
    for (i = 0; i < AGFS_NAME_MAX; ++i)
        if (entry_name[i] != expected[i]) return 0;
    return 1;
}

/* Number of directory sectors for `file_count` entries, overflow-safe:
 * ceil(file_count / entries_per_sector) computed without the intermediate
 * (file_count + entries_per_sector - 1) that would wrap for a corrupt
 * file_count near UINT32_MAX. */
static uint32_t dir_sector_count(uint32_t file_count) {
    return file_count / AGFS_DIR_ENTRIES_PER_SECTOR +
           (file_count % AGFS_DIR_ENTRIES_PER_SECTOR != 0);
}

/* Validate a file's physical extent without 32-bit overflow. The file must
 * start after the superblock (LBA 0) and directory (cached_data_start) and its
 * last data sector (for a non-empty file) must still be inside the device.
 * This rejects corrupt start_lba/size pairs that would otherwise wrap around
 * and read e.g. the superblock at LBA 0, or overlap the directory. */
static int file_in_range(const agfs_file *file) {
    uint32_t last_sector;
    if (file->start_lba < cached_data_start) return 0;   /* superblock/directory */
    if (file->start_lba >= cached_total_sectors) return 0;
    if (file->size_bytes == 0) return 1;
    last_sector = (file->size_bytes - 1) / SD_SPI_SECTOR_SIZE;
    return last_sector < cached_total_sectors - file->start_lba;
}

agfs_result agfs_init(void) {
    const char *magic = AGFS_MAGIC;
    unsigned i;
    initialized = 0;
    if (sd_spi_read_sector(0, sector_buf) != SD_SPI_OK) return AGFS_IO;
    for (i = 0; i < 8; ++i)
        if (sector_buf[SB_MAGIC_OFF + i] != (uint8_t)magic[i]) return AGFS_BAD_MAGIC;
    if (sector_buf[SB_VERSION_OFF] != AGFS_FS_VERSION) return AGFS_VERSION;
    cached_file_count = read_le32(sector_buf + SB_FILE_COUNT_OFF);
    cached_dir_lba = read_le32(sector_buf + SB_DIR_LBA_OFF);
    cached_total_sectors = read_le32(sector_buf + SB_TOTAL_OFF);
    if (cached_total_sectors == 0) return AGFS_DIR;
    if (cached_dir_lba == 0 || cached_dir_lba >= cached_total_sectors) return AGFS_DIR;
    /* The whole directory must fit inside the device: dir_lba + dir_sectors
     * <= total_sectors. dir_sector_count() cannot overflow, so a corrupt
     * file_count (e.g. 0xffffffff) is rejected here instead of wrapping the
     * sector count to 0 and reading past the image. */
    cached_dir_sectors = dir_sector_count(cached_file_count);
    if (cached_dir_sectors > cached_total_sectors - cached_dir_lba) return AGFS_DIR;
    cached_data_start = cached_dir_lba + cached_dir_sectors;
    initialized = 1;
    return AGFS_OK;
}

agfs_result agfs_open(const char *name, agfs_file *out) {
    uint32_t remaining, s;
    if (!name || !out) return AGFS_RANGE;
    if (name[0] == 0) return AGFS_NOT_FOUND;
    if (!initialized) return AGFS_DIR;
    if (cached_file_count == 0) return AGFS_NOT_FOUND;
    remaining = cached_file_count;
    for (s = 0; s < cached_dir_sectors; ++s) {
        unsigned e;
        if (sd_spi_read_sector(cached_dir_lba + s, sector_buf) != SD_SPI_OK)
            return AGFS_IO;
        for (e = 0; e < AGFS_DIR_ENTRIES_PER_SECTOR && remaining > 0; ++e, --remaining) {
            const uint8_t *entry = sector_buf + e * AGFS_DIR_ENTRY_SIZE;
            if (names_equal(entry + DE_NAME_OFF, name)) {
                out->size_bytes = read_le32(entry + DE_SIZE_OFF);
                out->start_lba = read_le32(entry + DE_START_LBA_OFF);
                out->flags = read_le32(entry + DE_FLAGS_OFF);
                if (!file_in_range(out)) return AGFS_RANGE;
                return AGFS_OK;
            }
        }
    }
    return AGFS_NOT_FOUND;
}

agfs_result agfs_read(const agfs_file *file, uint32_t offset, uint8_t *buf, uint32_t len) {
    uint32_t copied = 0;
    if (!file || !buf) return AGFS_RANGE;
    if (len == 0) return AGFS_OK;
    if (!file_in_range(file)) return AGFS_RANGE;
    if (offset > file->size_bytes || len > file->size_bytes - offset) return AGFS_RANGE;
    while (copied < len) {
        uint32_t pos = offset + copied;
        uint32_t lba = file->start_lba + pos / SD_SPI_SECTOR_SIZE;
        uint32_t within = pos % SD_SPI_SECTOR_SIZE;
        uint32_t chunk = SD_SPI_SECTOR_SIZE - within;
        uint32_t i;
        if (chunk > len - copied) chunk = len - copied;
        if (sd_spi_read_sector(lba, sector_buf) != SD_SPI_OK) return AGFS_IO;
        for (i = 0; i < chunk; ++i) buf[copied + i] = sector_buf[within + i];
        copied += chunk;
    }
    return AGFS_OK;
}
