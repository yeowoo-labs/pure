CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build

.PHONY: all check
CORE = runtime.c evaluate.c
HOST = store.c store-port.c
HEADERS = runtime.h program.h store.h store-port.h flow.h result.h
all: $(BUILD)/pure-connections $(BUILD)/libpure-execution.a

$(BUILD):
	mkdir -p $@

$(BUILD)/pure-connections: server.c $(HOST) flow.c program.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c $(HOST) flow.c program.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/runtime.o: runtime.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c runtime.c -o $@

$(BUILD)/evaluate.o: evaluate.c program.h flow.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c evaluate.c -o $@

$(BUILD)/libpure-execution.a: $(BUILD)/runtime.o $(BUILD)/evaluate.o
	$(AR) rcs $@ $^

$(BUILD)/test-flow: test-flow.c store.c flow.c store.h flow.h result.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPURE_STORE_TEST test-flow.c store.c flow.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-program: test-program.c program.c evaluate.c program.h store.h flow.h result.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) test-program.c program.c evaluate.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-fields: test-fields.c program.c evaluate.c program.h store.h flow.h result.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) test-fields.c program.c evaluate.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-runtime: test-runtime.c program.c $(CORE) $(HOST) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) test-runtime.c program.c $(CORE) $(HOST) $(LDFLAGS) $(LDLIBS) -o $@

check: $(BUILD)/test-flow $(BUILD)/test-program $(BUILD)/test-fields $(BUILD)/test-runtime $(BUILD)/pure-connections
	$(BUILD)/test-flow
	$(BUILD)/test-program
	$(BUILD)/test-fields
	python3 check.py --server $(BUILD)/pure-connections --runtime-test $(BUILD)/test-runtime $(if $(EVIDENCE),--output "$(EVIDENCE)")
