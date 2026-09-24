#include "program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static const char get[] = "on get { let o = store.read(request.key); reply(o, o.body); }";
static const char put[] = "on put { let s = store.write(request.key, request.revision, request.body); reply(s); }";
static PureProgram parse(const char *source) {
    PureProgram program;
    char error[160];
    if (!pure_program_parse(source, strlen(source), &program, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); exit(1);
    }
    return program;
}
static void invalid(const char *source) {
    PureProgram program;
    memset(&program, 0x5a, sizeof(program));
    unsigned char original[sizeof(program)];
    memcpy(original, &program, sizeof(program));
    char error[160];
    CHECK(!pure_program_parse(source, strlen(source), &program, error, sizeof(error)));
    CHECK(*error && !memcmp(original, &program, sizeof(program)));
}
int main(void) {
    char source[4096];
    snprintf(source, sizeof(source), "%s %s", get, put);
    PureProgram plan = parse(source);
    CHECK(plan.read.whole && plan.write.whole && !plan.reply_input);
    CHECK(!plan.view_definitions && !plan.read_required && !plan.input_required);
    snprintf(source, sizeof(source), "%s on put { let a = range(request.body, 2, 10); "
        "let b = range(a, 3, 4); let alias = b; "
        "let saved = store.write(request.key, request.revision, alias); reply(saved, b); }", get);
    plan = parse(source);
    CHECK(plan.write.offset == 5 && plan.write.length == 4 && !plan.write.whole);
    CHECK(plan.reply.offset == 5 && plan.reply.length == 4 && plan.reply_input);
    CHECK(plan.input_required == 12 && plan.range_definitions == 2 && plan.view_definitions == 3);
    /* Ordered reference views keep even unused bounds failures observable. */
    unsigned char bytes[160];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i * 7);
    size_t comparisons = 0;
    for (size_t a = 0; a < 4; ++a) for (size_t b = 0; b < 6; ++b)
        for (size_t c = 0; c <= b; ++c) for (size_t d = 0; d <= b - c; ++d) {
            size_t unused_end = (a + b + c) % 12;
            snprintf(source, sizeof(source), "on get { let o = store.read(request.key); "
                "let unused = range(o.body, %zu, 0); let first = range(o.body, %zu, %zu); "
                "let second = range(first, %zu, %zu); reply(o, second); } %s",
                unused_end, a, b, c, d, put);
            plan = parse(source);
            for (size_t n = 0; n < 16; ++n) {
                bool reference_ok = unused_end <= n && a <= n && b <= n - a;
                PureSpan selected;
                bool prepared_ok = n >= plan.read_required &&
                    pure_region_span(plan.read, (PureSpan){bytes, n}, &selected);
                CHECK(reference_ok == prepared_ok);
                if (reference_ok) {
                    CHECK(selected.size == d);
                    CHECK(!d || !memcmp(selected.data, bytes + a + c, d));
                }
                ++comparisons;
            }
        }
    snprintf(source, sizeof(source), "on get { let o = store.read(request.key); reply(o); } %s", put);
    plan = parse(source);
    CHECK(!plan.read.whole && !plan.read.length);
    const char *bad_get[] = {
        "on get { let o = store.read(request.key); let x = range(o.body, 0, 2); let y = range(x, 2, 1); reply(o, y); }",
        "on get { let o = store.read(request.key); let x = range(o.body, 184467440737095516160, 1); reply(o, x); }",
        "on get { let o = store.read(request.key); let x = range(o.body, 1449984, 1); reply(o, x); }",
        "on get { let o = store.read(request.key); let o = o.body; reply(o); }",
        "on get { let o = store.read(request.key); let x = x; reply(o, x); }",
        "on get { let o = store.read(request.key); let x = store.read(request.key); reply(x); }",
        "on get { let o = store.read(request.key); reply(o, request.body); }",
        "on get { let o = store.read(request.key); let x = o.body; reply(x); }",
        "on get { let o = store.read(request.key); reply(o.body); }",
        "on get { let o = store.read(request.key); reply(o); let x = o.body; }",
        "on get { let o = store.read(request.key); reply(o);", "on get {}"
    };
    for (size_t i = 0; i < sizeof(bad_get) / sizeof(*bad_get); ++i) {
        snprintf(source, sizeof(source), "%s %s", bad_get[i], put); invalid(source);
    }
    const char *bad_put[] = {
        "on put { let s = store.write(request.key, request.revision, request.body); let b = range(request.body, 0, 1); reply(s, b); }",
        "on put { let s = store.write(request.key, request.revision, request.body); reply(s, s.body); }",
        "on put { let s = store.read(request.key); reply(s); }"
    };
    for (size_t i = 0; i < sizeof(bad_put) / sizeof(*bad_put); ++i) {
        snprintf(source, sizeof(source), "%s %s", get, bad_put[i]); invalid(source);
    }
    invalid(get);
    snprintf(source, sizeof(source), "%s %s %s", get, get, put); invalid(source);
    snprintf(source, sizeof(source), "%s %s garbage", get, put); invalid(source);
    snprintf(source, sizeof(source), "on get { let o = store.read(request.key); ");
    for (size_t i = 0; i < 32; ++i) {
        size_t used = strlen(source);
        snprintf(source + used, sizeof(source) - used, "let x%zu = o.body; ", i);
    }
    size_t used = strlen(source);
    snprintf(source + used, sizeof(source) - used, "reply(o); } %s", put); invalid(source);
    char oversized[PURE_PROGRAM_LIMIT + 1];
    memset(oversized, ' ', sizeof(oversized));
    char error[160];
    CHECK(!pure_program_parse(oversized, sizeof(oversized), &plan, error, sizeof(error)));
    const char embedded[] = "on get\0{}";
    CHECK(!pure_program_parse(embedded, sizeof(embedded) - 1, &plan, error, sizeof(error)));
    PureSpan selected;
    CHECK(pure_region_span((PureRegion){0}, (PureSpan){0}, &selected) && !selected.data && !selected.size);
    printf("{\"program\":\"prepare-views-preserve-guards-types-effects-and-atomic-install\","
           "\"boundary_semantic_comparisons\":%zu}\n", comparisons);
    return 0;
}
