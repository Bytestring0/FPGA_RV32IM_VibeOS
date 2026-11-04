#include "platform.h"
#include "ag_agent.h"
#include "npu_runtime.h"
#ifdef AGENT_UART_BOOT
#include "ag_asset_loader.h"
static int byte_rx(void*u,unsigned char*out,uint32_t timeout){(void)u;(void)timeout;for(unsigned n=0;n<1000000;n++){unsigned s=MMIO32(0x10001244u);if(s&6u)return -1;if(s&1u){*out=(unsigned char)MMIO32(0x10001240u);return 0;}}return -1;}
static int byte_tx(void*u,const unsigned char*p,size_t n){(void)u;while(n--)putchar_uart((char)*p++);return 0;}
#endif
extern unsigned char __agent_resource_start[],__agent_resource_end[];
extern const unsigned char agent_boot_payload_start[],agent_boot_payload_end[];
static int same(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
int main(void){
 while(!(MMIO32(0x10000200u)&1u)){}
 size_t bytes=(size_t)(__agent_resource_end-__agent_resource_start);
 if(bytes!=(size_t)(agent_boot_payload_end-agent_boot_payload_start)||!bytes)return 1;
#ifdef AGENT_UART_BOOT
 puts_uart("UART_ASSET_BOOT_READY\n");
 ag_asset_transport transport={0,byte_rx,byte_tx};if(ag_asset_receive(&transport,__agent_resource_start,(uint32_t)bytes))return 7;
#else
 for(size_t i=0;i<bytes;i++)__agent_resource_start[i]=agent_boot_payload_start[i];
#endif
 if(ag_cache_maintain_range((uint32_t)(uintptr_t)__agent_resource_start-0x80000000u,(uint32_t)bytes,1000000))return 2;
 __asm__ volatile("fence iorw,iorw; fence.i" ::: "memory");
 ag_runtime*rt=(ag_runtime*)(uintptr_t)0x80020000u;ag_receipt receipt;
 ag_init(rt,0,0);
 if(ag_execute(rt,"boot-calc","{\"name\":\"calculate\",\"arguments\":{\"expression\":\"(12+8)*3\"}}",0,&receipt)!=AG_OK)return 3;
 if(!same(receipt.result,"60"))return 4;
 if(ag_execute(rt,"boot-query","{\"name\":\"query_state\",\"arguments\":{}}",1,&receipt)!=AG_OK)return 5;
 if(!same(receipt.result,"notes=0;timer=0;remaining_ms=0"))return 6;
 puts_uart("AGENT_DDR_BOOT_PASS copied_resource clean_invalidate fence_i real_calculate=60 query_state\n");return 0;
}
