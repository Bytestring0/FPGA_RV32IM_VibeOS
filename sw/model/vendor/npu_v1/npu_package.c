#include "npu_package.h"

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
    static const uint32_t tab[16]={
        0x00000000u,0x1db71064u,0x3b6e20c8u,0x26d930acu,
        0x76dc4190u,0x6b6b51f4u,0x4db26158u,0x5005713cu,
        0xedb88320u,0xf00f9344u,0xd6d6a3e8u,0xcb61b38cu,
        0x9b64c2b0u,0x86d3d2d4u,0xa00ae278u,0xbdbdf21cu
    };
    c^=b; c=(c>>4)^tab[c&15u]; return (c>>4)^tab[c&15u];
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
