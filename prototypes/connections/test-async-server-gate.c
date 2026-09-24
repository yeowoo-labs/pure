#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <stdlib.h>
#include <unistd.h>

/* Test-only gate at the file sync point. The production server never links
 * this file or the PURE_STORE_TEST hook. Only the storage worker reaches it. */
int pure_store_test_fault(PureStoreStage stage) {
    static int entered;
    if (stage != PURE_BEFORE_FILE_SYNC || entered) return 0;
    entered = 1;
    const char *signal_fd = getenv("PURE_TEST_GATE_SIGNAL_FD");
    const char *release_fd = getenv("PURE_TEST_GATE_RELEASE_FD");
    if (!signal_fd || !release_fd) return 0;
    int signal = atoi(signal_fd), release = atoi(release_fd);
    const char byte = 1;
    if (write(signal, &byte, 1) != 1) _exit(91);
    char released;
    if (read(release, &released, 1) != 1) _exit(92);
    return 0;
}
