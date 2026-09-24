#ifndef PURE_PEER_WIRE_H
#define PURE_PEER_WIRE_H
#include "app.h"

/* Version 1 is a boundary format, not a native struct or a runtime core ABI.
 * Every TLS record is still a byte stream: peer.c prefixes these payloads
 * with a two-byte big-endian length. One request is outstanding per session. */
enum { PURE_PEER_HELLO_BYTES = 53, PURE_PEER_REQUEST_MAX = 28,
       PURE_PEER_REPLY_MAX = 26 + 32 * 8 };
typedef enum { PURE_PEER_STATE, PURE_PEER_ACTION, PURE_PEER_SAVE, PURE_PEER_LOAD } PurePeerOperation;
typedef struct {
    PurePeerOperation operation;
    int64_t expected;
    PureAction action;
} PurePeerRequest;
typedef struct {
    uint8_t status;
    int64_t revision, stored_revision;
    uint32_t scalar_error, held, state_count;
    double states[32];
} PurePeerReply;

void pure_peer_hello(const PureNativeApp *, unsigned char out[PURE_PEER_HELLO_BYTES]);
bool pure_peer_check_hello(const unsigned char *, size_t, const PureScalarPlan *,
                           int64_t *revision, int64_t *stored_revision);
size_t pure_peer_encode_request(const PurePeerRequest *, const PureScalarPlan *,
                                unsigned char out[PURE_PEER_REQUEST_MAX]);
bool pure_peer_decode_request(const unsigned char *, size_t, const PureScalarPlan *, PurePeerRequest *);
size_t pure_peer_encode_reply(const PureNativeApp *, PureResult,
                              unsigned char out[PURE_PEER_REPLY_MAX]);
bool pure_peer_decode_reply(const unsigned char *, size_t, const PureScalarPlan *, PurePeerReply *);
#endif
