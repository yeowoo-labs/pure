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
#   serve/  pure-serve: the HTTP host, outbound delivery, JSON in place.
#   cli/    command-line hosts.
# libpure.a is the app model with the tree store: what a host embeds (Vega's
# surfaces link it). prototypes/ keeps the earlier document runtime (000-077).
INCLUDES = -Icore
MODEL = core/module.c core/module-list.c core/module-save.c core/module-text.c core/module-store.c core/flow.c
STORE = store/store-tree.c
SERVE = serve/serve.c serve/deliver.c serve/message-json.c
HEADERS = $(wildcard core/*.h) $(wildcard serve/*.h)
OBJECTS = $(patsubst %.c,$(BUILD)/obj/%.o,$(MODEL) $(STORE))

.PHONY: all check
all: $(BUILD)/libpure.a $(BUILD)/pure-serve $(BUILD)/pure-run $(BUILD)/pure-edit-module

$(BUILD)/obj/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libpure.a: $(OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

# The runtime's tickets and budgets, with effects completed off the owner thread.
# It links after the library, last as it always was, so code keeps its place.
$(BUILD)/pure-serve: $(SERVE) core/runtime.c $(BUILD)/libpure.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -Iserve -DPURE_ASYNC_EFFECTS $(SERVE) $(BUILD)/libpure.a core/runtime.c \
		$(LDFLAGS) -lssl -lcrypto -lm -pthread $(LDLIBS) -o $@

$(BUILD)/pure-run: cli/pure-run.c $(BUILD)/libpure.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) cli/pure-run.c $(BUILD)/libpure.a $(LDFLAGS) -lm $(LDLIBS) -o $@

# The document CLI contract, driven by a module on the tree store.
$(BUILD)/pure-edit-module: cli/edit-module.c $(BUILD)/libpure.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) cli/edit-module.c $(BUILD)/libpure.a $(LDFLAGS) -lm $(LDLIBS) -o $@

# Tests build their own copies: the store's fault hooks exist only under PURE_STORE_TEST.
$(BUILD)/test-module: core/test-module.c $(MODEL) $(STORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -DPURE_STORE_TEST core/test-module.c $(MODEL) $(STORE) $(LDFLAGS) -pthread -lm $(LDLIBS) -o $@

$(BUILD)/test-tree-store: store/test-tree-store.c $(STORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -DPURE_STORE_TEST store/test-tree-store.c $(STORE) $(LDFLAGS) $(LDLIBS) -o $@

check: all $(BUILD)/test-module $(BUILD)/test-tree-store
	$(BUILD)/test-tree-store
	$(BUILD)/test-module
