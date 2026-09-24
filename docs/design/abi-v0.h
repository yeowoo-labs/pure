/* Draft data records for Pure's in-process byte-stream profile (0.1).
 * No module loader or callable function table is implemented by this header.
 * These records use native endianness and are NOT a file/wire format.
 */
#ifndef PURE_DRAFT_ABI_V0_H
#define PURE_DRAFT_ABI_V0_H

#include <stdint.h>
#include <stddef.h>

#define PURE_ABI_MAJOR_V0 UINT32_C(0)
#define PURE_ABI_MINOR_V0 UINT32_C(1)
#define PURE_PROFILE_BYTE_STREAM_V0 UINT32_C(1)

#define PURE_STEP_MORE_V0 UINT32_C(0)
#define PURE_STEP_NEED_INPUT_V0 UINT32_C(1)
#define PURE_STEP_NEED_OUTPUT_V0 UINT32_C(2)
#define PURE_STEP_DONE_V0 UINT32_C(3)
#define PURE_STEP_FAILED_V0 UINT32_C(4)
#define PURE_STEP_CANCELLED_V0 UINT32_C(5)

#define PURE_ERROR_NONE_V0 UINT32_C(0)
#define PURE_ERROR_INVALID_V0 UINT32_C(1)
#define PURE_ERROR_LIMIT_V0 UINT32_C(2)
#define PURE_ERROR_UNSUPPORTED_V0 UINT32_C(3)
#define PURE_ERROR_BACKEND_V0 UINT32_C(4)
#define PURE_ERROR_TRUNCATED_V0 UINT32_C(5)
#define PURE_ERROR_AUTH_V0 UINT32_C(6)

typedef struct pure_handle_v0 {
    uint32_t slot;
    uint32_t generation;
} pure_handle_v0;

typedef struct pure_span_v0 {
    pure_handle_v0 buffer;
    uint32_t offset;
    uint32_t length;
} pure_span_v0;

typedef struct pure_step_result_v0 {
    uint32_t struct_bytes;
    uint32_t status;
    uint32_t error;
    uint32_t consumed;
    uint32_t produced;
    uint32_t reserved;
} pure_step_result_v0;

typedef struct pure_stream_limits_v0 {
    uint32_t struct_bytes;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t profile;
    uint32_t flags;
    uint32_t input_count;
    uint32_t output_count;
    uint32_t max_pending_per_context;
    uint64_t context_budget_bytes;
    uint64_t scratch_budget_bytes;
    uint32_t max_input_bytes;
    uint32_t max_output_bytes;
    uint32_t buffer_alignment;
    uint32_t reserved;
} pure_stream_limits_v0;

_Static_assert(sizeof(pure_handle_v0) == 8, "handle record size");
_Static_assert(sizeof(pure_span_v0) == 16, "span record size");
_Static_assert(offsetof(pure_span_v0, offset) == 8, "span offset field");
_Static_assert(sizeof(pure_step_result_v0) == 24, "step result size");
_Static_assert(offsetof(pure_step_result_v0, produced) == 16, "produced field");
_Static_assert(sizeof(pure_stream_limits_v0) == 64, "stream limits size");
_Static_assert(offsetof(pure_stream_limits_v0, context_budget_bytes) == 32, "context budget field");
_Static_assert(offsetof(pure_stream_limits_v0, max_input_bytes) == 48, "input bound field");

#endif
