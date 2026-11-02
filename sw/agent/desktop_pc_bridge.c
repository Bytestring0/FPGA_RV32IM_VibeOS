#include "desktop_pc_bridge.h"
volatile desktop_pc_mailbox pc_bridge_mailbox;
static unsigned posted, last_wall;
static void copy_to(volatile char *d,const char *s,unsigned cap){unsigned i=0;while(i+1<cap&&s&&s[i]){d[i]=s[i];i++;}d[i]=0;}
void desktop_pc_begin(unsigned id){pc_bridge_mailbox.state=0;pc_bridge_mailbox.magic=PC_BRIDGE_MAGIC;pc_bridge_mailbox.task_id=id;pc_bridge_mailbox.sequence=0;pc_bridge_mailbox.cancelled_task=0;posted=0;last_wall=pc_bridge_mailbox.wall_ms;}
void desktop_pc_cancel(void){pc_bridge_mailbox.cancelled_task=pc_bridge_mailbox.task_id;pc_bridge_mailbox.state=0;posted=0;}
/* Poll only at real 1s wall-clock intervals, or immediately for replies.
 * ag_loop's existing 128-poll limit remains meaningful (128s/turn), while the
 * harness separately enforces a 90s task deadline. UI CPU/raster keep running. */
int desktop_pc_poll_due(void){unsigned wall=pc_bridge_mailbox.wall_ms;if(!posted||pc_bridge_mailbox.state>=2||wall-last_wall>=1000){last_wall=wall;return 1;}return 0;}
ag_backend_poll_status desktop_pc_poll(void *ctx,const ag_loop_input *in,uint32_t quota,char *out,size_t cap,size_t *bytes){
    (void)ctx;(void)quota;*bytes=0;
    if(!posted){pc_bridge_mailbox.kind=in->kind;pc_bridge_mailbox.receipt_code=in->receipt_code;copy_to(pc_bridge_mailbox.task,in->task,sizeof pc_bridge_mailbox.task);copy_to(pc_bridge_mailbox.call_id,in->call_id,sizeof pc_bridge_mailbox.call_id);copy_to(pc_bridge_mailbox.receipt_text,in->receipt_text,sizeof pc_bridge_mailbox.receipt_text);pc_bridge_mailbox.response_bytes=0;pc_bridge_mailbox.sequence++;posted=1;__asm__ volatile("fence rw,rw":::"memory");pc_bridge_mailbox.state=1;return AG_BACKEND_PENDING;}
    if(pc_bridge_mailbox.state==3){posted=0;pc_bridge_mailbox.state=0;return AG_BACKEND_FAILED;}
    if(pc_bridge_mailbox.state!=2)return AG_BACKEND_PENDING;
    unsigned n=pc_bridge_mailbox.response_bytes;
    if(n>cap||n>=sizeof pc_bridge_mailbox.response){*bytes=cap+1;out[0]=0;}else{for(unsigned i=0;i<n;i++)out[i]=pc_bridge_mailbox.response[i];out[n]=0;*bytes=n;}
    posted=0;pc_bridge_mailbox.state=0;return AG_BACKEND_READY;
}
