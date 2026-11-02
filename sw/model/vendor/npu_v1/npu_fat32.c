#include "npu_fat32.h"
#include <string.h>

#define FAT_EOC UINT32_C(0x0ffffff8)
#define FAT_BAD UINT32_C(0x0ffffff7)
#define FAT_RESERVED UINT32_C(0x0ffffff0)
#define FS_READY UINT32_C(0x46415432)

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int read_sector(npu_fat32_fs *fs, uint64_t lba, void *dst)
{
    if (lba > UINT32_MAX || (fs->capacity_sectors && lba >= fs->capacity_sectors))
        return NPU_FAT32_E_RANGE;
    if (fs->mounted == FS_READY &&
        (lba < fs->volume_lba || lba - fs->volume_lba >= fs->total_sectors))
        return NPU_FAT32_E_RANGE;
    return fs->read_blocks(fs->ctx, (uint32_t)lba, 1u, dst) ?
           NPU_FAT32_E_IO : NPU_FAT32_OK;
}
static int valid_cluster(const npu_fat32_fs *fs, uint32_t c)
{
    return c >= 2u && c < FAT_RESERVED && (uint64_t)c < (uint64_t)fs->cluster_count + 2u;
}
/* next=0 denotes EOC, never a free/reserved/bad cluster. */
static int next_cluster(npu_fat32_fs *fs, uint32_t c, uint32_t *next)
{
    uint64_t byte = (uint64_t)c * 4u;
    uint8_t *fat = (uint8_t *)&fs->scratch[128];
    uint32_t value;
    int rc;
    if (!valid_cluster(fs, c) || byte / 512u >= fs->fat_sectors)
        return NPU_FAT32_E_CHAIN;
    rc = read_sector(fs, (uint64_t)fs->fat_lba + byte / 512u, fat);
    if (rc) return rc;
    value = le32(fat + (size_t)(byte % 512u)) & UINT32_C(0x0fffffff);
    if (value >= FAT_EOC) { *next = 0; return 0; }
    if (value == FAT_BAD || !valid_cluster(fs, value)) return NPU_FAT32_E_CHAIN;
    *next = value;
    return 0;
}
/* Brent cycle detector: constant memory and bounded by the volume geometry. */
static int chain_length(npu_fat32_fs *fs, uint32_t first, uint32_t *length)
{
    uint32_t current = first, anchor = first, power = 1u, span = 0u, n = 0u;
    int rc;
    if (!valid_cluster(fs, first)) return NPU_FAT32_E_CHAIN;
    while (current) {
        if (n >= fs->cluster_count) return NPU_FAT32_E_CHAIN;
        ++n;
        rc = next_cluster(fs, current, &current);
        if (rc) return rc;
        if (!current) break;
        ++span;
        if (current == anchor) return NPU_FAT32_E_CHAIN;
        if (span == power) {
            anchor = current;
            span = 0;
            if (power <= UINT32_MAX / 2u) power *= 2u;
        }
    }
    *length = n;
    return 0;
}
static int parse_bpb(npu_fat32_fs *fs, uint32_t start, uint32_t partition_sectors)
{
    const uint8_t *b = (const uint8_t *)fs->scratch;
    uint32_t spc = b[13], reserved = le16(b + 14), fats = b[16];
    uint32_t total = le32(b + 32), fat_size = le32(b + 36);
    uint32_t root = le32(b + 44), flags = le16(b + 40), active = 0;
    uint32_t info = le16(b + 48), backup = le16(b + 50);
    uint64_t overhead, clusters, end = (uint64_t)start + total;
    if (b[510] != 0x55 || b[511] != 0xaa ||
        le16(b + 11) != 512u || !spc || spc > 128u || (spc & (spc - 1u)) ||
        !reserved || !fats || fats > 2u || le16(b + 17) || le16(b + 19) ||
        le16(b + 22) || !total || !fat_size || le16(b + 42) || b[21] < 0xf0u ||
        (flags & 0xff70u) ||
        (info != 0xffffu && (!info || info >= reserved)) ||
        (backup != 0xffffu && backup && backup >= reserved))
        return NPU_FAT32_E_FORMAT;
    if (flags & 0x80u) active = flags & 0x0fu;
    else if (flags & 0x0fu) return NPU_FAT32_E_FORMAT;
    if (active >= fats) return NPU_FAT32_E_FORMAT;
    if (end > (UINT64_C(1) << 32) ||
        (partition_sectors && total > partition_sectors) ||
        (fs->capacity_sectors && end > fs->capacity_sectors)) return NPU_FAT32_E_RANGE;
    overhead = (uint64_t)reserved + (uint64_t)fats * fat_size;
    if (overhead >= total) return NPU_FAT32_E_FORMAT;
    clusters = ((uint64_t)total - overhead) / spc;
    if (clusters < 65525u || clusters + 2u > FAT_RESERVED ||
        (uint64_t)fat_size * 128u < clusters + 2u) return NPU_FAT32_E_FORMAT;
    fs->volume_lba = start;
    fs->total_sectors = total;
    fs->fat_lba = (uint32_t)((uint64_t)start + reserved + (uint64_t)active * fat_size);
    fs->fat_sectors = fat_size;
    fs->data_lba = (uint32_t)((uint64_t)start + overhead);
    fs->cluster_count = (uint32_t)clusters;
    fs->root_cluster = root;
    fs->sectors_per_cluster = spc;
    if (!valid_cluster(fs, root)) return NPU_FAT32_E_FORMAT;
    fs->mounted = FS_READY;
    return 0;
}
int npu_fat32_mount(npu_fat32_fs *fs, npu_fat32_read_blocks read_blocks,
                    void *ctx, uint64_t capacity_sectors)
{
    uint8_t *b;
    uint32_t i, starts[4],lengths[4];
    uint8_t types[4],flags[4];
    int rc,last_error=NPU_FAT32_E_FORMAT;
    if (!fs) return NPU_FAT32_E_ARG;
    memset(fs, 0, sizeof(*fs));
    if (!read_blocks || capacity_sectors > (UINT64_C(1) << 32)) return NPU_FAT32_E_ARG;
    fs->read_blocks = read_blocks;
    fs->ctx = ctx;
    fs->capacity_sectors = capacity_sectors;
    b = (uint8_t *)fs->scratch;
    rc = read_sector(fs, 0, b);
    if (rc) return rc;
    if (b[510] != 0x55 || b[511] != 0xaa) return NPU_FAT32_E_FORMAT;
    if ((b[0] == 0xeb || b[0] == 0xe9) && le16(b + 11) == 512u) {
        rc=parse_bpb(fs,0,0);
        if(!rc)return 0;
        fs->mounted=0;
        rc=read_sector(fs,0,b);
        if(rc)return rc;
    }
    for (i = 0; i < 4u; ++i) {
        const uint8_t *p=b+446u+i*16u;
        types[i]=p[4];flags[i]=p[0];starts[i]=le32(p+8);lengths[i]=le32(p+12);
    }
    for(i=0;i<4u;i++){
        if(types[i]!=0x0bu&&types[i]!=0x0cu)continue;
        if(flags[i]!=0&&flags[i]!=0x80u){last_error=NPU_FAT32_E_FORMAT;continue;}
        if(!starts[i]||!lengths[i]||(uint64_t)starts[i]+lengths[i]>(UINT64_C(1)<<32)||
           (capacity_sectors&&(uint64_t)starts[i]+lengths[i]>capacity_sectors)){
            last_error=NPU_FAT32_E_RANGE;continue;
        }
        fs->mounted=0;
        rc=read_sector(fs,starts[i],b);
        if(rc){last_error=rc;continue;}
        rc=parse_bpb(fs,starts[i],lengths[i]);
        if(!rc)return 0;
        fs->mounted=0;last_error=rc;
    }
    return last_error;
}
static uint64_t cluster_lba(const npu_fat32_fs *fs, uint32_t cluster)
{
    return (uint64_t)fs->data_lba + (uint64_t)(cluster - 2u) * fs->sectors_per_cluster;
}
int npu_fat32_open(npu_fat32_fs *fs, const char name83[11], npu_fat32_file *file)
{
    uint32_t current, root_length, visited, s;
    uint8_t *b;
    int rc;
    if (!file) return NPU_FAT32_E_ARG;
    memset(file, 0, sizeof(*file));
    if (!fs || fs->mounted != FS_READY || !name83) return NPU_FAT32_E_ARG;
    rc = chain_length(fs, fs->root_cluster, &root_length);
    if (rc) return rc;
    current = fs->root_cluster;
    b = (uint8_t *)fs->scratch;
    for (visited = 0; visited < root_length; ++visited) {
        if (!valid_cluster(fs, current)) return NPU_FAT32_E_CHAIN;
        for (s = 0; s < fs->sectors_per_cluster; ++s) {
            uint32_t off;
            rc = read_sector(fs, cluster_lba(fs, current) + s, b);
            if (rc) return rc;
            for (off = 0; off < 512u; off += 32u) {
                const uint8_t *e = b + off;
                uint32_t first, size, length;
                uint64_t expected;
                if (e[0] == 0) return NPU_FAT32_E_NOT_FOUND;
                if (e[0] == 0xe5 || (e[11] & 0x18u) || e[11] == 0x0fu) continue;
                if (memcmp(e, name83, 11u)) continue;
                if (e[11] & 0xc0u) return NPU_FAT32_E_FORMAT;
                first = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
                size = le32(e + 28);
                expected = ((uint64_t)size + (uint64_t)fs->sectors_per_cluster * 512u - 1u) /
                           ((uint64_t)fs->sectors_per_cluster * 512u);
                if (!size) {
                    if (first) return NPU_FAT32_E_CHAIN;
                    length = 0;
                } else {
                    rc = chain_length(fs, first, &length);
                    if (rc) return rc;
                    if ((uint64_t)length != expected) return NPU_FAT32_E_CHAIN;
                }
                file->fs = fs;
                file->first_cluster = first;
                file->size_bytes = size;
                file->cluster_length = length;
                return 0;
            }
        }
        rc = next_cluster(fs, current, &current);
        if (rc) return rc;
    }
    return NPU_FAT32_E_NOT_FOUND;
}
int npu_fat32_open_params(npu_fat32_fs *fs, npu_fat32_file *file)
{
    return npu_fat32_open(fs, "PARAMS  BIN", file);
}
int npu_fat32_read_exact_at(void *ctx, uint64_t offset, void *dst, size_t bytes)
{
    npu_fat32_file *file = (npu_fat32_file *)ctx;
    npu_fat32_fs *fs;
    uint32_t current, length, i;
    uint64_t cluster_bytes, skip;
    uint8_t *out = (uint8_t *)dst;
    int rc;
    if (!file || !file->fs || (!dst && bytes)) return NPU_FAT32_E_ARG;
    fs = file->fs;
    if (fs->mounted != FS_READY) return NPU_FAT32_E_ARG;
    if (offset > file->size_bytes || (uint64_t)bytes > file->size_bytes - offset ||
        (bytes && (uintptr_t)dst > UINTPTR_MAX - (bytes - 1u))) return NPU_FAT32_E_RANGE;
    if (!bytes) return 0;
    rc = chain_length(fs, file->first_cluster, &length);
    if (rc) return rc;
    if (length != file->cluster_length) return NPU_FAT32_E_CHAIN;
    cluster_bytes = (uint64_t)fs->sectors_per_cluster * 512u;
    skip = offset / cluster_bytes;
    if (skip >= length) return NPU_FAT32_E_CHAIN;
    current = file->first_cluster;
    for (i = 0; (uint64_t)i < skip; ++i) {
        rc = next_cluster(fs, current, &current);
        if (rc) return rc;
        if (!current) return NPU_FAT32_E_CHAIN;
    }
    offset %= cluster_bytes;
    while (bytes) {
        uint64_t sector = offset / 512u;
        size_t within = (size_t)(offset % 512u);
        size_t take = 512u - within;
        if (!valid_cluster(fs, current)) return NPU_FAT32_E_CHAIN;
        if (take > bytes) take = bytes;
        rc = read_sector(fs, cluster_lba(fs, current) + sector, fs->scratch);
        if (rc) return rc;
        memcpy(out, (const uint8_t *)fs->scratch + within, take);
        bytes -= take;
        out += take;
        offset += take;
        if (bytes && offset == cluster_bytes) {
            rc = next_cluster(fs, current, &current);
            if (rc) return rc;
            if (!current) return NPU_FAT32_E_CHAIN;
            offset = 0;
        }
    }
    return 0;
}
