#ifndef AG_QW_RUNTIME_H
#define AG_QW_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#include "vendor/npu_v1/npu_storage.h"
#include "vendor/npu_v1/npu_raw.h"
#ifndef QW_CONTEXT_MAX
#define QW_CONTEXT_MAX 1024u
#endif
#define QW_VOCAB 248320u
#define QW_HIDDEN 1024u
#define QW_LAYERS 24u
#define QW_POLICY_VERSION 1u
/* Opt-in candidate only; callers retain control of options.tile_rows. */
#define QW_TILE_ROWS_OPTIMIZED 8192u
enum { QW_OK=0,QW_ARGUMENT=-100,QW_CAPACITY=-101,QW_FORMAT=-102,
 QW_IO=-103,QW_HASH=-104,QW_DEVICE=-105,QW_UNKNOWN=-106,
 QW_NEED_RESET=-107,QW_CONTEXT_FULL=-108,QW_NUMERIC=-109,QW_TOKEN=-110 };
typedef struct qw_model qw_model;
/* Trace data is borrowed until the next callback, never a reference answer. */
typedef void (*qw_trace_fn)(void *user,uint32_t position,uint32_t layer,
 const char *stage,const float *data,size_t count);
/* Integer-only monotonic tick source. All *_ticks use this caller's unit;
 * hardwarebusy_cycles uses the separate NPU CSR clock domain. */
typedef uint64_t (*qw_clock_fn)(void *user);
/* Optional synchronous BF16 head. weights is row-major little-endian BF16;
 * return zero after all rows complete, nonzero for QW_NUMERIC. Preserve the
 * scalar per-row decode/multiply/add order; do not reenter the runtime. */
typedef int (*qw_bf16_head_fn)(void *user,const unsigned char *weights,
 const float *input,uint32_t rows,uint32_t cols,float *out);
typedef struct {
 uint64_t totalstep_ticks,quantize_ticks,rawexecute_ticks,scale_reduce_ticks,bf16head_ticks;
 uint64_t hardwarebusy_cycles;
 uint64_t tokens_started,tokens_completed,tokens_failed;
 uint64_t matrix_calls,jobs_started,jobs_completed,hardwarebusy_jobs,bf16head_calls;
} qw_profile;
typedef struct {
 uint32_t context_tokens,prefill_chunk_tokens,tile_rows,arena_ddr_offset;
 size_t system_reserved_bytes,framebuffer_reserved_bytes;
 npu_raw_mode mode;
 volatile npu_dma_regs *device;
 npu_raw_budgets budgets;
 qw_trace_fn trace;void *trace_user;
 /* Required exact SHA256 digests of the three immutable deployment files. */
 const uint8_t *weight_sha256,*cpu_sha256,*catalog_sha256;
 /* Enabled only when BOTH profile and profile_clock are non-NULL. Caller
  * owns/zeros profile; all pointers remain valid until successful close.
  * The clock callback must be integer-only and must not reenter the runtime. */
 qw_profile *profile;qw_clock_fn profile_clock;void *profile_clock_user;
 /* NULL keeps the scalar BF16 head. Ignored when a W4 head is selected. */
 qw_bf16_head_fn bf16_head_kernel;void *bf16_head_user;
 /* Zero preserves the FP baseline. Nonzero opts into integer LINEAR LUTs
  * for forward SiLU/sigmoid/softplus/softmax exp only; graph remains mixed.
  * Zero-initialize options; use qw_set_lut_nonlinear between runs to switch. */
 uint32_t lut_nonlinear;
 /* Lazy tied-embedding load. When nonzero and embedding_read is set, the
  * token_embd matrix is NOT streamed at open; qw_step fetches the single
  * 2048-byte row it needs from storage into the row's arena slot. Values are
  * bit-identical to a full load. Requires an enabled W4 head. */
 uint32_t lazy_embedding;
 int (*embedding_read)(void *user,uint64_t offset,void *dst,size_t bytes);
 void *embedding_user;
} qw_options;
typedef struct {
 size_t context_offset,weights_offset,weights_bytes,cpu_offset,cpu_bytes;
 size_t catalog_offset,catalog_bytes,state_offset,state_bytes;
 size_t work_offset,work_bytes,activation_offset,activation_bytes;
 size_t partial_offset,partial_bytes,logits_offset,logits_bytes;
 size_t arena_bytes,system_reserved_bytes,framebuffer_reserved_bytes;
 uint32_t context_tokens,policy_version;
} qw_memory_plan;
typedef struct {
 uint32_t position,needs_reset,lease_active,npu_jobs;
 int last_error;const float *logits;size_t logits_count;
} qw_status;
int qw_plan(const qw_options *,uint64_t weights_bytes,uint64_t cpu_bytes,
 uint64_t catalog_bytes,size_t arena_capacity,qw_memory_plan *);
/* Try 8192 rows before open; options and optional plan change only on success.
 * No implicit fallback or change to the caller's existing (e.g. 512) tile. */
int qw_try_opt_tile(qw_options *,uint64_t weights_bytes,uint64_t cpu_bytes,
 uint64_t catalog_bytes,size_t arena_capacity,qw_memory_plan *);
/* Between calls only: change scheduling within the existing partial allocation.
 * Active/UNKNOWN leases are rejected without recovery; arena/grants stay fixed. */
int qw_set_tile_rows(qw_model *,uint32_t rows);
/* Clear/snapshot only between runtime calls; model reset does not clear stats.
 * get returns QW_ARGUMENT when profiling is disabled. No allocation or MMIO. */
void qw_profile_clear(qw_profile *);
int qw_get_profile(const qw_model *,qw_profile *);
/* Initialize *model=NULL. A non-NULL handle is never overwritten, including
 * after UNKNOWN; close successfully and clear the handle before reopening. */
int qw_open(qw_model **,void *arena,size_t arena_capacity,const qw_options *,
 const npu_storage_source *weights,const npu_storage_source *cpu,
 const npu_storage_source *catalog,qw_memory_plan *);
/* Prefill and decode use the identical sequential recurrent token transition.
 * processed reports complete tokens even when a later token fails. Chunk128
 * is a software scheduling limit, not a hardware GEMM claim. */
int qw_prefill(qw_model *,const uint32_t *,size_t count,size_t *processed);
int qw_decode(qw_model *,uint32_t token,const float **logits);
int qw_get_status(const qw_model *,qw_status *);
int qw_reset(qw_model *);
/* UNKNOWN keeps the caller arena leased. Do not free/reuse on failure. */
int qw_close(qw_model *);
/* Greedy or seeded temperature/top-k sampling, no hidden scripted fallback. */
int qw_sample(const float *,size_t,float temperature,uint32_t top_k,
 uint32_t *seed,uint32_t *token);
int qw_is_stop(uint32_t token);
#endif
