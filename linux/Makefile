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

all: $(LIB_OBJ)

$(BUILD)/test_unit: tests/test_unit.c $(LIB_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

.PHONY: test
test: $(BUILD)/test_unit
	$(BUILD)/test_unit


.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
