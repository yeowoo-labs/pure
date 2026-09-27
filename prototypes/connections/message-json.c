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

/* A fixed field's value from its text (a query value or a JSON number) into
 * the prefix: a u32 in range, or a finite number. */
bool pure_json_fixed(const PureField *field, const unsigned char *text, size_t size, unsigned char *prefix) {
    bool integer;
    if (!size || json_number(text, size, &integer) != size || size > 40) return false;
    char digits[48];
    memcpy(digits, text, size);
    digits[size] = 0;
    if (field->type == PURE_FIELD_U32) {
        uint64_t n;
        if (!integer || !decimal(digits, UINT32_MAX, &n)) return false;
        write_u32(prefix + field->offset, (uint32_t)n);
        return true;
    }
    double value = strtod(digits, NULL);
    if (!isfinite(value)) return false;
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
        bool variable = field->type == PURE_FIELD_BYTES || field->type == PURE_FIELD_TEXT, integer;
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
