CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build

# Pure is a small core with the rest selected around it:
#   core/   execution, ownership, limits and completion (runtime), and the app
#           model: modules verified before they run, events as transactions,
#           states as drafts; state.h and store.h are what it asks of storage.
#   store/  the tree store (one file; commits are one contiguous write and one
#           sync; formats PURETRE3 and PUREREC1).
#   link/   links without connections (UDP, sealed, no handshake) and the
#           crypto they and sealed sends use; with pure-crypto.so beside it,
#           OpenSSL's part (TLS to https receivers, sealing), mapped only by a
#           process that uses one.
#   serve/  pure-serve: the HTTP host, outbound delivery, JSON in place.
#   cli/    command-line hosts.
# libpure.a is the app model with the tree store: what a host embeds (Vega's
# surfaces link it); libpure-link.a is the links with the crypto loader.
# prototypes/ keeps the earlier document runtime (000-077).
# Unit tests sit by their part (core/test-module.c, store/test-tree-store.c,
# store/test-log.c, link/test-link.c, serve/test-message-json.c);
# tests/check.py checks the hosts end to end.
INCLUDES = -Icore
MODEL = core/module.c core/module-batch.c core/module-jit.c core/module-list.c core/module-save.c core/module-text.c core/module-store.c core/flow.c
STORE = store/store-tree.c store/store-log.c
LINK = link/link-protocol.c link/link.c link/sending.c link/crypto.c
SERVE = serve/serve.c serve/deliver.c serve/message-json.c serve/sha256.c
HEADERS = $(wildcard core/*.h) $(wildcard link/*.h) $(wildcard serve/*.h) $(wildcard store/*.h)
OBJECTS = $(patsubst %.c,$(BUILD)/obj/%.o,$(MODEL) $(STORE))
LINK_OBJECTS = $(patsubst %.c,$(BUILD)/obj/%.o,$(LINK))

.PHONY: all check
all: $(BUILD)/libpure.a $(BUILD)/libpure-link.a $(BUILD)/pure-serve $(BUILD)/pure-crypto.so $(BUILD)/pure-run

$(BUILD)/obj/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@

# The lists' searches count a node's keys and positions whole, and move
# items in a leaf column by column: -O3 makes vector instructions of them
# (docs/design/020: 8% of a breadth first search).
LIST_OPTIMIZE ?= -O3
$(BUILD)/obj/core/module-list.o: CFLAGS += $(LIST_OPTIMIZE)
# A batch's instructions are loops over its lanes: vector instructions too
# (docs/design/024), each loop at the start of a 64-byte line: else a change
# anywhere in the file moves them, and its hot loops ran 6% apart with the
# same instructions (knapsack 8.26, 8.74, 8.20 ms; docs/research/138). (The
# lists' loops so aligned cost the bank's queries 1-2% more instructions.)
BATCH_OPTIMIZE ?= $(LIST_OPTIMIZE) -falign-loops=64
$(BUILD)/obj/core/module-batch.o: CFLAGS += $(BATCH_OPTIMIZE)

# The links know nothing of the core: they build without its headers.
$(BUILD)/obj/link/%.o: link/%.c $(wildcard link/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libpure.a: $(OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/libpure-link.a: $(LINK_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

# The runtime's tickets and budgets, with effects completed off the owner thread.
# It links after the library, last as it always was, so code keeps its place.
# The app model's functions are exported for a surface it maps (serve/surface.h).
$(BUILD)/pure-serve: $(SERVE) core/runtime.c $(BUILD)/libpure.a $(BUILD)/libpure-link.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -Ilink -Iserve -DPURE_ASYNC_EFFECTS $(SERVE) $(BUILD)/libpure-link.a \
		$(BUILD)/libpure.a core/runtime.c \
		$(LDFLAGS) -Wl,--export-dynamic-symbol='pure_*' -lm -pthread $(LDLIBS) -o $@

$(BUILD)/pure-crypto.so: link/crypto-openssl.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared link/crypto-openssl.c $(LDFLAGS) -lssl -lcrypto \
		$(LDLIBS) -o $@

$(BUILD)/pure-run: cli/pure-run.c $(BUILD)/libpure.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) cli/pure-run.c $(BUILD)/libpure.a $(LDFLAGS) -lm $(LDLIBS) -o $@

# Tests build their own copies: the store's fault hooks exist only under PURE_STORE_TEST.
$(BUILD)/test-module: core/test-module.c $(MODEL) $(STORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -DPURE_STORE_TEST core/test-module.c $(MODEL) $(STORE) $(LDFLAGS) -pthread -lm $(LDLIBS) -o $@

# The same, under AddressSanitizer and UndefinedBehaviorSanitizer with leaks
# counted: snapshots are read and let go on other threads (eebdeea left a
# race on their register and leaks that the plain build passes).
$(BUILD)/test-module-sanitized: core/test-module.c $(MODEL) $(STORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all $(INCLUDES) -DPURE_STORE_TEST core/test-module.c \
		$(MODEL) $(STORE) $(LDFLAGS) -pthread -lm $(LDLIBS) -o $@

# A link's two sides through a network of the test's own (pure-crypto.so beside it).
$(BUILD)/test-link: link/test-link.c $(BUILD)/libpure-link.a $(BUILD)/pure-crypto.so $(wildcard link/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) link/test-link.c $(BUILD)/libpure-link.a $(LDFLAGS) -lm -pthread -ldl $(LDLIBS) -o $@

$(BUILD)/test-message-json: serve/test-message-json.c serve/message-json.c $(BUILD)/libpure.a $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -Iserve serve/test-message-json.c serve/message-json.c $(BUILD)/libpure.a \
		$(LDFLAGS) -lm $(LDLIBS) -o $@

# LD_PRELOAD: the wall clock set back an hour, to see timers go on (tests/check.py).
$(BUILD)/clock-back.so: tests/clock-back.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared tests/clock-back.c $(LDFLAGS) -ldl $(LDLIBS) -o $@

# A module run ranged and not from the same inputs, compared (tests/check.py).
$(BUILD)/module-probe: tests/module-probe.c $(BUILD)/libpure.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) tests/module-probe.c $(BUILD)/libpure.a $(LDFLAGS) -lm $(LDLIBS) -o $@

$(BUILD)/test-log: store/test-log.c store/store-log.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -DPURE_STORE_TEST store/test-log.c store/store-log.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-tree-store: store/test-tree-store.c $(STORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -DPURE_STORE_TEST store/test-tree-store.c $(STORE) $(LDFLAGS) $(LDLIBS) -o $@

# The same with ASan and UBSan: a store's mappings are counted and let go on
# any thread (threads, grouped), and its writer outlives each commit.
$(BUILD)/test-tree-store-sanitized: store/test-tree-store.c $(STORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all $(INCLUDES) -DPURE_STORE_TEST store/test-tree-store.c \
		$(STORE) $(LDFLAGS) -pthread $(LDLIBS) -o $@

# The HTTP host's checks build their apps with aup (the sibling checkout) and
# compare documents with the earlier document server.
AUP ?= ../aup
check: all $(BUILD)/test-module $(BUILD)/test-module-sanitized $(BUILD)/test-tree-store $(BUILD)/test-tree-store-sanitized \
		$(BUILD)/test-log $(BUILD)/test-link $(BUILD)/test-message-json $(BUILD)/module-probe $(BUILD)/clock-back.so
	$(BUILD)/test-tree-store
	ASAN_OPTIONS=detect_leaks=1 $(BUILD)/test-tree-store-sanitized > /dev/null
	$(BUILD)/test-log
	$(BUILD)/test-module
	ASAN_OPTIONS=detect_leaks=1 $(BUILD)/test-module-sanitized > /dev/null
	$(BUILD)/test-link
	$(BUILD)/test-message-json
	$(MAKE) -C $(AUP) build/aup
	$(MAKE) -C prototypes/connections BUILD=build/tree STORE=store-tree.c build/tree/pure-connections
	python3 tests/check.py --aup $(AUP)/build/aup --aup-root $(AUP)
