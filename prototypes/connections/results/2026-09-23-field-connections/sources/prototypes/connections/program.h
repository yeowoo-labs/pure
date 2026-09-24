#ifndef PURE_PROGRAM_H
#define PURE_PROGRAM_H
#include "flow.h"

#define PURE_PROGRAM_LIMIT 16384u
#define PURE_DYNAMIC_LIMIT 8u
typedef struct {
    size_t offset, length;
    bool to_end;
} PureRegion;

typedef struct { size_t value; bool from_u32le; } PureIndex;
typedef struct {
    PureRegion source;
    size_t source_step; /* 0: source region; otherwise a preceding step + 1. */
    PureIndex offset, length;
} PureRangeStep;

/* Prepared connections, not an instruction stream. All view definitions are
 * checked at preparation; their root-length requirements survive elimination.
 * Input views are evaluated before the write effect. A read still opens and
 * validates the object even if its response has no body. */
typedef struct {
    PureRegion read, write, reply;
    size_t read_required, input_required;
    size_t view_definitions, range_definitions;
    size_t write_step, reply_step, input_steps;
    PureRangeStep steps[PURE_DYNAMIC_LIMIT];
    bool reply_input;
} PureProgram;

bool pure_program_parse(const char *source, size_t length, PureProgram *out,
                        char *error, size_t error_size);
bool pure_program_load(const char *path, PureProgram *out, char *error, size_t error_size);
bool pure_region_span(PureRegion region, PureSpan source, PureSpan *out);
/* Validates all input views, including unused dynamic views, before any effect.
 * Returns borrowed selections; their lifetime remains that of source. */
bool pure_program_input(const PureProgram *, PureSpan source, PureSpan *write, PureSpan *reply);
#endif
