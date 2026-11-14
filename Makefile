CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2
CFLAGS += -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
CPPFLAGS += -Inative -MMD -MP

GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk4)

BUILD := build
BINARIES := $(BUILD)/8bdgui $(BUILD)/bd_cli $(BUILD)/bd_build $(BUILD)/bd_watch

all: $(BINARIES)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: native/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: linux/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/8bdgui.o: native/8bdgui.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) -c -o $@ $<

$(BUILD)/8bdgui: $(BUILD)/8bdgui.o $(BUILD)/bd_core.o $(BUILD)/bd_hid.o
	$(CC) $(LDFLAGS) -o $@ $^ $(GTK_LIBS) -lm

$(BUILD)/bd_cli: $(BUILD)/bd_cli.o $(BUILD)/bd_hid.o $(BUILD)/bd_core.o $(BUILD)/bd_usb.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/bd_build: $(BUILD)/bd_build.o $(BUILD)/bd_core.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/bd_watch: $(BUILD)/bd_watch.o
	$(CC) $(LDFLAGS) -o $@ $^

test: all
	native/tests/run-tests.sh

VERSION ?= 0.1.0

appimage: all
	packaging/appimage.sh $(VERSION)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

.PHONY: appimage  all test clean
