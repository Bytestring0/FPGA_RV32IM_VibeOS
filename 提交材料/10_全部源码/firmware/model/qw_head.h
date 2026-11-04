#ifndef AG_QW_HEAD_H
#define AG_QW_HEAD_H
#include "qw_runtime.h"

#define QW_W4_HEAD_TENSOR_NAME "output_w4.weight"

/* Explicit experimental candidate load. Unlike qw_open, this requires exactly
 * 187 package W4 entries and 321 catalog entries (187 W4 + 134 CPU). The only
 * added entry must have this exact reserved name, target_dtype
 * signed_W4_zero_point_0 and source_shape [QW_VOCAB,QW_HIDDEN]. The original
 * token_embd.weight remains BF16, once, for embedding and the default head.
 * All three exact deployment SHA256 digests remain mandatory. Candidate bytes
 * must be budgeted with qw_plan; no automatic conversion or quality claim.
 * Success still selects the BF16 head. Call qw_enable_w4_head separately.
 * Initialize *model=NULL; all qw_open ownership/UNKNOWN rules still apply. */
int qw_open_w4_head_candidate(qw_model **model,void *arena,size_t arena_capacity,
 const qw_options *options,const npu_storage_source *weights,
 const npu_storage_source *cpu,const npu_storage_source *catalog,
 qw_memory_plan *plan,const char *exact_tensor_name);

/* Requires successful candidate open, the exact reserved tensor name, a
 * quiescent raw context and no pending reset. Never recovers/discards UNKNOWN.
 * Selection only changes the output head; embeddings/state are unchanged.
 * Newly selecting the head invalidates previously published logits. Reset
 * preserves this explicit selection; close/reopen starts with BF16 again.
 * One caller only, like the runtime/raw APIs; do not bind from a trace callback.
 * Returns QW_UNKNOWN while the raw lease is active, QW_NEED_RESET for a pending
 * reset, QW_ARGUMENT for an invalid handle/name, QW_FORMAT if no candidate. */
int qw_enable_w4_head(qw_model *model,const char *exact_tensor_name);

/* Query selection only: *exact_tensor_name is NULL for the default BF16 head,
 * or the static reserved name for the explicitly enabled W4 head. This is
 * metadata, not proof of device quiescence or of numerical quality. */
int qw_get_w4_head(const qw_model *model,const char **exact_tensor_name);
#endif
