#include "module-text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHUNK = 4096 };

/* state: pending continuation bytes (bits 0-1) and the allowed range of the
 * next one (bits 2-4): 0 any, 1 A0-BF after E0, 2 80-9F after ED, 3 90-BF
 * after F0, 4 80-8F after F4. */
bool pure_utf8_step(unsigned *state, PureSpan span) {
    unsigned remaining = *state & 3, range = *state >> 2;
    for (size_t i = 0; i < span.size; ++i) {
        /* Eight ASCII bytes at a time between characters. */
        uint64_t word;
        while (!remaining && span.size - i >= 8 && (memcpy(&word, span.data + i, 8), !(word & UINT64_C(0x8080808080808080))))
            i += 8;
        if (i == span.size) break;
        unsigned char b = span.data[i];
        if (remaining) {
            unsigned low = range == 1 ? 0xA0 : range == 3 ? 0x90 : 0x80;
            unsigned high = range == 2 ? 0x9F : range == 4 ? 0x8F : 0xBF;
            if (b < low || b > high) return false;
            --remaining;
            range = 0;
        } else if (b < 0x80) {
            continue;
        } else if (b >= 0xC2 && b <= 0xDF) {
            remaining = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            remaining = 2;
            range = b == 0xE0 ? 1 : b == 0xED ? 2 : 0;
        } else if (b >= 0xF0 && b <= 0xF4) {
            remaining = 3;
            range = b == 0xF0 ? 3 : b == 0xF4 ? 4 : 0;
        } else return false;
    }
    *state = remaining | range << 2;
    return true;
}

bool pure_utf8_complete(unsigned state) { return (state & 3) == 0; }

bool pure_utf8_valid(PureSpan span) {
    unsigned state = 0;
    return pure_utf8_step(&state, span) && pure_utf8_complete(state);
}

size_t pure_utf8_scalars(PureSpan span) {
    size_t count = 0;
    for (size_t i = 0; i < span.size; ++i) count += (span.data[i] & 0xC0) != 0x80;
    return count;
}

size_t pure_utf8_newlines(PureSpan span) {
    size_t count = 0;
    for (size_t i = 0; i < span.size; ++i) count += span.data[i] == '\n';
    return count;
}

size_t pure_format_number(double value, char out[32]) {
    if (value == 0) { memcpy(out, "0", 2); return 1; }
    if (value == floor(value) && fabs(value) < 1e15) {
        /* An integer's digits, without printf (the common case, many times cheaper). */
        char digits[16];
        uint64_t n = (uint64_t)fabs(value);
        size_t count = 0, length = 0;
        do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n);
        if (value < 0) out[length++] = '-';
        while (count) out[length++] = digits[--count];
        out[length] = 0;
        return length;
    }
    for (int precision = 1; precision <= 17; ++precision) {
        snprintf(out, 32, "%.*g", precision, value);
        if (strtod(out, NULL) == value) break;
    }
    char *exponent = strchr(out, 'e');
    if (exponent) {
        char *digits = exponent + 2; /* after the sign */
        while (digits[0] == '0' && digits[1]) memmove(digits, digits + 1, strlen(digits));
    }
    return strlen(out);
}

typedef struct { unsigned char *out; } Copy;

static bool copy(void *context, PureSpan span) {
    Copy *c = context;
    memcpy(c->out, span.data, span.size);
    c->out += span.size;
    return true;
}

static void read_bytes(const PureDraft *draft, PureVersion version, size_t offset, size_t length,
                       unsigned char *out) {
    Copy c = {out};
    if (length) pure_draft_read_version(draft, version, offset, length, copy, &c);
}

static size_t forward(const PureDraft *draft, PureVersion version, size_t position) {
    if (position >= version.bytes) return version.bytes;
    unsigned char lead;
    read_bytes(draft, version, position, 1, &lead);
    size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    return position + length <= version.bytes ? position + length : version.bytes;
}

static size_t backward(const PureDraft *draft, PureVersion version, size_t position) {
    if (!position) return 0;
    size_t take = position < 4 ? position : 4;
    unsigned char bytes[4];
    read_bytes(draft, version, position - take, take, bytes);
    size_t at = take - 1;
    while (at && (bytes[at] & 0xC0) == 0x80) --at;
    return position - take + at;
}

/* Saturating: stops at the start or end, so work is bounded by the text size. */
size_t pure_text_step(const PureDraft *draft, PureVersion version, size_t position, double count) {
    for (; count > 0 && position < version.bytes; --count) position = forward(draft, version, position);
    for (; count < 0 && position > 0; ++count) position = backward(draft, version, position);
    return position;
}

size_t pure_text_line_start(const PureDraft *draft, PureVersion version, size_t position) {
    unsigned char chunk[CHUNK];
    while (position) {
        size_t take = position < CHUNK ? position : CHUNK;
        read_bytes(draft, version, position - take, take, chunk);
        for (size_t i = take; i-- > 0;)
            if (chunk[i] == '\n') return position - take + i + 1;
        position -= take;
    }
    return 0;
}

size_t pure_text_line_end(const PureDraft *draft, PureVersion version, size_t position) {
    unsigned char chunk[CHUNK];
    while (position < version.bytes) {
        size_t take = version.bytes - position < CHUNK ? version.bytes - position : CHUNK;
        read_bytes(draft, version, position, take, chunk);
        for (size_t i = 0; i < take; ++i)
            if (chunk[i] == '\n') return position + i;
        position += take;
    }
    return version.bytes;
}

static size_t advance_within(const PureDraft *draft, PureVersion version, size_t position,
                             size_t columns, size_t limit) {
    for (; columns && position < limit; --columns) position = forward(draft, version, position);
    return position < limit ? position : limit;
}

/* Moves keep the column; with no further line the cursor goes to the end or
 * the start of the text. */
size_t pure_text_line_step(const PureDraft *draft, PureVersion version, size_t position, double count) {
    size_t start = pure_text_line_start(draft, version, position), column = 0;
    for (size_t at = start; at < position; at = forward(draft, version, at)) ++column;
    for (; count > 0; --count) {
        size_t end = pure_text_line_end(draft, version, position);
        if (end == version.bytes) return version.bytes;
        size_t next = end + 1;
        position = advance_within(draft, version, next, column, pure_text_line_end(draft, version, next));
    }
    for (; count < 0; ++count) {
        size_t here = pure_text_line_start(draft, version, position);
        if (!here) return 0;
        size_t previous = pure_text_line_start(draft, version, here - 1);
        position = advance_within(draft, version, previous, column, here - 1);
    }
    return position;
}

typedef struct { size_t count; unsigned unit; } Count;

static bool count_span(void *context, PureSpan span) {
    Count *c = context;
    c->count += c->unit ? pure_utf8_newlines(span) : pure_utf8_scalars(span);
    return true;
}

size_t pure_text_count(const PureDraft *draft, PureVersion version, size_t position, unsigned unit) {
    Count c = {0, unit};
    if (position) pure_draft_read_version(draft, version, 0, position, count_span, &c);
    return c.count + (unit ? 1 : 0);
}
