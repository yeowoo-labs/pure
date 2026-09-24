#ifndef PURE_PROGRAM_H
#define PURE_PROGRAM_H
#include "flow.h"

#define PURE_PROGRAM_LIMIT 16384u
typedef struct {
    size_t offset, length;
    bool whole;
} PureRegion;

/* Prepared connections, not an instruction stream. All view definitions are
 * checked at preparation; their root-length requirements survive elimination.
 * Input views are evaluated before the write effect. A read still opens and
 * validates the object even if its response has no body. */
typedef struct {
    PureRegion read, write, reply;
    size_t read_required, input_required;
    size_t view_definitions, range_definitions;
    bool reply_input;
} PureProgram;

bool pure_program_parse(const char *source, size_t length, PureProgram *out,
                        char *error, size_t error_size);
bool pure_program_load(const char *path, PureProgram *out, char *error, size_t error_size);
bool pure_region_span(PureRegion region, PureSpan source, PureSpan *out);
#endif
