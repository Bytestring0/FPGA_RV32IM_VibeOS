#include "ag_asset_loader.h"
static uint32_t word(const unsigned char*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t crc(uint32_t c,const unsigned char*p,size_t n){
 static const uint32_t t[16]={0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
 while(n--){c^=*p++;c=(c>>4)^t[c&15];c=(c>>4)^t[c&15];}return c;
}
static int read_exact(const ag_asset_transport*t,unsigned char*p,size_t n){while(n--)if(t->receive(t->user,p++,10000u))return -2;return 0;}
static int ack(const ag_asset_transport*t,unsigned status,uint32_t offset){unsigned char a[5]={(unsigned char)status,(unsigned char)offset,(unsigned char)(offset>>8),(unsigned char)(offset>>16),(unsigned char)(offset>>24)};return t->send(t->user,a,5)?-2:0;}
int ag_asset_receive(const ag_asset_transport*t,void*destination,uint32_t bytes){
 if(!t||!t->receive||!t->send||!destination||!bytes)return -1;
 unsigned char header[12],packet[264],sum[4];uint32_t offset=0,whole=0xffffffffu;
 if(read_exact(t,header,sizeof header))return -2;
 if(word(header)!=0x31525341u||word(header+4)!=bytes)return -3;
 uint32_t expected_crc=word(header+8);if(ack(t,6,0))return -2;
 while(offset<bytes){
  if(read_exact(t,packet,8))return -2;
  uint32_t at=word(packet),length=(uint32_t)packet[4]|((uint32_t)packet[5]<<8);
  if(!length||length>256||packet[6]||packet[7])return -3;
  if(read_exact(t,packet+8,length)||read_exact(t,sum,4))return -2;
  uint32_t got=crc(0xffffffffu,packet,8+length)^0xffffffffu;
  if(got!=word(sum)){if(ack(t,0x15,offset))return -2;continue;}
  if(at<offset&&length<=offset-at){if(ack(t,6,offset))return -2;continue;}
  if(at!=offset||length>bytes-offset){if(ack(t,0x15,offset))return -2;continue;}
  unsigned char*out=(unsigned char*)destination+offset;
  for(uint32_t i=0;i<length;i++)out[i]=packet[8+i];
  whole=crc(whole,packet+8,length);offset+=length;
  if(ack(t,6,offset))return -2;
 }
 if((whole^0xffffffffu)!=expected_crc){(void)ack(t,0x15,offset);return -4;}
 return ack(t,4,offset);
}
