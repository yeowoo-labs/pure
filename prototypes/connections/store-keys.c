#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Key listing for the stores that publish each object as obj-KEY (the flat
 * and paged formats). Linked only by hosts that browse documents, so the
 * default CLI and HTTP hosts do not carry it. */
static bool valid_key(const char *key) {
    size_t length = strlen(key);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

PureResult pure_store_keys(const char *path, bool (*visit)(void *, const char *key),
                           void *context) {
    int descriptor = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *directory = descriptor >= 0 ? fdopendir(descriptor) : NULL;
    if (!directory) {
        if (descriptor >= 0) close(descriptor);
        return PURE_STORE_ERROR;
    }
    PureResult result = PURE_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = PURE_STORE_ERROR; break; }
        if (strncmp(entry->d_name, "obj-", 4) || !valid_key(entry->d_name + 4)) continue;
        if (!visit(context, entry->d_name + 4)) break;
    }
    if (closedir(directory)) result = PURE_STORE_ERROR;
    return result;
}
