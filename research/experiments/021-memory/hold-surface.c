/* A surface that holds a snapshot until the server stops: a reader that
 * never lets go, for run.py's held case (research/experiments/021-memory).
 * It holds the first snapshot whose number view VIEW is at least AT (the
 * first snapshot at all without them), and lets every other one go.
 *
 *   pure-serve ... --surface hold-surface.so [-- VIEW AT] */
#include "surface.h"
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

int pure_surface_run(const PureSurface *s, int argc, char **argv) {
    static PureApp reader;
    int view = argc > 2 ? pure_module_view(s->module, argv[1]) : -1;
    double at = argc > 2 ? atof(argv[2]) : 0, value = 0;
    PureSnapshot *held = NULL;
    while (!s->stopping(s->host)) {
        struct pollfd wait = {s->changes, POLLIN, 0};
        if (poll(&wait, 1, 20) > 0 && (wait.revents & POLLIN)) {
            uint64_t saves;
            if (read(s->changes, &saves, sizeof(saves)) < 0) { /* cleared already */ }
        }
        if (held) continue;
        PureSnapshot *read_now = s->read(s->host, &reader);
        if (!read_now) continue;
        if (view < 0 || (pure_app_view(&reader, (uint32_t)view, &value) && value >= at)) held = read_now;
        else s->release(s->host, read_now);
    }
    if (held) s->release(s->host, held);
    return 0;
}
