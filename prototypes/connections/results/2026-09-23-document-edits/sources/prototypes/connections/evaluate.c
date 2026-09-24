#include "program.h"
#include <stdint.h>

bool pure_region_span(PureRegion region, PureSpan source, PureSpan *out) {
    *out = (PureSpan){0};
    if (region.offset > source.size) return false;
    size_t length = region.to_end ? source.size - region.offset : region.length;
    if (length > source.size - region.offset) return false;
    *out = (PureSpan){length ? source.data + region.offset : NULL, length};
    return true;
}

static bool index_read(PureIndex index, PureSpan source, size_t *out) {
    if (!index.from_u32le) { *out = index.value; return true; }
    if (index.value > source.size || 4 > source.size - index.value) return false;
    const unsigned char *bytes = source.data + index.value;
    *out = (size_t)bytes[0] | (size_t)bytes[1] << 8 | (size_t)bytes[2] << 16 | (size_t)bytes[3] << 24;
    return true;
}

bool pure_program_splice(const PureProgram *p, PureSpan source, size_t *offset, size_t *remove) {
    return p->splice && index_read(p->splice_offset, source, offset) &&
           index_read(p->splice_remove, source, remove);
}

static bool resolve(PureRegion region, size_t step, PureSpan source,
                    const PureSpan *values, size_t count, PureSpan *out) {
    if (!step) return pure_region_span(region, source, out);
    if (step > count) return false;
    *out = values[step - 1];
    return true;
}

bool pure_program_input(const PureProgram *p, PureSpan source, PureSpan *write, PureSpan *reply) {
    *write = (PureSpan){0};
    *reply = (PureSpan){0};
    if (source.size < p->input_required || p->input_steps > PURE_DYNAMIC_LIMIT) return false;
    PureSpan values[PURE_DYNAMIC_LIMIT];
    for (size_t i = 0; i < p->input_steps; ++i) {
        const PureRangeStep *step = &p->steps[i];
        PureSpan parent;
        size_t offset, length;
        if (!resolve(step->source, step->source_step, source, values, i, &parent) ||
            !index_read(step->offset, source, &offset) || !index_read(step->length, source, &length) ||
            !pure_region_span((PureRegion){.offset = offset, .length = length}, parent, &values[i])) return false;
    }
    PureSpan stored, returned = {0};
    if (!resolve(p->write, p->write_step, source, values, p->input_steps, &stored) ||
        (p->reply_input && !resolve(p->reply, p->reply_step, source, values, p->input_steps, &returned))) return false;
    *write = stored;
    *reply = returned;
    return true;
}
