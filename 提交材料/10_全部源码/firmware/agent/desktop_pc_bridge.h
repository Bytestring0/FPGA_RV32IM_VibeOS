#ifndef DESKTOP_PC_BRIDGE_H
#define DESKTOP_PC_BRIDGE_H
/* Simulation-only RAM mailbox. No UART RX or physical transport is implied.
 * The Verilator harness locates pc_bridge_mailbox from ELF symbols. All lengths
 * are bounded bytes; strings are UTF-8. state: 0 idle,1 request,2 ready,3 fail.
 * Single producer/consumer; task+sequence identify stale replies. */
#include "ag_loop.h"
#define PC_BRIDGE_MAGIC 0x50514331u
#define PC_BRIDGE_TASK_BYTES 513u
#define PC_BRIDGE_CALL_BYTES 33u
#define PC_BRIDGE_RECEIPT_BYTES 161u
#define PC_BRIDGE_RESPONSE_BYTES 769u
#define PC_BRIDGE_TIMEOUT_MS 90000u
typedef struct {
    uint32_t magic, state, task_id, sequence, kind, receipt_code;
    uint32_t response_bytes, wall_ms, cancelled_task;
    char task[PC_BRIDGE_TASK_BYTES];
    char call_id[PC_BRIDGE_CALL_BYTES];
    char receipt_text[PC_BRIDGE_RECEIPT_BYTES];
    char response[PC_BRIDGE_RESPONSE_BYTES];
} desktop_pc_mailbox;
extern volatile desktop_pc_mailbox pc_bridge_mailbox;
void desktop_pc_begin(unsigned task_id);
void desktop_pc_cancel(void);
int desktop_pc_poll_due(void);
ag_backend_poll_status desktop_pc_poll(void *, const ag_loop_input *, uint32_t,
                                      char *, size_t, size_t *);
#endif
