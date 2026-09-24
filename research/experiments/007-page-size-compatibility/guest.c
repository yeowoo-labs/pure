/* PID 1 in the 4 KiB AArch64 test initramfs. Keep the storage assertions and
 * the real prepared document CLI in the same guest with no host file mapping. */
#define main paged_store_test_main
#include "../../../prototypes/connections/test-paged-store.c"
#undef main
#include <sys/reboot.h>

int pure_edit_main(int argc, char **argv);

static void check_native_app(void) {
    const char *store = "/tmp/pure-4k-app";
    const char *input = "/tmp/pure-4k-input";
    const char *replacement = "/tmp/pure-4k-replacement";
    FILE *file = fopen(input, "wb");
    assert(file);
    unsigned char block[4096];
    memset(block, 'A', sizeof(block));
    for (unsigned i = 0; i < 256; ++i)
        assert(fwrite(block, 1, sizeof(block), file) == sizeof(block));
    assert(fclose(file) == 0);

    char *first[] = {"pure-edit-fixed", "--store", (char *)store, "edit",
                     "note", "0", "0", "0", (char *)input};
    assert(pure_edit_main(9, first) == 0);
    PureStoredView view;
    assert(pure_store_get(store, "note", 1048576, &view) == PURE_OK);
    assert(view.revision == 1 && view.body.size == 1048576);
    for (size_t i = 0; i < view.body.size; ++i) assert(view.body.data[i] == 'A');
    pure_store_release(&view);

    file = fopen(replacement, "wb");
    assert(file && fputc('Z', file) == 'Z' && fclose(file) == 0);
    char *second[] = {"pure-edit-fixed", "--store", (char *)store, "edit",
                      "note", "1", "1048575", "1", (char *)replacement};
    assert(pure_edit_main(9, second) == 0);
    assert(pure_store_get(store, "note", 1048576, &view) == PURE_OK);
    assert(view.revision == 2 && view.body.size == 1048576);
    for (size_t i = 0; i < view.body.size - 1; ++i) assert(view.body.data[i] == 'A');
    assert(view.body.data[view.body.size - 1] == 'Z');
    pure_store_release(&view);

    assert(pure_edit_main(9, second) == 3);
    assert(pure_store_get(store, "note", 1048576, &view) == PURE_OK);
    assert(view.revision == 2 && view.body.data[view.body.size - 1] == 'Z');
    pure_store_release(&view);
}

int main(void) {
    long os_page = sysconf(_SC_PAGESIZE);
    printf("PURE_GUEST_OS_PAGE=%ld\n", os_page);
    fflush(stdout);
    if (os_page != 4096) return 2;
    int result = paged_store_test_main();
    printf("PURE_4K_STORE_RESULT=%d\n", result);
    fflush(stdout);
    if (!result) {
        check_native_app();
        puts("PURE_4K_APP_RESULT=0");
        fflush(stdout);
        reboot(RB_POWER_OFF);
    }
    return result;
}
