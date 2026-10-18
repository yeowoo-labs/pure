#include "message-json.h"
#include "module-text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(unsigned char *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

static bool decimal(const char *text, uint64_t maximum, uint64_t *value) {
    if (!*text) return false;
    uint64_t n = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (n > (maximum - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}

/* A JSON number's text: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, its
 * length (0 if there is none). An integer has neither part after it. */
static size_t json_number(const unsigned char *p, size_t size, bool *integer) {
    size_t i = 0;
    *integer = true;
    if (i < size && p[i] == '-') ++i;
    if (i == size || p[i] < '0' || p[i] > '9') return 0;
    if (p[i++] != '0') while (i < size && p[i] >= '0' && p[i] <= '9') ++i;
    if (i < size && p[i] == '.') {
        *integer = false;
        if (++i == size || p[i] < '0' || p[i] > '9') return 0;
        while (i < size && p[i] >= '0' && p[i] <= '9') ++i;
    }
    if (i < size && (p[i] == 'e' || p[i] == 'E')) {
        *integer = false;
        if (++i < size && (p[i] == '+' || p[i] == '-')) ++i;
        if (i == size || p[i] < '0' || p[i] > '9') return 0;
        while (i < size && p[i] >= '0' && p[i] <= '9') ++i;
    }
    return i;
}

/* A JSON number's finite value. */
static bool json_double(const unsigned char *text, size_t size, double *value) {
    bool integer;
    if (!size || json_number(text, size, &integer) != size || size > 40) return false;
    /* Digits m < 2^53 and a power of ten 10^e, |e| <= 22, are exact, so one
     * multiplication or division rounds m * 10^e correctly (Clinger's fast
     * path); other numbers go to strtod. */
    static const double tens[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const unsigned char *p = text, *end = text + size;
    bool negative = *p == '-', exact = true;
    uint64_t m = 0;
    int exponent = 0;
    p += negative;
    for (; p < end && *p >= '0' && *p <= '9'; ++p) {
        exact = exact && m < UINT64_C(900719925474099);
        m = m * 10 + (uint64_t)(*p - '0');
    }
    if (p < end && *p == '.')
        for (++p; p < end && *p >= '0' && *p <= '9'; ++p, --exponent) {
            exact = exact && m < UINT64_C(900719925474099);
            m = m * 10 + (uint64_t)(*p - '0');
        }
    if (p < end) { /* an exponent */
        bool below = *++p == '-';
        int e = 0;
        for (p += *p == '-' || *p == '+'; p < end; ++p) e = e < 1000 ? e * 10 + (*p - '0') : e;
        exponent += below ? -e : e;
    }
    if (exact && exponent >= -22 && exponent <= 22) {
        double v = exponent < 0 ? (double)m / tens[-exponent] : (double)m * tens[exponent];
        *value = negative ? -v : v;
        return true;
    }
    char digits[48];
    memcpy(digits, text, size);
    digits[size] = 0;
    *value = strtod(digits, NULL);
    return isfinite(*value);
}

/* A JSON integer in an i64's range (no point, no exponent), exactly: false for any other text. */
static bool json_integer(const unsigned char *text, size_t size, int64_t *value) {
    bool integer, negative = size && text[0] == '-';
    if (!size || json_number(text, size, &integer) != size || !integer) return false;
    uint64_t n = 0, most = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (size_t i = negative; i < size; ++i) {
        unsigned digit = (unsigned)(text[i] - '0');
        if (n > (most - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = !negative ? (int64_t)n : n ? -(int64_t)(n - 1) - 1 : 0; /* -2^63 is; -0 is 0 */
    return true;
}

/* A JSON true or false at text: its length (4 or 5), or 0. */
static size_t json_bool(const unsigned char *text, size_t size, bool *value) {
    if (size >= 4 && !memcmp(text, "true", 4)) return *value = true, 4;
    if (size >= 5 && !memcmp(text, "false", 5)) return *value = false, 5;
    return 0;
}

/* A fixed field's value from its text (a query value or a JSON value) into
 * the prefix: a u32 in range, a finite number, true or false for a bool, an
 * integer in range for an i64. */
bool pure_json_fixed(const PureField *field, const unsigned char *text, size_t size, unsigned char *prefix) {
    if (field->type == PURE_FIELD_BOOL) { /* true or false, as a JSON value says it */
        bool value;
        if (!size || json_bool(text, size, &value) != size) return false;
        prefix[field->offset] = value;
        return true;
    }
    if (field->type == PURE_FIELD_I64) {
        int64_t value;
        if (!json_integer(text, size, &value)) return false;
        for (unsigned i = 0; i < 8; ++i) prefix[field->offset + i] = (unsigned char)((uint64_t)value >> (8 * i));
        return true;
    }
    bool integer;
    if (!size || json_number(text, size, &integer) != size || size > 40) return false;
    if (field->type == PURE_FIELD_U32) {
        char digits[48];
        uint64_t n;
        memcpy(digits, text, size);
        digits[size] = 0;
        if (!integer || !decimal(digits, UINT32_MAX, &n)) return false;
        write_u32(prefix + field->offset, (uint32_t)n);
        return true;
    }
    double value;
    if (!json_double(text, size, &value)) return false;
    memcpy(prefix + field->offset, &value, 8);
    return true;
}

/* A JSON string's contents (after its opening quote) up to the closing one:
 * its length, or 0 when it does not close or holds a control byte. */
static size_t json_string_end(const unsigned char *p, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        /* Eight bytes at a time while none is a quote, a backslash or a control byte. */
        const uint64_t ones = UINT64_C(0x0101010101010101), highs = UINT64_C(0x8080808080808080);
        for (uint64_t w, q, s; size - i >= 8; i += 8) {
            memcpy(&w, p + i, 8);
            q = w ^ (ones * '"');
            s = w ^ (ones * '\\');
            if (((q - ones) & ~q & highs) | ((s - ones) & ~s & highs) | ((w - ones * 0x20) & ~w & highs)) break;
        }
        if (i == size) break;
        if (p[i] == '"') return i + 1;
        if (p[i] < 0x20) return 0;
        if (p[i] == '\\' && ++i == size) return 0;
    }
    return 0;
}

static int hex_digit(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool json_code(const unsigned char *p, size_t left, uint32_t *code) {
    if (left < 4) return false;
    *code = 0;
    for (int i = 0; i < 4; ++i) {
        int d = hex_digit(p[i]);
        if (d < 0) return false;
        *code = *code << 4 | (uint32_t)d;
    }
    return true;
}

/* Unescape a JSON string's contents into out (never longer); false for a
 * bad escape or a lone surrogate. The result is checked as UTF-8 after. */
static bool json_unescape(const unsigned char *in, size_t size, unsigned char *out, size_t *written) {
    size_t o = 0;
    for (size_t i = 0; i < size;) {
        unsigned char b = in[i++];
        if (b != '\\') { out[o++] = b; continue; }
        if (i == size) return false;
        b = in[i++];
        const char *plain = strchr("\"\\/bfnrt", b);
        if (b && plain) { out[o++] = (unsigned char)"\"\\/\b\f\n\r\t"[plain - "\"\\/bfnrt"]; continue; }
        uint32_t code, low;
        if (b != 'u' || !json_code(in + i, size - i, &code)) return false;
        i += 4;
        if (code >= 0xDC00 && code <= 0xDFFF) return false;
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (size - i < 6 || in[i] != '\\' || in[i + 1] != 'u' || !json_code(in + i + 2, size - i - 2, &low) ||
                low < 0xDC00 || low > 0xDFFF) return false;
            i += 6;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        if (code < 0x80) out[o++] = (unsigned char)code;
        else if (code < 0x800) { out[o++] = (unsigned char)(0xC0 | code >> 6); out[o++] = (unsigned char)(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) {
            out[o++] = (unsigned char)(0xE0 | code >> 12);
            out[o++] = (unsigned char)(0x80 | (code >> 6 & 0x3F));
            out[o++] = (unsigned char)(0x80 | (code & 0x3F));
        } else {
            out[o++] = (unsigned char)(0xF0 | code >> 18);
            out[o++] = (unsigned char)(0x80 | (code >> 12 & 0x3F));
            out[o++] = (unsigned char)(0x80 | (code >> 6 & 0x3F));
            out[o++] = (unsigned char)(0x80 | (code & 0x3F));
        }
    }
    *written = o;
    return true;
}

size_t pure_json_space(const unsigned char *p, size_t at, size_t size) {
    while (at < size && (p[at] == ' ' || p[at] == '\t' || p[at] == '\n' || p[at] == '\r')) ++at;
    return at;
}

/* Skip any JSON value at json[*at] (at most 64 deep): a member of an
 * answer that names no field. */
static bool skip_value(const unsigned char *json, size_t size, size_t *at, unsigned depth) {
    if (*at == size || depth > 64) return false;
    unsigned char b = json[*at];
    bool integer;
    if (b == '"') {
        size_t end = json_string_end(json + *at + 1, size - *at - 1);
        if (!end) return false;
        *at += 1 + end;
        return true;
    }
    if (b == '{' || b == '[') {
        unsigned char close = b == '{' ? '}' : ']';
        *at = pure_json_space(json, *at + 1, size);
        for (bool first = true; *at < size && json[*at] != close; first = false) {
            if (!first) {
                if (json[(*at)++] != ',') return false;
                *at = pure_json_space(json, *at, size);
            }
            if (close == '}') {
                size_t key_end = *at < size && json[*at] == '"' ? json_string_end(json + *at + 1, size - *at - 1) : 0;
                if (!key_end) return false;
                *at = pure_json_space(json, *at + 1 + key_end, size);
                if (*at == size || json[(*at)++] != ':') return false;
                *at = pure_json_space(json, *at, size);
            }
            if (!skip_value(json, size, at, depth + 1)) return false;
            *at = pure_json_space(json, *at, size);
        }
        if (*at == size) return false;
        ++*at;
        return true;
    }
    static const char *const words[] = {"true", "false", "null"};
    for (size_t w = 0; w < 3; ++w)
        if (size - *at >= strlen(words[w]) && !memcmp(json + *at, words[w], strlen(words[w]))) {
            *at += strlen(words[w]);
            return true;
        }
    size_t length = json_number(json + *at, size - *at, &integer);
    *at += length;
    return length > 0;
}

/* The field named path.key (key alone at the top), or the field count when
 * none is; group says whether fields are named inside it instead. */
static uint32_t field_named(const PureJsonMessage *b, const char *path, size_t path_size, const unsigned char *key,
                            size_t key_size, bool *group) {
    const PureModule *m = b->module;
    size_t size = path_size + !!path_size + key_size;
    *group = false;
    for (uint32_t f = 0; f < b->shape->field_count; ++f) {
        const char *name = m->field[b->shape->field_start + f].name;
        size_t length = strlen(name);
        if (length < size || memcmp(name, path, path_size) || (path_size && name[path_size] != '.') ||
            memcmp(name + path_size + !!path_size, key, key_size)) continue;
        if (length == size) return f;
        if (name[size] == '.') *group = true;
    }
    return b->shape->field_count;
}

/* Room for more bytes of rows. */
static bool rows_room(PureJsonMessage *b, size_t more) {
    if (more <= b->rows_capacity - b->rows_size) return true;
    size_t capacity = b->rows_capacity ? b->rows_capacity : 4096;
    while (capacity - b->rows_size < more) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    unsigned char *grown = realloc(b->rows, capacity);
    if (!grown) return false;
    b->rows = grown;
    b->rows_capacity = capacity;
    return true;
}

/* An array (at its opening bracket) of objects of a record's fields by name: a number, a string, or an array of the
 * record a list field holds (027's 4a), a field left out taking its default; encoded one after another into rows as
 * the list's stored form (a number's binary64 bits, a text's u32le length and bytes, a list's u32le length and its
 * items so). An item's lists are encoded first and the item after them, moved down over them at its end. */
static const char *items_of(PureJsonMessage *b, unsigned char *json, size_t size, size_t *at, uint32_t record_index,
                            unsigned depth) {
    const PureModule *m = b->module;
    const PureRecord *record = &m->record[record_index];
    if (depth >= 64) return "limit"; /* 64 lists deep at most */
    if (*at == size || json[(*at)++] != '[') return "json_type";
    *at = pure_json_space(json, *at, size);
    for (bool first = true; *at < size && json[*at] != ']'; first = false) {
        if (!first) {
            if (json[(*at)++] != ',') return "json";
            *at = pure_json_space(json, *at, size);
        }
        if (*at == size || json[(*at)++] != '{') return "json_type";
        PureValue numbers[PURE_RECORD_FIELDS]; /* a number's, a bool's or an i64's */
        PureSpan texts[PURE_RECORD_FIELDS];
        size_t lists[PURE_RECORD_FIELDS][2]; /* a list's stored form: where in rows, and its bytes */
        bool given[PURE_RECORD_FIELDS] = {false};
        size_t item = b->rows_size;
        *at = pure_json_space(json, *at, size);
        for (bool member = true; *at < size && json[*at] != '}'; member = false) {
            if (!member) {
                if (json[(*at)++] != ',') return "json";
                *at = pure_json_space(json, *at, size);
            }
            size_t key_end = *at < size && json[*at] == '"' ? json_string_end(json + *at + 1, size - *at - 1) : 0;
            if (!key_end) return "json";
            const unsigned char *key = json + *at + 1;
            size_t key_size = key_end - 1;
            *at = pure_json_space(json, *at + 1 + key_end, size);
            if (*at == size || json[(*at)++] != ':') return "json";
            *at = pure_json_space(json, *at, size);
            unsigned k = 0;
            while (k < record->count && (strlen(m->record_field_names[record->first + k]) != key_size ||
                                         memcmp(m->record_field_names[record->first + k], key, key_size)))
                ++k;
            if (k == record->count) {
                if (!b->lenient) return "json_field";
                if (!skip_value(json, size, at, depth + 1)) return "json";
                *at = pure_json_space(json, *at, size);
                continue;
            }
            if (given[k]) return "json_field";
            given[k] = true;
            uint8_t type = m->record_field_type[record->first + k];
            bool text = pure_record_bytes(type), truth; /* a string: a text's, or bytes' (as its UTF-8) */
            if (type == PURE_RECORD_LIST) {
                size_t from = b->rows_size;
                const char *why = items_of(b, json, size, at, (uint32_t)m->record_field_default[record->first + k].i64,
                                           depth + 1);
                if (why) return why;
                lists[k][0] = from;
                lists[k][1] = b->rows_size - from;
                *at = pure_json_space(json, *at, size);
                continue;
            }
            if (type == PURE_RECORD_BOOL) { /* true or false, nothing else */
                size_t word = json_bool(json + *at, size - *at, &truth);
                if (!word) return "json_type";
                numbers[k].num = truth;
                *at = pure_json_space(json, *at + word, size);
                continue;
            }
            if (*at < size && json[*at] == '"') {
                size_t end = json_string_end(json + *at + 1, size - *at - 1), written;
                if (!end) return "json";
                if (!text) return "json_type";
                unsigned char *value = json + *at + 1;
                if (!memchr(value, '\\', end - 1)) written = end - 1;
                else if (!json_unescape(value, end - 1, value, &written)) return "json";
                if (type == PURE_RECORD_TEXT && !pure_utf8_valid((PureSpan){value, written})) return "json";
                texts[k] = (PureSpan){value, written};
                *at = pure_json_space(json, *at + 1 + end, size);
                continue;
            }
            bool integer;
            size_t length = json_number(json + *at, size - *at, &integer);
            if (!length) return *at < size && (json[*at] == '[' || json[*at] == '{' || json[*at] == 't' || json[*at] == 'f' ||
                                               json[*at] == 'n') ? "json_type" : "json";
            if (text || !(type == PURE_RECORD_I64 ? json_integer(json + *at, length, &numbers[k].i64)
                                                  : json_double(json + *at, length, &numbers[k].num)))
                return "json_type";
            *at = pure_json_space(json, *at + length, size);
        }
        if (*at == size) return "json";
        ++*at;
        size_t lists_end = b->rows_size;
        for (unsigned k = 0; k < record->count; ++k) {
            uint32_t at_field = record->first + k;
            uint8_t type = m->record_field_type[at_field];
            if (pure_record_bytes(type) || type == PURE_RECORD_LIST) {
                PureSpan text = {0};
                size_t length = type == PURE_RECORD_LIST ? (given[k] ? lists[k][1] : 0) : 0;
                if (type != PURE_RECORD_LIST) {
                    uint32_t constant = (uint32_t)m->record_field_default[at_field].num;
                    text = given[k] ? texts[k]
                                    : (PureSpan){m->constant_data + m->constant_offset[constant], m->constant_length[constant]};
                    length = text.size;
                }
                if (length > UINT32_MAX || !rows_room(b, 4 + length)) return "memory";
                write_u32(b->rows + b->rows_size, (uint32_t)length);
                if (type == PURE_RECORD_LIST && length) memcpy(b->rows + b->rows_size + 4, b->rows + lists[k][0], length);
                else if (length) memcpy(b->rows + b->rows_size + 4, text.data, length);
                b->rows_size += 4 + length;
            } else {
                uint64_t bits = (uint64_t)(given[k] ? numbers[k] : m->record_field_default[at_field]).i64; /* its bits */
                if (!rows_room(b, 8)) return "memory";
                for (unsigned i = 0; i < 8; ++i) b->rows[b->rows_size + i] = (unsigned char)(bits >> (8 * i));
                b->rows_size += 8;
            }
        }
        if (lists_end > item) { /* the item over its lists' forms */
            memmove(b->rows + item, b->rows + lists_end, b->rows_size - lists_end);
            b->rows_size -= lists_end - item;
        }
    }
    if (*at == size) return "json";
    ++*at;
    return NULL;
}

/* A rows field's array, encoded so (items_of) and placed as the table says. */
static const char *rows_of(PureJsonMessage *b, unsigned char *json, size_t size, size_t *at, uint32_t f, unsigned depth) {
    const PureModule *m = b->module;
    const PureField *field = &m->field[b->shape->field_start + f];
    size_t start = b->rows_size;
    const char *why = items_of(b, json, size, at, m->state_ref[field->rows], depth);
    if (why) return why;
    if (b->rows_base + b->rows_size > UINT32_MAX >> 2) return "limit";
    write_u32(b->table + 8 * f, (uint32_t)(b->rows_base + start));
    write_u32(b->table + 8 * f + 4, (uint32_t)(b->rows_size - start));
    return NULL;
}

/* An object's members from json[*at] (its opening brace), in the group named
 * path: a field's value, or a group's object. Strings stay where they are
 * (an escaped one is unescaped over itself, which never makes it longer).
 * NULL, or the error to answer. */
static const char *members(PureJsonMessage *b, unsigned char *json, size_t size, size_t *at, const char *path, size_t path_size,
                           unsigned depth) {
    const PureModule *m = b->module;
    if (*at == size || json[(*at)++] != '{') return "json";
    *at = pure_json_space(json, *at, size);
    for (bool first = true; *at < size && json[*at] != '}'; first = false) {
        if (!first) {
            if (json[(*at)++] != ',') return "json";
            *at = pure_json_space(json, *at, size);
        }
        size_t key_end = *at < size && json[*at] == '"' ? json_string_end(json + *at + 1, size - *at - 1) : 0;
        if (!key_end) return "json";
        const unsigned char *key = json + *at + 1;
        size_t key_size = key_end - 1;
        *at = pure_json_space(json, *at + 1 + key_end, size);
        if (*at == size || json[(*at)++] != ':') return "json";
        *at = pure_json_space(json, *at, size);
        bool group;
        uint32_t f = field_named(b, path, path_size, key, key_size, &group);
        if (f < b->shape->field_count && m->field[b->shape->field_start + f].type == PURE_FIELD_ROWS) {
            if (b->seen[f] || (b->given && b->given[f])) return "json_field";
            b->seen[f] = true;
            const char *why = rows_of(b, json, size, at, f, depth);
            if (why) return why;
            *at = pure_json_space(json, *at, size);
            continue;
        }
        if (f == b->shape->field_count && !group && b->lenient) { /* not the app's: skipped */
            if (!skip_value(json, size, at, depth)) return "json";
            *at = pure_json_space(json, *at, size);
            continue;
        }
        if (*at < size && json[*at] == '{') {
            if (!group) return f < b->shape->field_count ? "json_type" : "json_field";
            char inner[PURE_MODULE_NAME];
            size_t inner_size = path_size + !!path_size + key_size;
            memcpy(inner, path, path_size);
            if (path_size) inner[path_size] = '.';
            memcpy(inner + path_size + !!path_size, key, key_size);
            const char *why = members(b, json, size, at, inner, inner_size, depth + 1);
            if (why) return why;
            *at = pure_json_space(json, *at, size);
            continue;
        }
        if (f == b->shape->field_count) return group ? "json_type" : "json_field";
        if (b->seen[f] || (b->given && b->given[f])) return "json_field";
        b->seen[f] = true;
        const PureField *field = &m->field[b->shape->field_start + f];
        bool variable = field->type == PURE_FIELD_BYTES || field->type == PURE_FIELD_TEXT, integer, truth;
        size_t word = field->type == PURE_FIELD_BOOL ? json_bool(json + *at, size - *at, &truth) : 0;
        if (word) { /* any other value for a bool is of another type (below) */
            b->payload[field->offset] = truth;
            *at = pure_json_space(json, *at + word, size);
            continue;
        }
        if (*at < size && json[*at] == '"') {
            size_t end = json_string_end(json + *at + 1, size - *at - 1), written;
            if (!end) return "json";
            if (!variable) return "json_type";
            unsigned char *value = json + *at + 1;
            if (!memchr(value, '\\', end - 1)) written = end - 1;
            else if (!json_unescape(value, end - 1, value, &written)) return "json";
            if (field->type == PURE_FIELD_TEXT && !pure_utf8_valid((PureSpan){value, written})) return "json";
            write_u32(b->table + 8 * f, (uint32_t)(value - b->payload));
            write_u32(b->table + 8 * f + 4, (uint32_t)written);
            *at = pure_json_space(json, *at + 1 + end, size);
            continue;
        }
        size_t length = json_number(json + *at, size - *at, &integer);
        if (!length) {
            static const char *const words[] = {"true", "false", "null"};
            for (size_t w = 0; w < 3 && !length; ++w)
                if (size - *at >= strlen(words[w]) && !memcmp(json + *at, words[w], strlen(words[w]))) return "json_type";
            return *at < size && json[*at] == '[' ? "json_type" : "json";
        }
        if (variable || !pure_json_fixed(field, json + *at, length, b->payload)) return "json_type";
        *at = pure_json_space(json, *at + length, size);
    }
    if (*at == size) return "json";
    ++*at;
    return NULL;
}

const char *pure_json_message(PureJsonMessage *message, unsigned char *json, size_t size, size_t *at) {
    return members(message, json, size, at, "", 0, 0);
}
