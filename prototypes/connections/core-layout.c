#include "runtime.h"
#include <stdio.h>

/* Host-side measurement tool; never linked into libpure-execution.a. */
int main(void) {
    printf("{\"domain_bytes\":%zu,\"work_slot_bytes\":%zu,\"ticket_bytes\":%zu,"
           "\"pointer_bytes\":%zu",
           sizeof(PureRuntime), sizeof(PureWork), sizeof(PureTicket), sizeof(void *));
#ifdef PURE_ASYNC_EFFECTS
    printf(",\"effect_bytes\":%zu", sizeof(PureEffect));
#endif
    puts("}");
    return 0;
}
