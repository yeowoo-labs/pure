#ifndef PURE_SURFACE_H
#define PURE_SURFACE_H
#include "module.h"

/* A surface shows an app to people (Vega's display, say): a library that
 * pure-serve maps and runs in a thread of its own beside its connection
 * threads (--surface LIBRARY [-- ARGUMENTS]). It reads the app as of the
 * last save, as a connection thread reads views, and gives it events that
 * the owner applies with the requests, one save a round. So a node is one
 * process with one owner of its states, whether people or programs use it,
 * and a surface reads states where they are, with no HTTP or JSON between.
 *
 * The library exports
 *
 *     int pure_surface_run(const PureSurface *, int argc, char **argv);
 *
 * and calls the app model (core/module.h) as pure-serve exports it: one
 * copy of that code in the process, the host's. argv[0] is the library.
 * pure_surface_run returns when stopping says so (the changes descriptor
 * becomes readable then too), or when it is done; the server serves on. */
enum { PURE_SURFACE_VERSION = 1 };

typedef struct PureSurfaceHost PureSurfaceHost;
typedef struct {
    unsigned version;
    size_t app_size, module_size; /* sizeof(PureApp), sizeof(PureModule): a surface built for another layout refuses */
    const PureModule *module;
    PureSurfaceHost *host;
    /* An eventfd, readable after each save that changed what views show and
     * when the server stops: poll it with the surface's own descriptors, and
     * read its 8 bytes to clear it. */
    int changes;
    /* The app as of the last save, shown in reader (a zeroed app the first
     * time, the same one after): its views, rows and texts read as
     * core/module.h says while the snapshot is held. Give it back with
     * release. NULL before the first save. */
    PureSnapshot *(*read)(PureSurfaceHost *, PureApp *reader);
    void (*release)(PureSurfaceHost *, PureSnapshot *);
    /* An event, applied by the owner with the next save: a key input pressed
     * and released, or text typed into a text input. Returns once it was
     * applied and saved (PURE_OK), or why it was not. */
    PureResult (*press)(PureSurfaceHost *, uint32_t input);
    PureResult (*type)(PureSurfaceHost *, uint32_t input, PureSpan text);
    bool (*stopping)(PureSurfaceHost *);
} PureSurface;

typedef int (*PureSurfaceRun)(const PureSurface *, int argc, char **argv);
#endif
