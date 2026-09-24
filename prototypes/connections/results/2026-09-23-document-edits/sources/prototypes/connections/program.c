#include "program.h"
#include "store.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { NAME = 256, NUMBER, SYMBOLS = 32, LAYOUTS = 4, FIELDS = 16 };
typedef enum { BYTE_VALUE, RESULT_VALUE, LAYOUT_VALUE, INDEX_VALUE } ValueKind;
typedef struct {
    char name[32];
    ValueKind kind;
    PureRegion region;
    size_t step, layout;
    PureIndex index;
} Symbol;
typedef struct {
    char name[32];
    size_t offset, length;
    bool number, tail;
} Field;
typedef struct {
    char name[32];
    Field fields[FIELDS];
    size_t count, prefix;
    bool tail;
} Layout;
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
    size_t count, layout_count;
    Layout layouts[LAYOUTS];
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
    } else if (c && strchr("{}().,=;:[]", c)) p->token = (unsigned char)c;
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

static bool require_bytes(Parser *p, const Symbol *value) {
    return value->kind == BYTE_VALUE || reject(p, "expected bytes; select a byte field");
}

static void require_length(Parser *p, size_t required) {
    size_t *minimum = p->put ? &p->program.input_required : &p->program.read_required;
    if (required > *minimum) *minimum = required;
}

static Layout *find_layout(Parser *p, const char *name) {
    for (size_t i = 0; i < p->layout_count; ++i)
        if (!strcmp(name, p->layouts[i].name)) return &p->layouts[i];
    return NULL;
}

static bool layout_definition(Parser *p) {
    if (p->layout_count == LAYOUTS) return reject(p, "program exceeds 4 layouts");
    if (!next(p)) return false;
    if (p->token != NAME || find_layout(p, p->name)) return reject(p, "expected a unique layout name");
    Layout *layout = &p->layouts[p->layout_count];
    strcpy(layout->name, p->name);
    if (!next(p) || !token(p, '{')) return false;
    while (p->token != '}') {
        if (layout->count == FIELDS) return reject(p, "layout exceeds 16 fields");
        if (layout->tail) return reject(p, "variable bytes must be the last field");
        if (p->token != NAME) return reject(p, "expected a field name");
        for (size_t i = 0; i < layout->count; ++i)
            if (!strcmp(layout->fields[i].name, p->name)) return reject(p, "duplicate field");
        Field *f = &layout->fields[layout->count];
        strcpy(f->name, p->name);
        f->offset = layout->prefix;
        if (!next(p) || !token(p, ':')) return false;
        if (is_word(p, "u32le")) {
            f->number = true;
            f->length = 4;
            if (!next(p)) return false;
        } else if (is_word(p, "bytes")) {
            if (!next(p)) return false;
            if (p->token == '[') {
                if (!next(p)) return false;
                if (p->token != NUMBER) return reject(p, "fixed bytes requires a constant size");
                f->length = p->number;
                if (!next(p) || !token(p, ']')) return false;
            } else { f->tail = true; layout->tail = true; }
        } else return reject(p, "supported field types: u32le, bytes[N], bytes");
        if (f->length > PURE_OBJECT_LIMIT - layout->prefix) return reject(p, "layout exceeds object bound");
        layout->prefix += f->length;
        ++layout->count;
        if (!token(p, ';')) return false;
    }
    if (!layout->count) return reject(p, "empty layout");
    ++p->layout_count;
    return token(p, '}');
}

static bool reference(Parser *p, Symbol *out) {
    if (is_word(p, "request")) {
        if (!p->put) return reject(p, "request.body is only available in on put");
        *out = (Symbol){.kind = BYTE_VALUE, .region = {.to_end = true}};
        return field(p, "request", "body");
    }
    Symbol *symbol = p->token == NAME ? find(p, p->name) : NULL;
    if (!symbol) return reject(p, "unknown value");
    *out = *symbol;
    if (!next(p)) return false;
    if (p->token != '.') return true;
    if (!next(p)) return false;
    if (symbol->kind == RESULT_VALUE) {
        if (p->put) return reject(p, "write result has no body; use the input value");
        *out = (Symbol){.kind = BYTE_VALUE, .region = {.to_end = true}};
        return word(p, "body");
    }
    if (symbol->kind != LAYOUT_VALUE || p->token != NAME) return reject(p, "expected a layout field");
    Layout *layout = &p->layouts[symbol->layout];
    Field *f = NULL;
    for (size_t i = 0; i < layout->count; ++i)
        if (!strcmp(p->name, layout->fields[i].name)) f = &layout->fields[i];
    if (!f) return reject(p, "unknown layout field");
    size_t offset = symbol->region.offset + f->offset;
    *out = (Symbol){.kind = f->number ? INDEX_VALUE : BYTE_VALUE};
    if (f->number) out->index = (PureIndex){.value = offset, .from_u32le = true};
    else out->region = (PureRegion){.offset = offset,
        .length = f->tail ? (symbol->region.to_end ? 0 : symbol->region.length - f->offset) : f->length,
        .to_end = f->tail && symbol->region.to_end};
    return next(p);
}

static bool byte_reference(Parser *p, Symbol *out) {
    return reference(p, out) && require_bytes(p, out);
}

static bool index_value(Parser *p, PureIndex *out) {
    if (p->token == NUMBER) {
        *out = (PureIndex){.value = p->number};
        return next(p);
    }
    Symbol value;
    if (!reference(p, &value)) return false;
    if (value.kind != INDEX_VALUE) return reject(p, "range requires an integer or u32le field");
    *out = value.index;
    return true;
}

static bool layout_view(Parser *p, Symbol *out) {
    if (!next(p) || !token(p, '(')) return false;
    Layout *layout = p->token == NAME ? find_layout(p, p->name) : NULL;
    if (!layout) return reject(p, "unknown layout");
    if (!next(p) || !token(p, ',')) return false;
    Symbol source;
    if (!byte_reference(p, &source) || !token(p, ')')) return false;
    if (source.step) return reject(p, "layout view requires a statically located source");
    size_t bound = source.region.to_end ? PURE_OBJECT_LIMIT - source.region.offset : source.region.length;
    if (layout->prefix > bound) return reject(p, "layout exceeds source bound");
    require_length(p, source.region.offset + layout->prefix);
    *out = (Symbol){.kind = LAYOUT_VALUE, .region = source.region, .layout = (size_t)(layout - p->layouts)};
    if (!layout->tail) { out->region.length = layout->prefix; out->region.to_end = false; }
    return true;
}

static bool view(Parser *p, Symbol *out) {
    if (is_word(p, "view")) return layout_view(p, out);
    if (!is_word(p, "range")) return reference(p, out);
    Symbol parent;
    PureIndex offset, length;
    if (!next(p) || !token(p, '(') || !byte_reference(p, &parent) || !token(p, ',') ||
        !index_value(p, &offset) || !token(p, ',') || !index_value(p, &length) || !token(p, ')')) return false;
    bool dynamic = parent.step || offset.from_u32le || length.from_u32le;
    *out = (Symbol){.kind = BYTE_VALUE};
    if (dynamic) {
        if (!p->put) return reject(p, "data-dependent ranges currently require PUT input");
        if (p->program.input_steps == PURE_DYNAMIC_LIMIT) return reject(p, "input exceeds 8 dynamic ranges");
        if ((!offset.from_u32le && offset.value > PURE_OBJECT_LIMIT) ||
            (!length.from_u32le && length.value > PURE_OBJECT_LIMIT)) return reject(p, "range exceeds object bound");
        PureRangeStep *step = &p->program.steps[p->program.input_steps++];
        *step = (PureRangeStep){.source = parent.region, .source_step = parent.step,
                                .offset = offset, .length = length};
        out->step = p->program.input_steps;
    } else {
        size_t bound = parent.region.to_end ? PURE_OBJECT_LIMIT - parent.region.offset : parent.region.length;
        if (offset.value > bound || length.value > bound - offset.value) return reject(p, "range exceeds its source bound");
        out->region = (PureRegion){.offset = parent.region.offset + offset.value, .length = length.value};
        require_length(p, out->region.offset + out->region.length);
    }
    ++p->program.range_definitions;
    return true;
}

static bool effect(Parser *p, Symbol *out) {
    if (p->effect) return reject(p, "only one storage effect is supported per handler");
    if (!word(p, "store") || !token(p, '.')) return false;
    bool splice = p->put && is_word(p, "splice");
    if (!word(p, p->put ? (splice ? "splice" : "write") : "read") ||
        !token(p, '(') || !field(p, "request", "key")) return false;
    if (p->put) {
        Symbol value;
        if (!token(p, ',') || !field(p, "request", "revision") || !token(p, ',')) return false;
        if (splice) {
            if (!index_value(p, &p->program.splice_offset) || !token(p, ',') ||
                !index_value(p, &p->program.splice_remove) || !token(p, ',')) return false;
            if ((!p->program.splice_offset.from_u32le && p->program.splice_offset.value > PURE_OBJECT_LIMIT) ||
                (!p->program.splice_remove.from_u32le && p->program.splice_remove.value > PURE_OBJECT_LIMIT))
                return reject(p, "splice exceeds object bound");
            p->program.splice = true;
        }
        if (!byte_reference(p, &value)) return false;
        p->program.write = value.region;
        p->program.write_step = value.step;
    }
    if (!token(p, ')')) return false;
    out->kind = RESULT_VALUE;
    p->effect = true;
    return true;
}

static bool handler(Parser *p) {
    p->count = 0;
    p->effect = false;
    if (!token(p, '{')) return false;
    while (is_word(p, "let")) {
        if (p->put && p->effect) return reject(p, "all views must precede the storage effect; reply must follow it");
        if (p->count == SYMBOLS) return reject(p, "handler exceeds 32 definitions");
        if (!next(p)) return false;
        if (p->token != NAME || is_word(p, "request") || is_word(p, "store") ||
            is_word(p, "range") || is_word(p, "view")) return reject(p, "expected a non-reserved definition name");
        if (find(p, p->name)) return reject(p, "duplicate definition");
        char name[32];
        strcpy(name, p->name);
        Symbol symbol = {0};
        if (!next(p) || !token(p, '=')) return false;
        if (is_word(p, "store")) {
            if (!effect(p, &symbol)) return false;
        } else {
            if (!view(p, &symbol)) return false;
            ++p->program.view_definitions;
        }
        if (!token(p, ';')) return false;
        strcpy(symbol.name, name);
        p->symbols[p->count++] = symbol;
    }
    if (!p->effect) return reject(p, "handler requires its storage effect");
    if (!word(p, "reply") || !token(p, '(')) return false;
    Symbol *result = p->token == NAME ? find(p, p->name) : NULL;
    if (!result || result->kind != RESULT_VALUE) return reject(p, "reply requires this handler's storage result");
    if (!next(p)) return false;
    Symbol body = {0};
    bool has_body = p->token == ',';
    if (has_body && (!next(p) || !byte_reference(p, &body))) return false;
    if (!token(p, ')') || !token(p, ';') || !token(p, '}')) return false;
    if (p->put) {
        p->program.reply = body.region;
        p->program.reply_step = body.step;
        p->program.reply_input = has_body;
    } else p->program.read = body.region;
    return true;
}

bool pure_program_parse(const char *source, size_t length, PureProgram *out,
                        char *error, size_t error_size) {
    Parser p = {.source = source, .length = length, .line = 1, .token_line = 1,
                .error = error, .error_size = error_size};
    if (error_size) error[0] = 0;
    if (length > PURE_PROGRAM_LIMIT) return reject(&p, "program exceeds 16 KiB");
    if (!next(&p)) return false;
    while (is_word(&p, "layout")) if (!layout_definition(&p)) return false;
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
