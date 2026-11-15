/* Coverage fuzzing of the loader and the verifier: whatever bytes come, pure_module_load answers (it loads them or
 * names the rule that refuses them) and what it loaded is given back. Where a tier emits code as a module loads,
 * that is run too.
 *
 *   make fuzz && sh tests/fuzz.sh load MINUTES      (clang's libFuzzer, under ASan and UBSan; not part of make check) */
#include "module.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    PureModule module = {0};
    char error[160];
    if (pure_module_load(data, size, &module, error, sizeof(error)) == PURE_OK) pure_module_release(&module);
    return 0;
}
