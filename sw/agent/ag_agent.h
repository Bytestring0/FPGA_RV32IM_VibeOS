#ifndef AG_AGENT_H
#define AG_AGENT_H
#include <stddef.h>
#include <stdint.h>

#define AG_REQUEST_MAX 512
#define AG_ID_MAX 32
#define AG_TEXT_MAX 192
#define AG_NOTES_MAX 4
#ifndef AG_RECEIPTS_MAX
#define AG_RECEIPTS_MAX 8
#endif
#define AG_RESULT_MAX 96

typedef enum {
    AG_OK, AG_BAD_JSON, AG_BAD_ARGS, AG_UNSUPPORTED, AG_LIMIT,
    AG_ARITHMETIC, AG_CONFLICT, AG_NOT_FOUND, AG_TIMER_STATE, AG_STORAGE
} ag_code;
typedef enum { AG_IDLE, AG_RUNNING, AG_PAUSED, AG_FINISHED } ag_timer_status;
typedef struct { ag_code code; char result[AG_RESULT_MAX + 1]; } ag_receipt;
typedef struct {
    char id[AG_ID_MAX + 1], request[AG_REQUEST_MAX + 1];
    char tool[32];
    ag_receipt receipt;
} ag_record;
typedef struct {
    char notes[AG_NOTES_MAX][AG_TEXT_MAX + 1];
    unsigned note_count, record_count;
    ag_record records[AG_RECEIPTS_MAX];
    ag_timer_status timer;
    uint64_t deadline_ms, remaining_ms;
    char label[AG_TEXT_MAX + 1];
    uint32_t timer_revision;
} ag_state;
/* commit must durably save the complete candidate, or fail without committing.
 * NULL is explicitly volatile mode; no power-loss guarantee is provided.
 * Do not dump ag_state as a portable disk format (padding/endian/ABI).
 */
typedef int (*ag_commit_fn)(void *context, const ag_state *candidate);
typedef struct {
    ag_state state, scratch; /* scratch avoids a large RV32 stack frame */
    ag_commit_fn commit;
    void *context;
} ag_runtime;

void ag_init(ag_runtime *runtime, ag_commit_fn commit, void *context);
ag_code ag_execute(ag_runtime *runtime, const char *call_id,
                   const char *normalized_json, uint64_t now_ms,
                   ag_receipt *receipt);
ag_code ag_tick(ag_runtime *runtime, uint64_t now_ms);
const char *ag_code_name(ag_code code);
/* Three decimal places, truncation toward zero per operation, abs <= 1e6. */
ag_code ag_calculate(const char *expression, char output[AG_RESULT_MAX + 1]);
#endif
