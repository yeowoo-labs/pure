CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build

.PHONY: all core async-core editor fixed-editor fixed-server fixed-tui source-tui compiler mirror window server async-server document-peer document-server check check-policy check-policy-peer check-portable check-portable-tui check-paged-portable check-tree check-module check-portable-peer check-native-checkpoint check-mirror check-core check-async check-async-server check-async-service-scale check-profile check-fixed-profile check-fixed-server check-service-scale check-tui-profile check-source-tui-profile check-tui check-source-tui check-release check-micro-memory check-hybrid check-flat-export check-copy-range check-peer-profile check-document-server-profile check-window-profile check-document-peer check-document-server FORCE
CORE = runtime.c
STORE ?= store.c
FIXED_PROGRAM ?= examples/editor.pure
HOST = $(STORE) store-port.c evaluate.c
# Document browsers list keys through the store. The tree store keeps its own
# catalog; per-object file stores share a directory listing.
KEYS = $(if $(filter store-tree.c,$(STORE)),,store-keys.c)
HEADERS = runtime.h program.h store.h store-port.h state.h flow.h result.h
all: core fixed-editor
core: $(BUILD)/libpure-execution.a
async-core: $(BUILD)/libpure-execution-async.a
editor: $(BUILD)/pure-edit
fixed-editor: $(BUILD)/pure-edit-fixed
fixed-server: $(BUILD)/pure-connections-fixed
fixed-tui: $(BUILD)/pure-edit-tui-fixed
source-tui: $(BUILD)/pure-edit-tui
compiler: $(BUILD)/pure-compile
mirror: $(BUILD)/pure-mirror
window: $(BUILD)/pure-edit-window
server: $(BUILD)/pure-connections
async-server: $(BUILD)/pure-connections-async
document-peer: $(BUILD)/pure-document-peer
document-server: $(BUILD)/pure-document-server

$(BUILD):
	mkdir -p $@

$(BUILD)/pure-connections: server.c $(HOST) flow.c program.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS server.c $(HOST) flow.c program.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/pure-connections-async: server.c $(HOST) flow.c program.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS -DPURE_ASYNC_EFFECTS -DPURE_HTTP_ASYNC server.c $(HOST) flow.c program.c $(CORE) $(LDFLAGS) -pthread $(LDLIBS) -o $@

$(BUILD)/pure-connections-async-gated: server.c $(HOST) flow.c program.c $(CORE) test-async-server-gate.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS -DPURE_ASYNC_EFFECTS -DPURE_HTTP_ASYNC -DPURE_STORE_TEST server.c $(HOST) flow.c program.c $(CORE) test-async-server-gate.c $(LDFLAGS) -pthread $(LDLIBS) -o $@

$(BUILD)/pure-connections-fixed: server.c $(HOST) flow.c $(CORE) $(HEADERS) $(BUILD)/embedded-plan.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS -DPURE_EMBEDDED_PROGRAM -I$(BUILD) server.c $(HOST) flow.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/pure-edit: edit.c $(HOST) program.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) edit.c $(HOST) program.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/pure-compile: compile-program.c program.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) compile-program.c program.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/embedded-plan.h: FORCE $(FIXED_PROGRAM) $(BUILD)/pure-compile
	$(BUILD)/pure-compile $(FIXED_PROGRAM) $@

$(BUILD)/pure-edit-fixed: edit.c $(HOST) $(CORE) $(HEADERS) $(BUILD)/embedded-plan.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_EMBEDDED_PROGRAM -I$(BUILD) edit.c $(HOST) $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/storage-plan.h: FORCE examples/storage.pure $(BUILD)/pure-compile
	$(BUILD)/pure-compile examples/storage.pure $@

$(BUILD)/pure-mirror: mirror.c $(HOST) $(CORE) $(HEADERS) $(BUILD)/storage-plan.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(BUILD) mirror.c $(HOST) $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/pure-edit-tui-fixed: edit-tui.c $(HOST) $(KEYS) $(CORE) $(HEADERS) $(BUILD)/embedded-plan.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_EMBEDDED_PROGRAM -I$(BUILD) edit-tui.c $(HOST) $(KEYS) $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/pure-edit-tui: edit-tui.c $(HOST) $(KEYS) $(CORE) program.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) edit-tui.c $(HOST) $(KEYS) $(CORE) program.c $(LDFLAGS) $(LDLIBS) -o $@

# TLS/socket code is linked only when a document peer is explicitly selected.
$(BUILD)/pure-document-peer: document-peer.c ../common/tls-link.c ../common/tls-link.h $(HOST) program.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) document-peer.c ../common/tls-link.c $(HOST) program.c $(CORE) $(LDFLAGS) -lssl -lcrypto $(LDLIBS) -o $@

$(BUILD)/pure-document-server: document-peer.c ../common/tls-link.c ../common/tls-link.h $(HOST) program.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_PEER_SERVER_ONLY document-peer.c ../common/tls-link.c $(HOST) program.c $(CORE) $(LDFLAGS) -lssl -lcrypto $(LDLIBS) -o $@

# GTK is selected only for the document window. The core, CLI and server do
# not need its headers, libraries, display server or runtime dependencies.
$(BUILD)/pure-edit-window: edit-window.c $(HOST) program.c $(CORE) $(HEADERS) | $(BUILD)
	pkg-config --atleast-version=4.10 gtk4
	$(CC) $(CPPFLAGS) $(CFLAGS) $$(pkg-config --cflags gtk4) edit-window.c $(HOST) program.c $(CORE) $(LDFLAGS) $$(pkg-config --libs gtk4) $(LDLIBS) -o $@

$(BUILD)/runtime.o: runtime.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c runtime.c -o $@

$(BUILD)/runtime-async.o: runtime.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_ASYNC_EFFECTS -c runtime.c -o $@

$(BUILD)/evaluate.o: evaluate.c program.h flow.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c evaluate.c -o $@

$(BUILD)/libpure-execution.a: $(BUILD)/runtime.o
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/libpure-execution-async.a: $(BUILD)/runtime-async.o
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/test-flow: test-flow.c store.c flow.c store.h flow.h result.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_STORE_TEST test-flow.c store.c flow.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-program: test-program.c program.c evaluate.c program.h store.h flow.h result.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) test-program.c program.c evaluate.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-fields: test-fields.c program.c evaluate.c program.h store.h flow.h result.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) test-fields.c program.c evaluate.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-runtime: test-runtime.c program.c $(CORE) $(HOST) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS test-runtime.c program.c $(CORE) $(HOST) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-async: test-async.c program.c $(CORE) $(HOST) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS -DPURE_ASYNC_EFFECTS test-async.c program.c $(HOST) $(CORE) $(LDFLAGS) -pthread $(LDLIBS) -o $@

$(BUILD)/test-splice: test-splice.c program.c $(CORE) $(HOST) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_RUNTIME_METRICS -DPURE_STORE_TEST test-splice.c program.c $(CORE) $(HOST) $(LDFLAGS) $(LDLIBS) -o $@

# Pure modules: loader/verifier/executor, its tests and a test host.
# Text states are runtime-owned drafts, so the executor links the tree engine.
MODULE = module.c module-list.c module-save.c module-text.c store-tree.c
MODULE_HEADERS = module.h module-list.h module-save.h module-text.h $(HEADERS)

$(BUILD)/test-module: test-module.c module-store.c $(MODULE) module-store.h $(MODULE_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_STORE_TEST test-module.c module-store.c $(MODULE) $(LDFLAGS) -lm $(LDLIBS) -o $@

$(BUILD)/pure-run: pure-run.c module-store.c $(MODULE) module-store.h $(MODULE_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) pure-run.c module-store.c $(MODULE) $(LDFLAGS) -lm $(LDLIBS) -o $@

# The document CLI contract driven by a module on the tree store.
$(BUILD)/pure-edit-module: edit-module.c module-store.c $(MODULE) module-store.h $(MODULE_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) edit-module.c module-store.c $(MODULE) $(LDFLAGS) -lm $(LDLIBS) -o $@

check-module: $(BUILD)/test-module $(BUILD)/pure-run $(BUILD)/pure-edit-module
	$(BUILD)/test-module

$(BUILD)/test-tree-store: test-tree-store.c store-tree.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_STORE_TEST test-tree-store.c store-tree.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-paged-store: test-paged-store.c store-paged.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_STORE_TEST test-paged-store.c store-paged.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/core-layout: core-layout.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) core-layout.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/core-layout-async: core-layout.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_ASYNC_EFFECTS core-layout.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/measure-peak-rss: measure-peak-rss.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) measure-peak-rss.c $(LDFLAGS) $(LDLIBS) -o $@

# Run on the release build, separately from sanitizer-instrumented objects.
check-core: core $(BUILD)/core-layout
	python3 check_core.py --archive $(BUILD)/libpure-execution.a --layout $(BUILD)/core-layout $(if $(CORE_EVIDENCE),--output "$(CORE_EVIDENCE)")

check-async: async-core $(BUILD)/core-layout-async $(BUILD)/test-async
	python3 check_core.py --archive $(BUILD)/libpure-execution-async.a --layout $(BUILD)/core-layout-async --budget async-core-budget.json $(if $(ASYNC_CORE_EVIDENCE),--output "$(ASYNC_CORE_EVIDENCE)")
	$(BUILD)/test-async

check-async-server: async-server fixed-editor $(BUILD)/pure-connections-async-gated
	python3 check_profile.py --build $(BUILD) --budget async-server-budget.json $(if $(ASYNC_SERVER_PROFILE_EVIDENCE),--output "$(ASYNC_SERVER_PROFILE_EVIDENCE)")
	python3 check_async_server.py --server $(BUILD)/pure-connections-async-gated --editor $(BUILD)/pure-edit-fixed $(if $(ASYNC_SERVER_EVIDENCE),--output "$(ASYNC_SERVER_EVIDENCE)")

# Release-linked default hosts are reviewed separately from the execution core.
# Do not run this size gate on sanitizer-instrumented binaries.
check-profile: editor server
	python3 check_profile.py --build $(BUILD) $(if $(PROFILE_EVIDENCE),--output "$(PROFILE_EVIDENCE)")

check-fixed-profile: fixed-editor fixed-server $(BUILD)/measure-peak-rss
	python3 check_profile.py --build $(BUILD) --budget fixed-budget.json $(if $(FIXED_PROFILE_EVIDENCE),--output "$(FIXED_PROFILE_EVIDENCE)")
	python3 check_micro_memory.py --build $(BUILD) --fixed $(if $(FIXED_MEMORY_EVIDENCE),--output "$(FIXED_MEMORY_EVIDENCE)")

check-fixed-server: fixed-editor fixed-server server
	python3 check_fixed_server.py --build $(BUILD) $(if $(FIXED_SERVER_EVIDENCE),--output "$(FIXED_SERVER_EVIDENCE)")

check-service-scale: fixed-editor fixed-server editor server
	python3 check_service_scale.py --build $(BUILD) $(if $(SERVICE_SCALE_EVIDENCE),--output "$(SERVICE_SCALE_EVIDENCE)")

check-async-service-scale: fixed-editor fixed-server editor server async-server
	python3 check_service_scale.py --build $(BUILD) --async-source $(if $(SERVICE_SCALE_EVIDENCE),--output "$(SERVICE_SCALE_EVIDENCE)")

check-tui-profile: fixed-tui
	python3 check_profile.py --build $(BUILD) --budget tui-budget.json $(if $(TUI_PROFILE_EVIDENCE),--output "$(TUI_PROFILE_EVIDENCE)")

check-source-tui-profile: source-tui
	python3 check_profile.py --build $(BUILD) --budget source-tui-budget.json $(if $(TUI_PROFILE_EVIDENCE),--output "$(TUI_PROFILE_EVIDENCE)")

check-tui: fixed-editor check-tui-profile
	python3 check_tui.py --build $(BUILD) $(if $(TUI_EVIDENCE),--output "$(TUI_EVIDENCE)")

check-source-tui: editor fixed-editor check-source-tui-profile
	python3 check_tui.py --build $(BUILD) --source $(if $(TUI_EVIDENCE),--output "$(TUI_EVIDENCE)")

check-release: check-core check-profile check-fixed-profile check check-policy check-micro-memory

check-micro-memory: editor $(BUILD)/measure-peak-rss
	python3 check_micro_memory.py --build $(BUILD) $(if $(MEMORY_EVIDENCE),--output "$(MEMORY_EVIDENCE)")

check-hybrid:
	$(MAKE) BUILD=build/hybrid STORE=store-paged.c check build/hybrid/test-paged-store
	build/hybrid/test-paged-store
	python3 check_profile.py --build build/hybrid --budget hybrid-budget.json
	$(MAKE) BUILD=build/hybrid STORE=store-paged.c check-micro-memory
	python3 check_small_scan.py --editor build/hybrid/pure-edit $(if $(SMALL_SCAN_EVIDENCE),--output "$(SMALL_SCAN_EVIDENCE)")
	$(MAKE) check-flat-export

# Selected page format can be exported to the unchanged default store format.
# This conversion tool and test are outside the runtime and default app builds.
check-flat-export: fixed-editor
	$(MAKE) BUILD=build/hybrid STORE=store-paged.c fixed-editor
	python3 check_flat_export.py --paged-editor build/hybrid/pure-edit-fixed --flat-editor $(BUILD)/pure-edit-fixed $(if $(EXPORT_EVIDENCE),--output "$(EXPORT_EVIDENCE)")

# Real source document -> two same-process domains using pinned mapped input.
check-mirror: mirror fixed-editor $(BUILD)/measure-peak-rss
	python3 check_mirror.py --mirror $(BUILD)/pure-mirror --editor $(BUILD)/pure-edit-fixed --rss-helper $(BUILD)/measure-peak-rss $(if $(MIRROR_SCRATCH_PARENT),--scratch-parent "$(MIRROR_SCRATCH_PARENT)") $(if $(MIRROR_EVIDENCE),--output "$(MIRROR_EVIDENCE)")

# Selected Linux-only storage copy experiment; does not change the default build.
check-copy-range: editor $(BUILD)/measure-peak-rss
	$(MAKE) BUILD=build/cfr CPPFLAGS="$(CPPFLAGS) -DPURE_STORE_CFR_COPY" check check-profile check-micro-memory check-service-scale
	python3 check_copy_path.py --baseline $(BUILD)/pure-edit --candidate build/cfr/pure-edit --baseline-helper $(BUILD)/measure-peak-rss --candidate-helper build/cfr/measure-peak-rss $(if $(COPY_RANGE_EVIDENCE),--output "$(COPY_RANGE_EVIDENCE)")

check-peer-profile: document-peer
	python3 check_profile.py --build $(BUILD) --budget peer-budget.json $(if $(PEER_PROFILE_EVIDENCE),--output "$(PEER_PROFILE_EVIDENCE)")

check-document-server-profile: document-server
	python3 check_profile.py --build $(BUILD) --budget peer-server-budget.json $(if $(PEER_SERVER_PROFILE_EVIDENCE),--output "$(PEER_SERVER_PROFILE_EVIDENCE)")

check-window-profile: window
	python3 check_profile.py --build $(BUILD) --budget window-budget.json $(if $(WINDOW_PROFILE_EVIDENCE),--output "$(WINDOW_PROFILE_EVIDENCE)")

check-document-peer: check-peer-profile editor
	python3 check_document_peer.py --peer $(BUILD)/pure-document-peer --editor $(BUILD)/pure-edit $(if $(PEER_EVIDENCE),--output "$(PEER_EVIDENCE)")

check-document-server: check-document-server-profile document-peer editor
	python3 check_document_peer.py --peer $(BUILD)/pure-document-peer --server $(BUILD)/pure-document-server --editor $(BUILD)/pure-edit $(if $(PEER_SERVER_EVIDENCE),--output "$(PEER_SERVER_EVIDENCE)")

# Opt-in TLS host checks: same authenticated work and failure scenarios without
# Linux /proc RSS or GNU binary-size tools. Requires OpenSSL headers and libs.
check-portable-peer: document-peer document-server editor
	python3 check_document_peer.py --peer $(BUILD)/pure-document-peer --editor $(BUILD)/pure-edit --portable $(if $(PORTABLE_PEER_EVIDENCE),--output "$(PORTABLE_PEER_EVIDENCE)")
	python3 check_document_peer.py --peer $(BUILD)/pure-document-peer --server $(BUILD)/pure-document-server --editor $(BUILD)/pure-edit --portable $(if $(PORTABLE_DOCUMENT_SERVER_EVIDENCE),--output "$(PORTABLE_DOCUMENT_SERVER_EVIDENCE)")

check: $(BUILD)/test-flow $(BUILD)/test-program $(BUILD)/test-fields $(BUILD)/test-runtime $(BUILD)/test-splice $(BUILD)/pure-edit $(BUILD)/pure-connections
	$(BUILD)/test-flow
	$(BUILD)/test-program
	$(BUILD)/test-fields
	python3 check.py --server $(BUILD)/pure-connections --runtime-test $(BUILD)/test-runtime --splice-test $(BUILD)/test-splice --editor $(BUILD)/pure-edit --store-source $(STORE) $(if $(EVIDENCE),--output "$(EVIDENCE)")

check-policy: editor server source-tui
	$(MAKE) BUILD=$(BUILD)/policy FIXED_PROGRAM=examples/bounded-editor.pure fixed-editor fixed-server fixed-tui
	python3 check_policy.py --source $(BUILD)/pure-edit --fixed $(BUILD)/policy/pure-edit-fixed --source-server $(BUILD)/pure-connections --fixed-server $(BUILD)/policy/pure-connections-fixed --source-tui $(BUILD)/pure-edit-tui --fixed-tui $(BUILD)/policy/pure-edit-tui-fixed --definition examples/bounded-editor.pure $(if $(POLICY_PEER),--peer $(POLICY_PEER)) $(if $(POLICY_EVIDENCE),--output "$(POLICY_EVIDENCE)")

check-policy-peer: document-peer
	$(MAKE) BUILD=$(BUILD) POLICY_PEER=$(BUILD)/pure-document-peer check-policy

# Host-neutral C smoke path; the release size/RSS gates above are calibrated
# only for Linux AArch64 and must not be reported as macOS measurements.
check-portable: $(BUILD)/test-flow $(BUILD)/test-program $(BUILD)/test-fields $(BUILD)/test-runtime $(BUILD)/test-splice $(BUILD)/test-async $(BUILD)/pure-edit $(BUILD)/pure-connections $(BUILD)/pure-edit-tui-fixed $(BUILD)/pure-edit-tui
	$(BUILD)/test-flow
	$(BUILD)/test-program
	$(BUILD)/test-fields
	$(BUILD)/test-runtime
	$(BUILD)/test-splice
	$(BUILD)/test-async

# Same real PTY/CLI scenarios as the Linux TUI gates, with no /proc RSS claim.
check-portable-tui: fixed-editor fixed-tui editor source-tui
	python3 check_tui.py --build $(BUILD) --portable $(if $(PORTABLE_FIXED_TUI_EVIDENCE),--output "$(PORTABLE_FIXED_TUI_EVIDENCE)")
	python3 check_tui.py --build $(BUILD) --source --portable $(if $(PORTABLE_SOURCE_TUI_EVIDENCE),--output "$(PORTABLE_SOURCE_TUI_EVIDENCE)")

# Selected single-file persistent-tree store: its own crash/oracle checks, then
# the unchanged runtime, splice, async and real-PTY editor checks on it.
check-tree:
	$(MAKE) BUILD=build/tree STORE=store-tree.c CPPFLAGS="$(CPPFLAGS) -DPURE_STORE_TREE" check-portable build/tree/test-tree-store fixed-editor fixed-tui editor source-tui
	build/tree/test-tree-store
	python3 check_tui.py --build build/tree --portable --single-file-store $(if $(TREE_TUI_EVIDENCE),--output "$(TREE_TUI_EVIDENCE)")
	python3 check_tui.py --build build/tree --source --portable --single-file-store

# Exercise the selected logical-page store without Linux-only size/RSS gates.
# The CI runner separately asserts its real OS page size before this target.
check-paged-portable:
	$(MAKE) BUILD=build/paged-portable STORE=store-paged.c check-portable check-portable-tui check build/paged-portable/test-paged-store
	build/paged-portable/test-paged-store

# Host-native behavior plus the same real document workload on both CLI variants.
# RSS is recorded on macOS and Linux; only check-micro-memory enforces the
# reviewed Linux AArch64 ceiling.
check-native-checkpoint: check-portable check-portable-tui fixed-editor $(BUILD)/measure-peak-rss
	python3 check_micro_memory.py --build $(BUILD) --portable $(if $(PORTABLE_SOURCE_MEMORY_EVIDENCE),--output "$(PORTABLE_SOURCE_MEMORY_EVIDENCE)")
	python3 check_micro_memory.py --build $(BUILD) --fixed --portable $(if $(PORTABLE_FIXED_MEMORY_EVIDENCE),--output "$(PORTABLE_FIXED_MEMORY_EVIDENCE)")
