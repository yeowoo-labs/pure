#include "peer-wire.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Codec test double; no VM instance or TLS process is needed to check the
 * on-wire representation and malformed-frame rejection. */
double pure_scalar_get_state(PureScalar *scalar, uint32_t id) {
    (void)scalar;
    return id == 0 ? 12.5 : -0.0;
}
int main(void) {
    PureScalarState states[] = {{"x", 0}, {"y", 0}};
    PureScalarInput inputs[] = {
        {"key", "Space", NULL, PURE_KEY},
        {"point", NULL, "shape", PURE_POINT},
        {"routed", NULL, NULL, PURE_ROUTED},
        {"value", NULL, NULL, PURE_VALUE},
    };
    PureScalarTarget targets[] = {{2, 0, 1, "shape"}};
    PureScalarPlan plan = {.state_count = 2, .input_count = 4, .target_count = 1,
                           .states = states, .inputs = inputs, .targets = targets};
    for (unsigned i = 0; i < 32; ++i) plan.identity[i] = (unsigned char)(i + 1);
    PureNativeApp app = {.plan = &plan, .revision = 7, .stored_revision = 3};
    unsigned char hello[PURE_PEER_HELLO_BYTES];
    pure_peer_hello(&app, hello);
    int64_t revision = -1, stored = -1;
    assert(pure_peer_check_hello(hello, sizeof(hello), &plan, &revision, &stored));
    assert(revision == 7 && stored == 3 && !memcmp(hello, "PUR1", 4));
    hello[4] ^= 1;
    assert(!pure_peer_check_hello(hello, sizeof(hello), &plan, &revision, &stored));
    hello[4] ^= 1;
    assert(!pure_peer_check_hello(hello, sizeof(hello) - 1, &plan, &revision, &stored));

    unsigned char wire[PURE_PEER_REQUEST_MAX];
    PurePeerRequest request = {.operation = PURE_PEER_STATE};
    assert(pure_peer_encode_request(&request, &plan, wire) == 1);
    PurePeerRequest decoded;
    assert(pure_peer_decode_request(wire, 1, &plan, &decoded) && decoded.operation == PURE_PEER_STATE);
    assert(!pure_peer_decode_request(wire, 2, &plan, &decoded));
    request = (PurePeerRequest){.operation = PURE_PEER_SAVE, .expected = 0x0102030405060708LL};
    assert(pure_peer_encode_request(&request, &plan, wire) == 9);
    assert(wire[1] == 1 && wire[8] == 8);
    assert(pure_peer_decode_request(wire, 9, &plan, &decoded) && decoded.expected == request.expected);
    wire[1] = 255;
    assert(!pure_peer_decode_request(wire, 9, &plan, &decoded));

    request = (PurePeerRequest){.operation = PURE_PEER_ACTION, .expected = 7,
        .action = {.input = 0, .phase = PURE_DOWN, .target = UINT32_MAX}};
    assert(pure_peer_encode_request(&request, &plan, wire) == 12);
    assert(pure_peer_decode_request(wire, 12, &plan, &decoded) && decoded.action.input == 0);
    request.action = (PureAction){.input = 3, .phase = PURE_CHANGE, .target = UINT32_MAX, .x = 12.5};
    assert(pure_peer_encode_request(&request, &plan, wire) == 20);
    assert(pure_peer_decode_request(wire, 20, &plan, &decoded) && decoded.action.x == 12.5);
    memset(wire + 12, 255, 8);
    assert(!pure_peer_decode_request(wire, 20, &plan, &decoded));
    request.action = (PureAction){.input = 1, .phase = PURE_MOVE, .target = UINT32_MAX, .x = 100, .y = -0.0};
    assert(pure_peer_encode_request(&request, &plan, wire) == 28);
    assert(pure_peer_decode_request(wire, 28, &plan, &decoded) && decoded.action.x == 100 && signbit(decoded.action.y));
    request.action = (PureAction){.input = 2, .phase = PURE_UP, .target = 0, .x = 5, .y = 6};
    assert(pure_peer_encode_request(&request, &plan, wire) == 28);
    assert(pure_peer_decode_request(wire, 28, &plan, &decoded) && decoded.action.target == 0);
    wire[11] = 1;
    assert(!pure_peer_decode_request(wire, 28, &plan, &decoded));
    request.action = (PureAction){.input = UINT32_MAX, .phase = PURE_FRAME, .target = UINT32_MAX};
    assert(pure_peer_encode_request(&request, &plan, wire) == 12);
    assert(pure_peer_decode_request(wire, 12, &plan, &decoded) && decoded.action.phase == PURE_FRAME);

    unsigned char reply[PURE_PEER_REPLY_MAX];
    size_t length = pure_peer_encode_reply(&app, PURE_CONFLICT, reply);
    assert(length == 42 && reply[0] == 2 && reply[25] == 2);
    PurePeerReply decoded_reply;
    assert(pure_peer_decode_reply(reply, length, &plan, &decoded_reply));
    assert(decoded_reply.revision == 7 && decoded_reply.stored_revision == 3);
    assert(decoded_reply.states[0] == 12.5 && signbit(decoded_reply.states[1]));
    reply[25] = 3;
    assert(!pure_peer_decode_reply(reply, length, &plan, &decoded_reply));
    reply[25] = 2;
    assert(!pure_peer_decode_reply(reply, length - 1, &plan, &decoded_reply));
    reply[0] = 255;
    assert(!pure_peer_decode_reply(reply, length, &plan, &decoded_reply));
    puts("peer wire: bounded identity, revisions, actions, finite states and malformed frames pass");
    return 0;
}
