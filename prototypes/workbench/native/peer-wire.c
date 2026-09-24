#include "peer-wire.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(double) == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "peer v1 requires IEEE binary64");

static void put(unsigned char *at, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) at[bytes - 1 - i] = (unsigned char)(value >> (8 * i));
}
static uint64_t get(const unsigned char *at, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | at[i];
    return value;
}
static void put_double(unsigned char *at, double value) {
    uint64_t bits;
    memcpy(&bits, &value, 8);
    put(at, bits, 8);
}
static double get_double(const unsigned char *at) {
    uint64_t bits = get(at, 8);
    double value;
    memcpy(&value, &bits, 8);
    return value;
}

static bool valid_action(const PureScalarPlan *plan, const PureAction *a, size_t bytes) {
    if (a->phase == PURE_FRAME || a->phase == PURE_RESET)
        return bytes == 12 && a->input == UINT32_MAX && a->target == UINT32_MAX;
    if (a->input >= plan->input_count || a->input >= 8) return false;
    PureScalarInputKind kind = plan->inputs[a->input].kind;
    if (kind == PURE_KEY)
        return bytes == 12 && (a->phase == PURE_DOWN || a->phase == PURE_UP) && a->target == UINT32_MAX;
    if (kind == PURE_VALUE)
        return bytes == 20 && a->phase == PURE_CHANGE && a->target == UINT32_MAX && isfinite(a->x);
    if (kind != PURE_POINT && kind != PURE_ROUTED) return false;
    if (bytes != 28 || a->phase > PURE_CANCEL || !isfinite(a->x) || !isfinite(a->y)) return false;
    if (kind == PURE_POINT) return a->target == UINT32_MAX;
    return a->target < plan->target_count && a->target < 24 &&
           plan->targets[a->target].input == a->input;
}

void pure_peer_hello(const PureNativeApp *app, unsigned char out[PURE_PEER_HELLO_BYTES]) {
    memcpy(out, "PUR1", 4);
    memcpy(out + 4, app->plan->identity, 32);
    out[36] = (unsigned char)app->plan->state_count;
    put(out + 37, (uint64_t)app->revision, 8);
    put(out + 45, (uint64_t)app->stored_revision, 8);
}
bool pure_peer_check_hello(const unsigned char *wire, size_t length, const PureScalarPlan *plan,
                           int64_t *revision, int64_t *stored_revision) {
    if (!wire || !plan || !revision || !stored_revision || length != PURE_PEER_HELLO_BYTES ||
        memcmp(wire, "PUR1", 4) || memcmp(wire + 4, plan->identity, 32) ||
        wire[36] != plan->state_count) return false;
    uint64_t local = get(wire + 37, 8), stored = get(wire + 45, 8);
    if (local > INT64_MAX || stored > INT64_MAX) return false;
    *revision = (int64_t)local; *stored_revision = (int64_t)stored;
    return true;
}

size_t pure_peer_encode_request(const PurePeerRequest *request, const PureScalarPlan *plan,
                                unsigned char out[PURE_PEER_REQUEST_MAX]) {
    if (!request || !plan || !out || request->operation > PURE_PEER_LOAD) return 0;
    out[0] = (unsigned char)request->operation;
    if (request->operation == PURE_PEER_STATE) return 1;
    if (request->expected < 0) return 0;
    put(out + 1, (uint64_t)request->expected, 8);
    if (request->operation != PURE_PEER_ACTION) return 9;
    const PureAction *a = &request->action;
    size_t length = 12;
    if (a->phase != PURE_FRAME && a->phase != PURE_RESET && a->input < plan->input_count) {
        PureScalarInputKind kind = plan->inputs[a->input].kind;
        if (kind == PURE_VALUE) length = 20;
        if (kind == PURE_POINT || kind == PURE_ROUTED) length = 28;
    }
    if (!valid_action(plan, a, length)) return 0;
    out[9] = a->input == UINT32_MAX ? 255 : (unsigned char)a->input;
    out[10] = (unsigned char)a->phase;
    out[11] = a->target == UINT32_MAX ? 255 : (unsigned char)a->target;
    if (length >= 20) put_double(out + 12, a->x);
    if (length == 28) put_double(out + 20, a->y);
    return length;
}

bool pure_peer_decode_request(const unsigned char *wire, size_t length, const PureScalarPlan *plan,
                              PurePeerRequest *out) {
    if (!wire || !plan || !out || !length || wire[0] > PURE_PEER_LOAD) return false;
    PurePeerRequest candidate = {.operation = (PurePeerOperation)wire[0]};
    if (candidate.operation == PURE_PEER_STATE) {
        if (length != 1) return false;
    } else {
        if (length < 9 || get(wire + 1, 8) > INT64_MAX) return false;
        candidate.expected = (int64_t)get(wire + 1, 8);
        if (candidate.operation != PURE_PEER_ACTION) {
            if (length != 9) return false;
        } else {
            if (length != 12 && length != 20 && length != 28) return false;
            candidate.action.input = wire[9] == 255 ? UINT32_MAX : wire[9];
            candidate.action.phase = wire[10];
            candidate.action.target = wire[11] == 255 ? UINT32_MAX : wire[11];
            if (length >= 20) candidate.action.x = get_double(wire + 12);
            if (length == 28) candidate.action.y = get_double(wire + 20);
            if (!valid_action(plan, &candidate.action, length)) return false;
        }
    }
    *out = candidate;
    return true;
}

/* These numbers belong to the wire version; do not serialize the C enum. */
static unsigned char wire_status(PureResult code) {
    switch (code) {
    case PURE_OK: return 0; case PURE_NOT_FOUND: return 1; case PURE_CONFLICT: return 2;
    case PURE_BUSY: return 3; case PURE_BUDGET: return 4; case PURE_LIMIT: return 5;
    case PURE_STORE_ERROR: return 6; case PURE_UNCERTAIN: return 7; case PURE_INVALID: return 8;
    case PURE_DENIED: return 9; case PURE_CLOSED: return 10; case PURE_CANCELLED: return 11;
    case PURE_ALLOCATION: return 12;
    }
    return 8;
}
size_t pure_peer_encode_reply(const PureNativeApp *app, PureResult status,
                              unsigned char out[PURE_PEER_REPLY_MAX]) {
    if (!app || !out || app->plan->state_count > 32 || app->revision < 0 || app->stored_revision < 0) return 0;
    out[0] = wire_status(status);
    put(out + 1, (uint64_t)app->revision, 8);
    put(out + 9, (uint64_t)app->stored_revision, 8);
    put(out + 17, app->last_scalar_error, 4);
    put(out + 21, app->held, 4);
    out[25] = (unsigned char)app->plan->state_count;
    for (uint32_t i = 0; i < app->plan->state_count; ++i)
        put_double(out + 26 + i * 8, pure_scalar_get_state(app->scalar, i));
    return 26 + 8u * app->plan->state_count;
}
bool pure_peer_decode_reply(const unsigned char *wire, size_t length, const PureScalarPlan *plan,
                            PurePeerReply *out) {
    if (!wire || !plan || !out || length < 26 || plan->state_count > 32 || wire[0] > 12 ||
        wire[25] != plan->state_count || length != 26 + 8u * plan->state_count) return false;
    uint64_t revision = get(wire + 1, 8), stored = get(wire + 9, 8);
    if (revision > INT64_MAX || stored > INT64_MAX) return false;
    PurePeerReply candidate = {.status = wire[0], .revision = (int64_t)revision,
        .stored_revision = (int64_t)stored, .scalar_error = (uint32_t)get(wire + 17, 4),
        .held = (uint32_t)get(wire + 21, 4), .state_count = plan->state_count};
    for (uint32_t i = 0; i < plan->state_count; ++i) {
        candidate.states[i] = get_double(wire + 26 + i * 8);
        if (!isfinite(candidate.states[i])) return false;
    }
    *out = candidate;
    return true;
}
