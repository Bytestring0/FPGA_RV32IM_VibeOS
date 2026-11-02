#ifndef AG_QW_RUNTIME_H
#define AG_QW_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#include "vendor/npu_v1/npu_storage.h"
#include "vendor/npu_v1/npu_raw.h"
#define QW_RUNTIME_API_VERSION 2u
#define QW_CONTEXT_MAX 2048u
#define QW_VOCAB 248320u
#define QW_HIDDEN 1024u
#define QW_LAYERS 24u
#define QW_POLICY_VERSION 1u
enum { QW_OK=0,QW_ARGUMENT=-100,QW_CAPACITY=-101,QW_FORMAT=-102,
 QW_IO=-103,QW_HASH=-104,QW_DEVICE=-105,QW_UNKNOWN=-106,
 QW_NEED_RESET=-107,QW_CONTEXT_FULL=-108,QW_NUMERIC=-109,QW_TOKEN=-110,
 QW_BUSY=-111,QW_CANCELLED=-112,QW_UNSUPPORTED=-113,QW_PENDING=1 };
typedef struct qw_model qw_model;
/* Trace data is borrowed until the next callback, never a reference answer. */
typedef void (*qw_trace_fn)(void *user,uint32_t position,uint32_t layer,
 const char *stage,const float *data,size_t count);
typedef struct {
 uint32_t context_tokens,prefill_chunk_tokens,tile_rows,arena_ddr_offset;
 size_t system_reserved_bytes,framebuffer_reserved_bytes;
 npu_raw_mode mode;
 volatile npu_dma_regs *device;
 npu_raw_budgets budgets;
 qw_trace_fn trace;void *trace_user;
 /* Exact SHA256 identities are mandatory and copied into the model. */
 const uint8_t *weight_sha256,*cpu_sha256,*catalog_sha256;
 uint64_t (*now_ms)(void *user);
 void *clock_user;
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
 uint32_t execution_active,stage,layer,last_poll_units;
 uint64_t last_poll_ms,max_poll_ms;
 size_t completed_tokens;
 uint32_t checkpoint_active;
} qw_status;
int qw_plan(const qw_options *,uint64_t weights_bytes,uint64_t cpu_bytes,
 uint64_t catalog_bytes,size_t arena_capacity,qw_memory_plan *);
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
/* Cooperative SOFTWARE_REFERENCE execution. Tokens are borrowed until terminal
 * completion/cancel. One unit is <=8 matrix rows, 128 conv channels, one Delta
 * head, 32 attention positions, 8 attention outputs or 64 BF16 head rows.
 * cpu_budget_ms requires now_ms; elapsed budget is checked between units.
 * Old hardware ABI has no nonblocking submit/poll: start returns UNSUPPORTED
 * there, never disguising a synchronous device wait as PENDING. */
int qw_start(qw_model *,const uint32_t *tokens,size_t count,int want_logits);
int qw_poll(qw_model *,uint32_t work_quota,uint32_t cpu_budget_ms,
 size_t *completed_tokens,const float **logits);
int qw_cancel(qw_model *);
/* Preserved old FP32 computation order, for scheduler differential tests. */
int qw_decode_reference(qw_model *,uint32_t token,const float **logits);
int qw_prefill_reference(qw_model *,const uint32_t *,size_t,size_t *processed,int want_logits);
int qw_get_status(const qw_model *,qw_status *);
int qw_reset(qw_model *);
/* UNKNOWN keeps the caller arena leased. Do not free/reuse on failure. */
int qw_close(qw_model *);
/* Software-reference checkpoint v1. Caller owns a separate aligned-or-unaligned
 * buffer; keep only the exact shared system prefix, never user/tool history.
 * Hashes are binary SHA256 of tokenizer, chat template and tool schema.
 * A saved image contains position, exact processed token IDs, all recurrent
 * DeltaNet/conv/KV state and the bound parameter SHA256 identities. This
 * baseline stores FP32 state; it is NOT an integer or hardware checkpoint. */
typedef struct {
  uint8_t tokenizer_sha256[32],template_sha256[32],schema_sha256[32];
  const uint32_t *prefix_ids;
  size_t prefix_count;
} qw_checkpoint_key;
int qw_checkpoint_bytes(const qw_model *,size_t prefix_count,size_t *bytes);
int qw_checkpoint_save(const qw_model *,const qw_checkpoint_key *,void *buffer,size_t capacity);
/* Validation is complete before state is changed; UNKNOWN keeps the arena
 * leased and returns without restoring anything. Logits are invalidated. */
int qw_checkpoint_restore(qw_model *,const qw_checkpoint_key *,const void *buffer,size_t bytes);
/* Offline whole-image load from immutable source offset zero. size_bytes must
 * equal qw_checkpoint_bytes exactly; expected_sha256 authenticates the entire
 * image in addition to its internal identities/digest. No heap or model-context
 * pointer serialization. Caller exclusively owns a separate buffer for the call;
 * source callback/ctx/backing data must stay stable and disjoint from the buffer
 * (opaque backing extents cannot be inferred here); callback writes only its dst,
 * and must not reenter or mutate the model, key, digest or buffer outside that dst.
 * UNKNOWN/BUSY and all preflight errors do no source I/O and leave buffer alone.
 * Once I/O starts the buffer magic stays invalid until the whole image is read
 * and authenticated; ANY later failure clears it. Model/arena change only after
 * successful qw_checkpoint_restore. UNKNOWN keeps the model arena leased.
 * ARGUMENT: invalid/overlapping/wrapping memory or callback; CAPACITY: insufficient
 * buffer/size arithmetic; FORMAT: source size or image header mismatch; IO: exact
 * read failed; HASH: outer/internal digest or identity; TOKEN: prefix mismatch.
 * Existing restore errors are propagated unchanged. This call is synchronous. */
int qw_checkpoint_restore_from_source(qw_model *,const qw_checkpoint_key *,
 const npu_storage_source *,const uint8_t expected_sha256[32],
 void *buffer,size_t capacity);
/* Invalidates an external image; caller releases its storage after use. */
int qw_checkpoint_invalidate(void *buffer,size_t bytes);
/* Cooperative FP32 checkpoint transaction; no asynchronous device/DMA claim.
 * Initialize a job with {0}; never move, copy, free or edit it while active.
 * Model, key/IDs and image must remain alive and immutable to other users until
 * terminal poll/cancel. Image/job/key/IDs must be independent of model/arena/work.
 * Begin returns OK=accepted without scanning/copying the full state. The model's
 * checkpoint_active lease blocks start/decode/reset/close (integration guards).
 * Each poll unit compares/copies/hashes <=4096 payload bytes, or clears <=4096
 * work/ID bytes, or commits a fixed header/metadata. Work quota is a strict upper
 * bound. cpu_budget_ms requires model.options.now_ms; budget is checked BETWEEN
 * units (one unit can overrun). UI should use quota=1 and measure its platform;
 * no universal <=100ms guarantee is inferred. Clock must be prompt/monotonic.
 * PENDING retains all resources; OK is committed. Validation errors release the
 * transaction without changing state. UNKNOWN/BUSY retain the transaction until
 * raw/execution work is proven quiescent. Cancel before restore copy returns
 * CANCELLED with old state intact; cancel/failure after ANY restore state/work/ID
 * write returns NEED_RESET and invalidates logits. Position publishes only at
 * final commit. A terminal cancel confirms this SOFTWARE transaction is stopped;
 * it never recovers unknown device work. Save magic remains zero until commit.
 * Runtime cannot detect freed pointers or arbitrary mutation of opaque borrowed
 * storage: exclusive immutable ownership is required throughout the transaction.
 * Existing synchronous APIs above remain for offline/non-UI use only. */
#define QW_CHECKPOINT_BLOCK_BYTES 4096u
#define QW_CHECKPOINT_JOB_BYTES 1024u
typedef union {
 max_align_t alignment;
 unsigned char opaque[QW_CHECKPOINT_JOB_BYTES];
} qw_checkpoint_job;
typedef enum {
 QW_CHECKPOINT_IDLE=0,QW_CHECKPOINT_SAVE_IDS,QW_CHECKPOINT_SAVE_STATE,
 QW_CHECKPOINT_VERIFY_IDS,QW_CHECKPOINT_VERIFY_STATE,QW_CHECKPOINT_RESTORE_STATE,
 QW_CHECKPOINT_CLEAR_WORK,QW_CHECKPOINT_PUBLISH_IDS,QW_CHECKPOINT_COMMIT,
 QW_CHECKPOINT_DONE,QW_CHECKPOINT_RESET_STATE
} qw_checkpoint_phase;
typedef struct {
 uint32_t active,restoring,resetting,phase,last_poll_units,state_modified;
 size_t image_bytes,hash_bytes,copy_bytes,clear_bytes;
 int result,failure_reason;
} qw_checkpoint_progress;
int qw_checkpoint_begin_save(qw_checkpoint_job *,qw_model *,
 const qw_checkpoint_key *,void *image,size_t capacity);
int qw_checkpoint_begin_restore(qw_checkpoint_job *,qw_model *,
 const qw_checkpoint_key *,const void *image,size_t bytes);
/* Clear already-bound FP32 state/work/IDs in 4KB units; no catalog rebinding.
 * Begin/cancel/lease/budget rules match restore. Before any write cancellation
 * leaves old state intact; after a write cancellation returns NEED_RESET.
 * Terminal jobs may be reused directly; NEVER memset an active/UNKNOWN job. */
int qw_checkpoint_begin_reset(qw_checkpoint_job *,qw_model *);
int qw_checkpoint_poll(qw_checkpoint_job *,uint32_t work_quota,uint32_t cpu_budget_ms);
int qw_checkpoint_cancel(qw_checkpoint_job *);
int qw_checkpoint_job_status(const qw_checkpoint_job *,qw_checkpoint_progress *);
/* Greedy or seeded temperature/top-k sampling, no hidden scripted fallback. */
int qw_sample(const float *,size_t,float temperature,uint32_t top_k,
 uint32_t *seed,uint32_t *token);
int qw_is_stop(uint32_t token);
#endif
