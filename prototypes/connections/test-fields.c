#include "program.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static const char get[] = "on get { let o = store.read(request.key); reply(o, o.body); }";
static const char put[] = "on put { let s = store.write(request.key, request.revision, request.body); reply(s); }";
static const char layout[] = "layout Envelope { marker: bytes[1]; start: u32le; count: u32le; text: bytes; }";
static PureProgram parse(const char *source) {
    PureProgram p;
    char error[160];
    if (!pure_program_parse(source, strlen(source), &p, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); exit(1);
    }
    return p;
}
static void invalid(const char *source) {
    PureProgram p;
    memset(&p, 0x5a, sizeof(p));
    unsigned char before[sizeof(p)];
    memcpy(before, &p, sizeof(p));
    char error[160];
    CHECK(!pure_program_parse(source, strlen(source), &p, error, sizeof(error)));
    CHECK(*error && !memcmp(before, &p, sizeof(p)));
}
static void encode(unsigned char *at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) at[i] = (unsigned char)(value >> (8 * i));
}
int main(void) {
    char source[8000];
    snprintf(source, sizeof(source), "%s %s on put { let input = view(Envelope, request.body); "
        "let alias = input; let start = alias.start; let text = alias.text; "
        "let selected = range(text, start, alias.count); let result = selected; "
        "let saved = store.write(request.key, request.revision, result); reply(saved, selected); }", layout, get);
    PureProgram p = parse(source);
    CHECK(p.input_steps == 1 && p.input_required == 9 && p.write_step == 1 && p.reply_step == 1);
    CHECK(p.steps[0].source.offset == 9 && p.steps[0].source.to_end);
    CHECK(p.steps[0].offset.value == 1 && p.steps[0].offset.from_u32le);
    CHECK(p.steps[0].length.value == 5 && p.steps[0].length.from_u32le);
    unsigned char bytes[609];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i * 13);
    PureSpan stored, returned;
    for (size_t n = 0; n < 9; ++n) {
        CHECK(!pure_program_input(&p, (PureSpan){bytes, n}, &stored, &returned));
        CHECK(!stored.size && !returned.size);
    }
    const uint32_t offsets[] = {0, 1, 255, 257, 599, 600, 601, UINT32_MAX};
    const uint32_t counts[] = {0, 1, 2, 260, 600, UINT32_MAX};
    size_t comparisons = 0;
    for (size_t i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i)
        for (size_t j = 0; j < sizeof(counts) / sizeof(*counts); ++j) {
            uint32_t start = offsets[i], count = counts[j];
            encode(bytes + 1, start);
            encode(bytes + 5, count);
            bool expected = start <= 600 && count <= 600 - start;
            CHECK(pure_program_input(&p, (PureSpan){bytes, sizeof(bytes)}, &stored, &returned) == expected);
            if (expected) {
                CHECK(stored.size == count && returned.size == count && stored.data == returned.data);
                CHECK(!count || stored.data == bytes + 9 + start);
            } else CHECK(!stored.size && !returned.size);
            ++comparisons;
        }
    snprintf(source, sizeof(source), "%s %s on put { let input = view(Envelope, request.body); "
        "let first = range(input.text, input.start, input.count); let second = range(first, 1, 2); "
        "let saved = store.write(request.key, request.revision, second); reply(saved, second); }", layout, get);
    p = parse(source);
    CHECK(p.input_steps == 2 && p.steps[1].source_step == 1);
    encode(bytes + 1, 3); encode(bytes + 5, 4);
    CHECK(pure_program_input(&p, (PureSpan){bytes, sizeof(bytes)}, &stored, &returned));
    CHECK(stored.size == 2 && stored.data == bytes + 13);
    encode(bytes + 5, 2);
    CHECK(!pure_program_input(&p, (PureSpan){bytes, sizeof(bytes)}, &stored, &returned));
    snprintf(source, sizeof(source), "%s %s on put { let input = view(Envelope, request.body); "
        "let unused = range(input.text, input.start, input.count); "
        "let saved = store.write(request.key, request.revision, input.text); reply(saved); }", layout, get);
    p = parse(source);
    encode(bytes + 1, UINT32_MAX);
    CHECK(!pure_program_input(&p, (PureSpan){bytes, sizeof(bytes)}, &stored, &returned));

    snprintf(source, sizeof(source), "%s on get { let o = store.read(request.key); "
        "let input = view(Envelope, o.body); reply(o, input.text); } %s", layout, put);
    p = parse(source);
    CHECK(p.read.to_end && p.read.offset == 9 && p.read_required == 9 && !p.input_steps);
    snprintf(source, sizeof(source), "%s %s on put { let input = view(Envelope, request.body); "
        "let selection = range(input.text, 2, 3); let saved = store.write(request.key, request.revision, selection); reply(saved); }", layout, get);
    p = parse(source);
    CHECK(!p.input_steps && p.write.offset == 11 && p.write.length == 3 && p.input_required == 14);
    snprintf(source, sizeof(source), "layout Empty { body: bytes; } %s on put { let input = view(Empty, request.body); "
        "let saved = store.write(request.key, request.revision, input.body); reply(saved, input.body); }", get);
    p = parse(source);
    CHECK(pure_program_input(&p, (PureSpan){0}, &stored, &returned) && !stored.size && !returned.size);

    const char *bad_layout[] = {
        "layout E {}", "layout E { x: bytes; y: u32le; }", "layout E { x: u32le; x: bytes; }",
        "layout E { x: bytes[1449985]; }", "layout E { x: bytes[1449984]; y: u32le; }",
        "layout E { x: bytes[184467440737095516160]; }", "layout E { x: u64le; }",
        "layout E { x: u32le; } layout E { y: bytes; }"
    };
    for (size_t i = 0; i < sizeof(bad_layout) / sizeof(*bad_layout); ++i) {
        snprintf(source, sizeof(source), "%s %s %s", bad_layout[i], get, put); invalid(source);
    }
    const char *bad_input[] = {
        "let input = view(Missing, request.body);",
        "let input = view(Envelope, request.body); let x = input.missing;",
        "let input = view(Envelope, request.body); let x = range(input.text, input.text, 1);",
        "let short = range(request.body, 0, 8); let input = view(Envelope, short);",
        "let input = view(Envelope, request.body); let x = range(input.text, input.start, 20); let y = view(Envelope, x);"
    };
    for (size_t i = 0; i < sizeof(bad_input) / sizeof(*bad_input); ++i) {
        snprintf(source, sizeof(source), "%s %s on put { %s let saved = store.write(request.key, request.revision, request.body); reply(saved); }", layout, get, bad_input[i]);
        invalid(source);
    }
    snprintf(source, sizeof(source), "%s %s on put { let input = view(Envelope, request.body); "
        "let saved = store.write(request.key, request.revision, input.start); reply(saved); }", layout, get);
    invalid(source);
    snprintf(source, sizeof(source), "%s on get { let o = store.read(request.key); let input = view(Envelope, o.body); "
        "let x = range(input.text, input.start, input.count); reply(o, x); } %s", layout, put);
    invalid(source);
    for (size_t count = 8; count <= 9; ++count) {
        snprintf(source, sizeof(source), "%s %s on put { let input = view(Envelope, request.body); ", layout, get);
        for (size_t i = 0; i < count; ++i) {
            size_t used = strlen(source);
            snprintf(source + used, sizeof(source) - used, "let v%zu = range(input.text, input.start, input.count); ", i);
        }
        size_t used = strlen(source);
        snprintf(source + used, sizeof(source) - used, "let saved = store.write(request.key, request.revision, input.text); reply(saved); }");
        if (count == 8) CHECK(parse(source).input_steps == 8); else invalid(source);
    }
    for (size_t count = 4; count <= 5; ++count) {
        source[0] = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t used = strlen(source);
            snprintf(source + used, sizeof(source) - used, "layout L%zu { body: bytes; } ", i);
        }
        size_t used = strlen(source);
        snprintf(source + used, sizeof(source) - used, "%s %s", get, put);
        if (count == 4) (void)parse(source); else invalid(source);
    }
    for (size_t count = 16; count <= 17; ++count) {
        strcpy(source, "layout L { ");
        for (size_t i = 0; i < count; ++i) {
            size_t used = strlen(source);
            snprintf(source + used, sizeof(source) - used, "f%zu: u32le; ", i);
        }
        size_t used = strlen(source);
        snprintf(source + used, sizeof(source) - used, "} %s %s", get, put);
        if (count == 16) (void)parse(source); else invalid(source);
    }
    printf("{\"fields\":\"named-layouts-unaligned-u32le-tail-dynamic-ranges-aliases-failures-limits\","
           "\"data_boundary_cases\":%zu}\n", comparisons);
    return 0;
}
