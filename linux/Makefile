# wlautoscroll - Wayland port of WinAutoScroll
#
#   make            build build/wlautoscroll
#   make test       unit tests (+ mock-compositor overlay test if python3 exists)
#   make install    DESTDIR=... PREFIX=/usr

VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)
PREFIX  ?= /usr
BUILD   ?= build
CC      ?= cc
CFLAGS  ?= -O2 -g

override CPPFLAGS += -D_GNU_SOURCE -DWLAS_VERSION='"$(VERSION)"' -DTOP_CONFIG='"$(CURDIR)/../config.ini"'
override CFLAGS   += -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter -MMD -MP
override LDLIBS   += -lm

LIB_SRC := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)

all: $(BUILD)/wlautoscroll

$(BUILD)/wlautoscroll: $(LIB_OBJ) $(BUILD)/main.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_unit: tests/test_unit.c $(LIB_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

.PHONY: test
test: $(BUILD)/test_unit $(BUILD)/wlautoscroll
	$(BUILD)/test_unit
	@if command -v python3 >/dev/null 2>&1; then \
		python3 tests/test_overlay.py $(BUILD)/wlautoscroll; \
	else echo "python3 not found: skipping overlay test"; fi

DESTDIR ?=

.PHONY: install
install: $(BUILD)/wlautoscroll
	install -Dm755 $(BUILD)/wlautoscroll $(DESTDIR)$(PREFIX)/bin/wlautoscroll
	install -Dm644 dist/60-wlautoscroll.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/60-wlautoscroll.rules
	install -Dm644 dist/wlautoscroll.conf $(DESTDIR)$(PREFIX)/lib/modules-load.d/wlautoscroll.conf
	install -Dm644 dist/wlautoscroll.service $(DESTDIR)$(PREFIX)/lib/systemd/user/wlautoscroll.service
	install -Dm644 ../config.ini $(DESTDIR)$(PREFIX)/share/doc/wlautoscroll/config.ini

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
