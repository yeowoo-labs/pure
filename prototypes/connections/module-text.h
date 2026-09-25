#ifndef PURE_MODULE_TEXT_H
#define PURE_MODULE_TEXT_H
#include "flow.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>

/* Text helpers of the module executor. Text is valid UTF-8; a character is a
 * Unicode scalar value, so every position these return is a scalar boundary. */

/* Strict UTF-8: no overlongs, surrogates or values past U+10FFFF. state
 * carries a sequence split across spans; start it at 0 and finish with
 * pure_utf8_complete. */
bool pure_utf8_step(unsigned *state, PureSpan span);
bool pure_utf8_complete(unsigned state);
bool pure_utf8_valid(PureSpan span);

/* Scalars (bytes that are not 10xxxxxx) and newlines in a span. */
size_t pure_utf8_scalars(PureSpan span);
size_t pure_utf8_newlines(PureSpan span);

/* Shortest decimal that reads back to the same finite double: integers
 * below 1e15 without a point or exponent, otherwise %g form with a trimmed
 * exponent. -0 prints as 0. Returns the length written (at most 32). */
size_t pure_format_number(double value, char out[32]);

/* Positions in one version of a text draft (size is version.bytes). */
size_t pure_text_step(const PureDraft *, PureVersion, size_t position, double count);
size_t pure_text_line_step(const PureDraft *, PureVersion, size_t position, double count);
size_t pure_text_line_start(const PureDraft *, PureVersion, size_t position);
size_t pure_text_line_end(const PureDraft *, PureVersion, size_t position);
/* Scalars (unit 0) or line number counting from 1 (unit 1) before position. */
size_t pure_text_count(const PureDraft *, PureVersion, size_t position, unsigned unit);
#endif
