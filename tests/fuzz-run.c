/* Coverage fuzzing of events on a loaded module: an input is a module and up to sixteen messages for it,
 *
 *   [module's length, 4 bytes] [module] { [input, 1 byte] [payload's length, 2 bytes] [payload] }
 *
 * (lengths little-endian; an input's number is taken modulo the module's inputs). The module is loaded; an app
 * starts on it without a store, a small work budget (100,000 instructions an event) and memory budget (1 MiB); each
 * message is delivered, whatever comes of the one before; then every state and view is shown as pure-run shows
 * them (its own code, its output thrown away). A message may fail; nothing may crash, read outside what it was
 * given or leak, and no typed buffer's list may end an event not held as one (the app's buffers_missed). The module and each payload are copied into blocks of their exact sizes, so a read past either's
 * end is seen, and they are aligned as a host's buffers are.
 *
 *   make fuzz && sh tests/fuzz.sh run MINUTES       (PURE_JIT=0 PURE_BATCH=0 for the interpreter alone) */
#define main pure_run_main
#include "../cli/pure-run.c"
#undef main

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc, (void)argv;
    return freopen("/dev/null", "w", stdout) ? 0 : 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint32_t length;
    if (size < 4) return 0;
    memcpy(&length, data, 4);
    if (length > size - 4) return 0;
    unsigned char *bytes = malloc(length ? length : 1), *payloads[16];
    unsigned events = 0;
    if (!bytes) return 0;
    memcpy(bytes, data + 4, length);
    PureModule module = {0};
    char error[160];
    if (pure_module_load(bytes, length, &module, error, sizeof(error)) == PURE_OK) {
        PureApp app;
        memset(&app, 0, sizeof(app));
        if (pure_app_start(&app, &module, (PureData){0}) == PURE_OK) {
            pure_app_work(&app, 100000);
            pure_app_memory(&app, 1 << 20);
            const uint8_t *at = data + 4 + length, *end = data + size;
            while (events < 16 && end - at >= 3 && module.inputs) {
                uint32_t input = at[0] % module.inputs;
                size_t given = (size_t)at[1] | (size_t)at[2] << 8;
                at += 3;
                if (given > (size_t)(end - at)) given = (size_t)(end - at);
                if (!(payloads[events] = malloc(given ? given : 1))) break;
                memcpy(payloads[events], at, given);
                (void)pure_app_message(&app, input, (PureSpan){payloads[events], given});
                at += given, ++events;
            }
            show(&app, NULL);
            if (app.buffers_missed) abort(); /* a typed buffer's list let go holding items and not held as one */
            pure_app_stop(&app);
        }
        pure_module_release(&module);
    }
    while (events) free(payloads[--events]);
    free(bytes);
    return 0;
}
