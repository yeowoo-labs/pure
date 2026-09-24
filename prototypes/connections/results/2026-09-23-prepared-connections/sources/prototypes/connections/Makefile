CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build

.PHONY: all check
all: $(BUILD)/pure-connections

$(BUILD):
	mkdir -p $@

$(BUILD)/pure-connections: server.c store.c flow.c program.c store.h flow.h program.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c store.c flow.c program.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-flow: test-flow.c store.c flow.c store.h flow.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_STORE_TEST test-flow.c store.c flow.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-program: test-program.c program.c program.h store.h flow.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) test-program.c program.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(BUILD)/test-flow $(BUILD)/test-program $(BUILD)/pure-connections
	$(BUILD)/test-flow
	$(BUILD)/test-program
	python3 check.py --server $(BUILD)/pure-connections $(if $(EVIDENCE),--output "$(EVIDENCE)")
