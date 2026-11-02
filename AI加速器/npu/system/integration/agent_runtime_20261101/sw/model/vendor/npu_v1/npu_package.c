#include "npu_package.h"

/* Opt in with -DNPU_FAST_CRC=1; retain the original implementation by default. */
#ifndef NPU_FAST_CRC
#define NPU_FAST_CRC 0
#endif

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1]<<8));
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p+4)<<32);
}
static int within(uint64_t off, uint64_t len, uint64_t total) {
    return off<=total && len<=total-off;
}
static int overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    return a<b+bn && b<a+an; /* caller first bounds each interval to <=1 GiB */
}
static uint32_t crc_byte(uint32_t c, uint8_t b) {
#if NPU_FAST_CRC == 1
    /* 256 reflected remainders for polynomial 0xedb88320. */
    static const uint32_t table[256] = {
        0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
        0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
        0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
        0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
        0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
        0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
        0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
        0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
        0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
        0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
        0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
        0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
        0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
        0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
        0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
        0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
        0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
        0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
        0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
        0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
        0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
        0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
        0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
        0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
        0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
        0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
        0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
        0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
        0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
        0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
        0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
        0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
    };
    return (c >> 8) ^ table[(c ^ b) & 0xffu];
#else
    static const uint32_t tab[16]={
        0x00000000u,0x1db71064u,0x3b6e20c8u,0x26d930acu,
        0x76dc4190u,0x6b6b51f4u,0x4db26158u,0x5005713cu,
        0xedb88320u,0xf00f9344u,0xd6d6a3e8u,0xcb61b38cu,
        0x9b64c2b0u,0x86d3d2d4u,0xa00ae278u,0xbdbdf21cu
    };
    c^=b; c=(c>>4)^tab[c&15u]; return (c>>4)^tab[c&15u];
#endif
}
uint32_t npu_package_crc32(const void *data, size_t bytes) {
    const uint8_t *p=(const uint8_t *)data;
    uint32_t c=UINT32_C(0xffffffff);
    while (bytes--) c=crc_byte(c,*p++);
    return c^UINT32_C(0xffffffff);
}
static uint32_t image_crc(const uint8_t *p, uint32_t bytes) {
    uint32_t i,c=UINT32_C(0xffffffff);
    for (i=0;i<bytes;++i) c=crc_byte(c,(i>=48u && i<52u)?0u:p[i]);
    return c^UINT32_C(0xffffffff);
}
uint32_t npu_package_name_id(const char *name) {
    uint32_t h=UINT32_C(2166136261); unsigned char c;
    if (!name || !*name) return 0;
    while ((c=(unsigned char)*name++)!=0) { h^=c; h*=UINT32_C(16777619); }
    return h ? h : 1u;
}
int npu_package_tensor_at(const npu_package_view *v, uint32_t index, npu_package_tensor *t) {
    const uint8_t *p; uint64_t off;
    if (!v || !v->buffer || !t || index>=v->tensor_count || v->tensor_record_bytes!=64u) return NPU_PACKAGE_E_ARG;
    off=(uint64_t)v->tensor_table_offset+(uint64_t)index*64u;
    if (!within(off,64u,v->bytes)) return NPU_PACKAGE_E_RANGE;
    p=v->buffer+(uint32_t)off;
    t->name_id=get32(p); t->flags=get32(p+4); t->n=get32(p+8); t->k=get32(p+12);
    t->padded_k=get32(p+16); t->row_stride=get32(p+20);
    t->weight_offset=get64(p+24); t->weight_length=get64(p+32);
    t->scale_offset=get64(p+40); t->scale_length=get64(p+48); t->shared_const_id=get32(p+56);
    return NPU_PACKAGE_OK;
}
int npu_package_find(const npu_package_view *v,uint32_t id,npu_package_tensor *t) {
    uint32_t i; npu_package_tensor x; int rc;
    if (!v || !t || id==0u) return NPU_PACKAGE_E_ARG;
    for (i=0;i<v->tensor_count;++i) {
        rc=npu_package_tensor_at(v,i,&x); if (rc) return rc;
        if (x.name_id==id) { *t=x; return NPU_PACKAGE_OK; }
    }
    return NPU_PACKAGE_E_NOT_FOUND;
}
int npu_package_ddr_offset(const npu_package_view *v,uint32_t ddr_base,
                           uint64_t offset,uint64_t length,uint32_t *result) {
    uint64_t absolute=(uint64_t)ddr_base+offset;
    if (!v || !v->buffer || !result) return NPU_PACKAGE_E_ARG;
    if (!within(offset,length,v->bytes) || !within(ddr_base,v->bytes,NPU_PACKAGE_MAX_DDR_BYTES) ||
        !within(absolute,length,NPU_PACKAGE_MAX_DDR_BYTES)) return NPU_PACKAGE_E_RANGE;
    *result=(uint32_t)absolute; return NPU_PACKAGE_OK;
}
static int validate_tensor(const npu_package_tensor *t,const npu_package_view *v) {
    uint64_t groups,need_w,need_s; uint64_t i;
    if (!t->name_id || t->flags || !t->n || !t->k || t->k>4096u) return NPU_PACKAGE_E_FORMAT;
    groups=(t->k+127u)/128u;
    if (t->padded_k!=groups*128u || t->row_stride<t->padded_k/2u || (t->row_stride&31u)) return NPU_PACKAGE_E_FORMAT;
    need_w=(uint64_t)t->n*t->row_stride; need_s=(uint64_t)t->n*groups*2u;
    if (t->weight_length!=need_w || t->scale_length!=need_s) return NPU_PACKAGE_E_FORMAT;
    if ((t->weight_offset&511u) || (t->scale_offset&511u) ||
        t->weight_offset<v->payload_offset || t->scale_offset<v->payload_offset ||
        !within(t->weight_offset,t->weight_length,v->bytes) ||
        !within(t->scale_offset,t->scale_length,v->bytes)) return NPU_PACKAGE_E_RANGE;
    if (overlap(t->weight_offset,t->weight_length,t->scale_offset,t->scale_length)) return NPU_PACKAGE_E_FORMAT;
    for (i=0;i<t->scale_length;i+=2u) {
        uint16_t bits=get16(v->buffer+(uint32_t)(t->scale_offset+i));
        if ((bits&0x8000u) || !(bits&0x7fffu) || (bits&0x7c00u)==0x7c00u) return NPU_PACKAGE_E_FORMAT;
    }
    return NPU_PACKAGE_OK;
}
int npu_package_load(npu_package_read_blocks read_blocks,void *ctx,uint32_t start_lba,
                     void *storage,uint32_t capacity,npu_package_view *v) {
    static const uint8_t magic[8]={'A','G','N','P','U','P','K',0};
    uint8_t *p=(uint8_t *)storage; uint32_t i,j,sectors,count; uint64_t total,table,payload,table_end;
    npu_package_view tmp; npu_package_tensor t,u; int rc;
    if (!v) return NPU_PACKAGE_E_ARG;
    *v=(npu_package_view){0};
    if (!read_blocks || !p || capacity> NPU_PACKAGE_MAX_DDR_BYTES || capacity<512u) return NPU_PACKAGE_E_ARG;
    if (read_blocks(ctx,start_lba,1u,p)) return NPU_PACKAGE_E_IO;
    if (p[0]=='G' && p[1]=='G' && p[2]=='U' && p[3]=='F') return NPU_PACKAGE_E_GGUF;
    for (i=0;i<8u;++i) if (p[i]!=magic[i]) return NPU_PACKAGE_E_MAGIC;
    if (get16(p+8)!=1u || get16(p+10)!=512u) return NPU_PACKAGE_E_VERSION;
    count=get32(p+16); table=get64(p+24); payload=get64(p+32); total=get64(p+40);
    if (get32(p+12) || !count || count>4096u || get32(p+20)!=64u ||
        get32(p+52)!=1u || get32(p+56)!=0x01020304u || get32(p+60)) return NPU_PACKAGE_E_FORMAT;
    for (i=64u;i<512u;++i) if (p[i]) return NPU_PACKAGE_E_FORMAT;
    if (total>capacity || total>NPU_PACKAGE_MAX_DDR_BYTES) return NPU_PACKAGE_E_CAPACITY;
    table_end=table+(uint64_t)count*64u;
    if (table!=512u || table_end>total || payload< table_end || payload>total ||
        (payload&511u) || (total&511u) || total<512u ||
        payload!=((table_end+511u)&~UINT64_C(511))) return NPU_PACKAGE_E_RANGE;
    sectors=(uint32_t)(total/512u);
    if ((uint64_t)start_lba+sectors>UINT64_C(0x100000000)) return NPU_PACKAGE_E_RANGE;
    for (i=1u;i<sectors;) {
        uint32_t n=sectors-i; if (n>128u) n=128u;
        if (read_blocks(ctx,start_lba+i,n,p+i*512u)) return NPU_PACKAGE_E_IO;
        i+=n;
    }
    if (image_crc(p,(uint32_t)total)!=get32(p+48)) return NPU_PACKAGE_E_CRC;
    tmp=(npu_package_view){p,(uint32_t)total,count,(uint32_t)table,64u,(uint32_t)payload};
    for (i=0;i<count;++i) {
        rc=npu_package_tensor_at(&tmp,i,&t); if (rc) return rc;
        if (get32(p+(uint32_t)table+i*64u+60u)) return NPU_PACKAGE_E_FORMAT;
        rc=validate_tensor(&t,&tmp); if (rc) return rc;
        for (j=0;j<i;++j) {
            rc=npu_package_tensor_at(&tmp,j,&u); if (rc) return rc;
            if (t.name_id==u.name_id ||
                overlap(t.weight_offset,t.weight_length,u.weight_offset,u.weight_length) ||
                overlap(t.scale_offset,t.scale_length,u.scale_offset,u.scale_length) ||
                overlap(t.weight_offset,t.weight_length,u.scale_offset,u.scale_length) ||
                overlap(t.scale_offset,t.scale_length,u.weight_offset,u.weight_length)) return NPU_PACKAGE_E_FORMAT;
        }
    }
    *v=tmp; return NPU_PACKAGE_OK;
}
