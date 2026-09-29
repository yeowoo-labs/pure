/* A module run twice from the same inputs: once as it loads, once with none
 * of its walks and folds ranged (every op's range let go). A range may only
 * skip items the walk's or fold's test would refuse, so the two must end
 * the same: state for state, item for item, byte for byte. For
 * tests/check.py.
 *
 *   module-probe FILE.pmod INPUT...
 *
 * Each INPUT is a key input's name, pressed. The module must not read the
 * clock (now): the two runs are not at the same time. Prints a line of
 * JSON: the inputs applied, the ops ranged, and each number state's value;
 * exits 1 at the first difference, naming the state. */
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *bytes; size_t size; } Bytes;

static bool keep(void *context, PureSpan span) {
    Bytes *b = context;
    unsigned char *grown = realloc(b->bytes, b->size + span.size + 1);
    if (!grown) return false;
    memcpy(grown + b->size, span.data, span.size);
    b->bytes = grown, b->size += span.size;
    return true;
}

static bool same_text(const PureDraft *a, const PureDraft *b) {
    size_t size = pure_draft_size(a);
    Bytes x = {0}, y = {0};
    bool same = size == pure_draft_size(b) && pure_draft_read(a, 0, size, keep, &x) == PURE_OK &&
                pure_draft_read(b, 0, size, keep, &y) == PURE_OK && x.size == y.size &&
                (!x.size || !memcmp(x.bytes, y.bytes, x.size));
    free(x.bytes), free(y.bytes);
    return same;
}

static bool same_cell(PureCell a, PureCell b) {
    return !memcmp(&a.num, &b.num, sizeof(a.num)) && a.size == b.size && (!a.size || !memcmp(a.text, b.text, a.size));
}

/* The first state the two apps hold differently, or -1. */
static int differs(const PureModule *m, const PureApp *a, const PureApp *b) {
    for (uint32_t s = 0; s < m->states; ++s) {
        bool same = true;
        switch (m->state_type[s]) {
        case PURE_STATE_TEXT: same = same_text(a->texts[s], b->texts[s]); break;
        case PURE_STATE_LIST: {
            size_t count = pure_list_count(a->lists[s]);
            unsigned fields = m->record[m->state_ref[s]].count;
            same = count == pure_list_count(b->lists[s]);
            for (size_t at = 0; same && at < count; ++at)
                for (unsigned f = 0; same && f < fields; ++f)
                    same = same_cell(pure_list_get(a->lists[s], at, f), pure_list_get(b->lists[s], at, f));
            break;
        }
        default: same = !memcmp(&a->values[s], &b->values[s], sizeof(double)); break;
        }
        if (!same) return (int)s;
    }
    return -1;
}

int main(int argc, char **argv) {
    PureModule modules[2] = {0};
    PureApp apps[2] = {0};
    char error[256];
    unsigned ranged = 0;
    if (argc < 2) {
        fputs("module-probe FILE.pmod INPUT...\n", stderr);
        return 2;
    }
    for (int k = 0; k < 2; ++k) {
        if (pure_module_read(argv[1], &modules[k], error, sizeof(error)) != PURE_OK) {
            fprintf(stderr, "module-probe: %s\n", error);
            return 2;
        }
        for (uint32_t pc = 0; pc < modules[k].code_count; ++pc) {
            if (!k) ranged += modules[k].code[pc].range != 0;
            else modules[k].code[pc].range = 0;
        }
        if (pure_app_start(&apps[k], &modules[k], (PureData){0}) != PURE_OK) {
            fprintf(stderr, "module-probe: the app does not start\n");
            return 2;
        }
    }
    for (int i = 2; i < argc; ++i) {
        int input = pure_module_input(&modules[0], argv[i]);
        if (input < 0) {
            fprintf(stderr, "module-probe: no input %s\n", argv[i]);
            return 2;
        }
        PureResult results[2];
        for (int k = 0; k < 2; ++k) results[k] = pure_app_input(&apps[k], (uint32_t)input, PURE_EDGE_DOWN);
        int state = differs(&modules[0], &apps[0], &apps[1]);
        if (results[0] != results[1] || state >= 0) {
            fprintf(stderr, "module-probe: after %s (input %d): %s differs ranged and not (results %d, %d)\n", argv[i],
                    i - 1, state >= 0 ? modules[0].state_names[state] : "the result", results[0], results[1]);
            return 1;
        }
    }
    printf("{\"inputs\":%d,\"ranged\":%u,\"states\":{", argc - 2, ranged);
    for (uint32_t s = 0, shown = 0; s < modules[0].states; ++s)
        if (modules[0].state_type[s] == PURE_STATE_NUM || modules[0].state_type[s] == PURE_STATE_INDEX)
            printf("%s\"%s\":%.17g", shown++ ? "," : "", modules[0].state_names[s], apps[0].values[s]);
    printf("}}\n");
    for (int k = 0; k < 2; ++k) {
        pure_app_stop(&apps[k]);
        pure_module_release(&modules[k]);
    }
    return 0;
}
