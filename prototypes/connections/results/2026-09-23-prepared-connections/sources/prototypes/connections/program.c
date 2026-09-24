#include "program.h"
#include "store.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { NAME = 256, NUMBER, SYMBOLS = 32 };
typedef struct {
    char name[32];
    bool result;
    PureRegion region;
} Symbol;
typedef struct {
    const char *source;
    size_t length, at, line, token_line;
    int token;
    char name[32];
    size_t number;
    char *error;
    size_t error_size;
    bool failed, put, effect;
    Symbol symbols[SYMBOLS];
    size_t count;
    PureProgram program;
} Parser;

static bool reject(Parser *p, const char *message) {
    if (!p->failed && p->error_size) snprintf(p->error, p->error_size, "line %zu: %s", p->token_line, message);
    p->failed = true;
    return false;
}

static bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool next(Parser *p) {
    while (p->at < p->length) {
        char c = p->source[p->at];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (c == '\n') ++p->line;
            ++p->at;
        } else if (c == '/' && p->at + 1 < p->length && p->source[p->at + 1] == '/') {
            while (p->at < p->length && p->source[p->at] != '\n') ++p->at;
        } else break;
    }
    p->token_line = p->line;
    p->name[0] = 0;
    p->token = 0;
    if (p->at == p->length) return true;
    char c = p->source[p->at++];
    if (letter(c)) {
        size_t start = p->at - 1;
        while (p->at < p->length && (letter(p->source[p->at]) ||
               (p->source[p->at] >= '0' && p->source[p->at] <= '9'))) ++p->at;
        size_t size = p->at - start;
        if (size >= sizeof(p->name)) return reject(p, "name exceeds 31 bytes");
        memcpy(p->name, p->source + start, size);
        p->name[size] = 0;
        p->token = NAME;
    } else if (c >= '0' && c <= '9') {
        size_t value = (size_t)(c - '0');
        while (p->at < p->length && p->source[p->at] >= '0' && p->source[p->at] <= '9') {
            unsigned digit = (unsigned)(p->source[p->at++] - '0');
            if (value > (SIZE_MAX - digit) / 10) return reject(p, "integer overflow");
            value = value * 10 + digit;
        }
        p->number = value;
        p->token = NUMBER;
    } else if (c && strchr("{}().,=;", c)) p->token = (unsigned char)c;
    else return reject(p, "unexpected character");
    return true;
}

static bool is_word(Parser *p, const char *word) {
    return p->token == NAME && !strcmp(p->name, word);
}

static bool word(Parser *p, const char *expected) {
    return is_word(p, expected) ? next(p) : reject(p, expected);
}

static bool token(Parser *p, int expected) {
    return p->token == expected ? next(p) : reject(p, "unexpected token");
}

static Symbol *find(Parser *p, const char *name) {
    for (size_t i = 0; i < p->count; ++i)
        if (!strcmp(name, p->symbols[i].name)) return &p->symbols[i];
    return NULL;
}

static bool field(Parser *p, const char *object, const char *name) {
    return word(p, object) && token(p, '.') && word(p, name);
}

static bool reference(Parser *p, PureRegion *out) {
    if (is_word(p, "request")) {
        if (!p->put) return reject(p, "request.body is only available in on put");
        *out = (PureRegion){.whole = true};
        return field(p, "request", "body");
    }
    Symbol *s = p->token == NAME ? find(p, p->name) : NULL;
    if (!s) return reject(p, "unknown byte source");
    bool result = s->result;
    *out = s->region;
    if (!next(p)) return false;
    if (!result) return true;
    if (p->put) return reject(p, "write result has no body; use the input value");
    *out = (PureRegion){.whole = true};
    return token(p, '.') && word(p, "body");
}

static bool view(Parser *p, PureRegion *out) {
    if (!is_word(p, "range")) return reference(p, out);
    PureRegion parent;
    size_t offset, length;
    if (!next(p) || !token(p, '(') || !reference(p, &parent) || !token(p, ',')) return false;
    if (p->token != NUMBER) return reject(p, "range offset must be a constant integer");
    offset = p->number;
    if (!next(p) || !token(p, ',')) return false;
    if (p->token != NUMBER) return reject(p, "range length must be a constant integer");
    length = p->number;
    if (!next(p) || !token(p, ')')) return false;
    size_t bound = parent.whole ? PURE_OBJECT_LIMIT : parent.length;
    if (offset > bound || length > bound - offset) return reject(p, "range exceeds its source bound");
    *out = (PureRegion){.offset = parent.offset + offset, .length = length};
    size_t required = out->offset + out->length;
    size_t *minimum = p->put ? &p->program.input_required : &p->program.read_required;
    if (required > *minimum) *minimum = required;
    ++p->program.range_definitions;
    return true;
}

static bool effect(Parser *p, Symbol *out) {
    if (p->effect) return reject(p, "only one storage effect is supported per handler");
    if (!word(p, "store") || !token(p, '.') || !word(p, p->put ? "write" : "read") ||
        !token(p, '(') || !field(p, "request", "key")) return false;
    if (p->put) {
        if (!token(p, ',') || !field(p, "request", "revision") || !token(p, ',') ||
            !reference(p, &p->program.write)) return false;
    }
    if (!token(p, ')')) return false;
    out->result = true;
    p->effect = true;
    return true;
}

static bool handler(Parser *p) {
    p->count = 0;
    p->effect = false;
    if (!token(p, '{')) return false;
    while (is_word(p, "let")) {
        if (p->put && p->effect) return reject(p, "all views must precede store.write; reply must follow it");
        if (p->count == SYMBOLS) return reject(p, "handler exceeds 32 definitions");
        if (!next(p)) return false;
        if (p->token != NAME || is_word(p, "request") || is_word(p, "store") || is_word(p, "range"))
            return reject(p, "expected a non-reserved definition name");
        if (find(p, p->name)) return reject(p, "duplicate definition");
        Symbol symbol = {0};
        strcpy(symbol.name, p->name);
        if (!next(p) || !token(p, '=')) return false;
        if (is_word(p, "store")) {
            if (!effect(p, &symbol)) return false;
        } else {
            if (!view(p, &symbol.region)) return false;
            ++p->program.view_definitions;
        }
        if (!token(p, ';')) return false;
        p->symbols[p->count++] = symbol;
    }
    if (!p->effect) return reject(p, "handler requires its storage effect");
    if (!word(p, "reply") || !token(p, '(')) return false;
    Symbol *result = p->token == NAME ? find(p, p->name) : NULL;
    if (!result || !result->result) return reject(p, "reply requires this handler's storage result");
    if (!next(p)) return false;
    PureRegion body = {0};
    bool has_body = p->token == ',';
    if (has_body && (!next(p) || !reference(p, &body))) return false;
    if (!token(p, ')') || !token(p, ';') || !token(p, '}')) return false;
    if (p->put) {
        p->program.reply = body;
        p->program.reply_input = has_body;
    } else p->program.read = body;
    return true;
}

bool pure_program_parse(const char *source, size_t length, PureProgram *out,
                        char *error, size_t error_size) {
    Parser p = {.source = source, .length = length, .line = 1, .token_line = 1,
                .error = error, .error_size = error_size};
    if (error_size) error[0] = 0;
    if (length > PURE_PROGRAM_LIMIT) return reject(&p, "program exceeds 16 KiB");
    if (!next(&p)) return false;
    unsigned handlers = 0;
    while (p.token) {
        if (!word(&p, "on")) return false;
        if (!is_word(&p, "get") && !is_word(&p, "put")) return reject(&p, "expected get or put handler");
        p.put = is_word(&p, "put");
        unsigned bit = p.put ? 2 : 1;
        if (handlers & bit) return reject(&p, "duplicate handler");
        handlers |= bit;
        if (!next(&p) || !handler(&p)) return false;
    }
    if (handlers != 3) return reject(&p, "both get and put handlers are required");
    *out = p.program; /* Invalid preparation never changes the installed plan. */
    return true;
}

bool pure_program_load(const char *path, PureProgram *out, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error_size) snprintf(error, error_size, "cannot open program");
        return false;
    }
    char source[PURE_PROGRAM_LIMIT + 1];
    size_t length = fread(source, 1, sizeof(source), file);
    bool bad = ferror(file) != 0;
    if (fclose(file)) bad = true;
    if (bad) {
        if (error_size) snprintf(error, error_size, "cannot read program");
        return false;
    }
    return pure_program_parse(source, length, out, error, error_size);
}

bool pure_region_span(PureRegion region, PureSpan source, PureSpan *out) {
    *out = (PureSpan){0};
    if (region.whole) { *out = source; return true; }
    if (region.offset > source.size || region.length > source.size - region.offset) return false;
    *out = (PureSpan){region.length ? source.data + region.offset : NULL, region.length};
    return true;
}
