#ifndef AG_NPU_PLATFORM_H
#define AG_NPU_PLATFORM_H
#include "npu_raw.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Optional AG platform binding. Regions are allocated by the Agent/CPU memory
 * planner, not by this driver. The context and region table remain caller-owned.
 * Initialize a new zeroed context only; one context owns a device at a time. */
int npu_ag_open(npu_raw_context *context, volatile npu_dma_regs *device,
                 const npu_raw_region *regions, size_t count);
#ifdef __cplusplus
}
#endif
#endif
