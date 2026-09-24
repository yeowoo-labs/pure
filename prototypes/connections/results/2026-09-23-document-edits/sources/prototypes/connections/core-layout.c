#include "runtime.h"
#include <stdio.h>

/* Host-side measurement tool; never linked into libpure-execution.a. */
int main(void) {
    printf("{\"domain_bytes\":%zu,\"work_slot_bytes\":%zu,\"ticket_bytes\":%zu,"
           "\"prepared_program_bytes\":%zu,\"pointer_bytes\":%zu}\n",
           sizeof(PureRuntime), sizeof(PureWork), sizeof(PureTicket),
           sizeof(PureProgram), sizeof(void *));
    return 0;
}
