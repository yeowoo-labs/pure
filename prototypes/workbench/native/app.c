#include "app.h"
#include "../../connections/store-port.h"
#include "../../connections/store.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>

_Static_assert(sizeof(double) == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "the current snapshot boundary requires IEEE binary64");

static const PureProgram connections = {.read = {.to_end = true}, .write = {.to_end = true}};
static const unsigned char marker[8] = {'P','U','R','E','S','0','0','1'};
enum { SNAPSHOT_HEADER = 44 };

static void release(void *memory, size_t size) { (void)size; free(memory); }
static bool allocate(void *context, size_t size, PureResource *out) {
    (void)context;
    void *memory = malloc(size);
    if (!memory) return false;
    *out = (PureResource){.body = {memory, size}, .allocation = memory,
        .allocation_size = size, .release = release};
    return true;
}
static uint64_t decode(const unsigned char *bytes, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}
static void encode(unsigned char *bytes, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

static PureResult read_value(void *context, const void *definition, const char *key,
                             size_t available, PureResource *out, int64_t *revision) {
    PureNativeApp *app = context;
    (void)definition;
    if (!strcmp(key, "load")) {
        PureHost store = pure_store_host(app->directory);
        return store.read(store.context, &connections, app->document, available, out, revision);
    }
    if (strcmp(key, "snapshot")) return PURE_INVALID;
    if (app->held) return PURE_BUSY;
    size_t size = SNAPSHOT_HEADER + 8u * app->plan->state_count;
    /* This host contract currently exports the complete typed snapshot. */
    if (size > available) return PURE_BUDGET;
    if (!allocate(NULL, size, out)) return PURE_ALLOCATION;
    unsigned char *bytes = out->allocation;
    memcpy(bytes, marker, sizeof(marker));
    memcpy(bytes + 8, app->plan->identity, 32);
    encode(bytes + 40, app->plan->state_count, 4);
    for (uint32_t i = 0; i < app->plan->state_count; ++i) {
        double value = pure_scalar_get_state(app->scalar, i);
        uint64_t bits;
        memcpy(&bits, &value, sizeof(bits));
        encode(bytes + SNAPSHOT_HEADER + i * 8, bits, 8);
    }
    *revision = app->revision;
    return PURE_OK;
}

static PureResult action(PureNativeApp *app, PureAction a) {
    if (a.phase == PURE_RESET) {
        if (app->held) return PURE_BUSY;
        pure_scalar_reset_state(app->scalar);
        return PURE_OK;
    }
    if (a.phase == PURE_FRAME) {
        app->last_scalar_error = pure_scalar_dispatch(app->scalar, 0, 1.0 / 60.0);
        return app->last_scalar_error ? PURE_INVALID : PURE_OK;
    }
    if (a.input >= app->plan->input_count || a.input >= 8) return PURE_INVALID;
    PureScalarInputKind kind = app->plan->inputs[a.input].kind;
    uint32_t bit = 1u << a.input, event;
    if (kind == PURE_VALUE) {
        if (a.phase != PURE_CHANGE) return PURE_INVALID;
        app->last_scalar_error = pure_scalar_dispatch_value(app->scalar, 33 + a.input, a.x);
        return app->last_scalar_error ? PURE_INVALID : PURE_OK;
    }
    if (kind == PURE_KEY) {
        if ((a.phase != PURE_DOWN && a.phase != PURE_UP) || a.source >= 32) return PURE_INVALID;
        uint32_t source = 1u << a.source, before = app->key_sources[a.input];
        if (a.phase == PURE_DOWN) {
            app->key_sources[a.input] |= source;
            app->held |= bit;
            if (before) return PURE_OK;
        } else {
            if (!(before & source)) return PURE_OK;
            app->key_sources[a.input] &= ~source;
            if (app->key_sources[a.input]) return PURE_OK;
            app->held &= ~bit;
        }
        event = a.input * 2 + (a.phase == PURE_DOWN ? 1u : 2u);
        app->last_scalar_error = pure_scalar_dispatch(app->scalar, event, 0);
    } else {
        if (a.phase > PURE_CANCEL) return PURE_INVALID;
        uint32_t slot = a.input;
        if (kind == PURE_ROUTED) {
            if (a.target >= app->plan->target_count || a.target >= 24 || app->plan->targets[a.target].input != a.input) return PURE_INVALID;
            slot = 8 + a.target;
        }
        bit = 1u << slot;
        if (a.phase == PURE_DOWN) {
            if (app->held & bit) return app->pointer_owners[slot] == a.source ? PURE_OK : PURE_BUSY;
            app->held |= bit;
            app->pointer_owners[slot] = a.source;
        } else {
            if (!(app->held & bit) || app->pointer_owners[slot] != a.source) return PURE_INVALID;
            if (a.phase == PURE_UP || a.phase == PURE_CANCEL) app->held &= ~bit;
        }
        event = a.phase == PURE_MOVE ? 17 + a.input : a.phase == PURE_CANCEL ? 25 + a.input
              : a.input * 2 + (a.phase == PURE_DOWN ? 1u : 2u);
        app->last_scalar_error = kind == PURE_ROUTED
            ? pure_scalar_dispatch_target(app->scalar, event, a.target, a.x, a.y)
            : pure_scalar_dispatch_point(app->scalar, event, a.x, a.y);
    }
    /* Input ownership follows the physical phases even if an authored handler
     * fails; its scalar writes remain atomic. Each routed target has its owner. */
    return app->last_scalar_error ? PURE_INVALID : PURE_OK;
}

static PureResult write_value(void *context, const void *definition, const char *key,
                              int64_t expected, PureSpan bytes, PureSpan *reply,
                              int64_t *revision) {
    PureNativeApp *app = context;
    (void)definition;
    *reply = (PureSpan){0};
    if (!strcmp(key, "save")) return pure_store_put(app->directory, app->document, expected, bytes, revision);
    *revision = app->revision;
    if (expected != app->revision) return PURE_CONFLICT;
    if (app->revision == INT64_MAX) return PURE_LIMIT;
    app->last_scalar_error = 0;
    if (!strcmp(key, "event")) {
        if (bytes.size != sizeof(PureAction)) return PURE_LIMIT;
        PureAction input;
        memcpy(&input, bytes.data, sizeof(input));
        PureResult status = action(app, input);
        if (status != PURE_OK) return status;
#ifdef PURE_NATIVE_AUTHORING
    } else if (!strcmp(key, "plan")) {
        if (bytes.size != sizeof(const PureScalarPlan *)) return PURE_LIMIT;
        if (app->held) return PURE_BUSY;
        const PureScalarPlan *candidate;
        memcpy(&candidate, bytes.data, sizeof(candidate));
        if (!candidate || !candidate->states || !candidate->inputs || !candidate->views ||
            candidate->state_count > 32 || candidate->input_count > 8 ||
            candidate->target_count > 24 || candidate->view_count > 24) return PURE_INVALID;
        app->last_scalar_error = pure_scalar_load(app->scalar, candidate, app->plan, NULL);
        if (app->last_scalar_error) return PURE_INVALID;
        app->plan = candidate;
        memset(app->pointer_owners, 0, sizeof(app->pointer_owners));
        memset(app->key_sources, 0, sizeof(app->key_sources));
#endif
    } else if (!strcmp(key, "restore")) {
        if (app->held) return PURE_BUSY;
        if (bytes.size != SNAPSHOT_HEADER + 8u * app->plan->state_count ||
            memcmp(bytes.data, marker, sizeof(marker)) || memcmp(bytes.data + 8, app->plan->identity, 32) ||
            decode(bytes.data + 40, 4) != app->plan->state_count) return PURE_INVALID;
        double values[32];
        for (uint32_t i = 0; i < app->plan->state_count; ++i) {
            uint64_t bits = decode(bytes.data + SNAPSHOT_HEADER + 8 * i, 8);
            memcpy(values + i, &bits, sizeof(bits));
        }
        app->last_scalar_error = pure_scalar_load(app->scalar, app->plan, NULL, values);
        if (app->last_scalar_error) return PURE_INVALID;
    } else return PURE_INVALID;
    *revision = ++app->revision;
    return PURE_OK;
}

PureResult pure_native_app_init(PureNativeApp *app, const PureScalarPlan *plan, void *memory,
                                size_t size, const char *directory, const char *key, size_t budget) {
    if (!app || !plan || !directory || !key || !*key || strlen(key) > 64 || plan->state_count > 32) return PURE_INVALID;
    for (const char *p = key; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return PURE_INVALID;
    PureScalar *scalar = pure_scalar_initialize(memory, size);
    if (!scalar) return PURE_INVALID;
    if (pure_scalar_load(scalar, plan, NULL, NULL)) return PURE_INVALID;
    PureResult status = pure_store_init(directory);
    if (status != PURE_OK) return status;
    *app = (PureNativeApp){.scalar = scalar, .plan = plan, .directory = directory};
    strcpy(app->document, key);
    PureHost host = {.context = app, .allocate = allocate, .read = read_value, .write = write_value};
    return pure_runtime_init(&app->execution, NULL, 0, NULL, host, budget) ? PURE_OK : PURE_INVALID;
}

static PureOutcome finish(PureNativeApp *app, PureTicket ticket) {
    PureOutcome result;
    while (!pure_runtime_result(ticket, &result)) {
        if (!pure_runtime_step(&app->execution)) return (PureOutcome){.status = PURE_INVALID};
    }
    return result;
}

PureOutcome pure_native_app_action(PureNativeApp *app, PureAction input) {
    PureWork work[1];
    if (!pure_runtime_bind(&app->execution, work, 1))
        return (PureOutcome){.status = PURE_BUSY};
    PureTicket ticket;
    PureResult status = pure_runtime_accept(&app->execution, PURE_INPUT_WRITE, "event", app->revision, sizeof(input), &ticket);
    PureOutcome result = {.status = status};
    if (status == PURE_OK) {
        memcpy(pure_runtime_ingress(ticket, NULL), &input, sizeof(input));
        pure_runtime_ready(ticket);
        result = finish(app, ticket);
        pure_runtime_release(ticket);
    }
    result.body = (PureSpan){0};
    if (!pure_runtime_bind(&app->execution, NULL, 0)) abort();
    return result;
}

static PureOutcome transfer(PureNativeApp *app, bool restore) {
    PureWork work[2];
    if (!pure_runtime_bind(&app->execution, work, 2))
        return (PureOutcome){.status = PURE_BUSY};
    PureTicket source, destination;
    PureResult status = pure_runtime_accept(&app->execution, PURE_INPUT_READ, restore ? "load" : "snapshot", 0, 0, &source);
    PureOutcome result = {.status = status};
    if (status != PURE_OK) goto done;
    result = finish(app, source);
    int64_t stored = result.revision;
    if (result.status == PURE_OK) {
        status = pure_runtime_forward(source, &app->execution, restore ? "restore" : "save",
                                      restore ? app->revision : app->stored_revision, &destination);
        pure_runtime_release(source);
        if (status != PURE_OK) { result = (PureOutcome){.status = status}; goto done; }
        result = finish(app, destination);
        pure_runtime_release(destination);
        if (result.status == PURE_OK) app->stored_revision = restore ? stored : result.revision;
    } else pure_runtime_release(source);
done:
    /* Bodies are consumed internally before this API returns. */
    result.body = (PureSpan){0};
    if (!pure_runtime_bind(&app->execution, NULL, 0)) abort();
    return result;
}
PureOutcome pure_native_app_save(PureNativeApp *app) { return transfer(app, false); }
PureOutcome pure_native_app_restore(PureNativeApp *app) { return transfer(app, true); }

#ifdef PURE_NATIVE_AUTHORING
PureResult pure_native_app_replace_plan(PureNativeApp *app, const PureScalarPlan *candidate) {
    if (!app || !candidate) return PURE_INVALID;
    if (app->held || app->execution.active || app->execution.retained) return PURE_BUSY;
    PureWork work[1];
    if (!pure_runtime_bind(&app->execution, work, 1)) return PURE_BUSY;
    PureTicket ticket;
    PureResult status = pure_runtime_accept(&app->execution, PURE_INPUT_WRITE,
                                            "plan", app->revision, sizeof(candidate), &ticket);
    if (status == PURE_OK) {
        memcpy(pure_runtime_ingress(ticket, NULL), &candidate, sizeof(candidate));
        pure_runtime_ready(ticket);
        PureOutcome result = finish(app, ticket);
        pure_runtime_release(ticket);
        status = result.status;
    }
    if (!pure_runtime_bind(&app->execution, NULL, 0)) abort();
    return status;
}
#endif
