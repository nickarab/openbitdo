CC ?= cc
PKG_CONFIG ?= pkg-config

# CFLAGS da linha de comando (make CFLAGS=-O3) troca só a otimização: o
# override mantém o padrão C23 e os warnings, que fazem parte do contrato.
CFLAGS ?= -O2
override CFLAGS += -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
override CPPFLAGS += -Inative -MMD -MP

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

$(BUILD)/8bdgui: $(BUILD)/8bdgui.o $(BUILD)/bd_core.o $(BUILD)/bd_hid.o $(BUILD)/bd_usb.o
	$(CC) $(LDFLAGS) -o $@ $^ $(GTK_LIBS) -lm

$(BUILD)/bd_cli: $(BUILD)/bd_cli.o $(BUILD)/bd_hid.o $(BUILD)/bd_core.o $(BUILD)/bd_usb.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/bd_build: $(BUILD)/bd_build.o $(BUILD)/bd_core.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/bd_watch: $(BUILD)/bd_watch.o
	$(CC) $(LDFLAGS) -o $@ $^

# Testes unitários: sem GTK e sem hardware (test_hid fala com um socketpair).
TEST_BINARIES := $(BUILD)/test_core $(BUILD)/test_hid

$(BUILD)/%.o: native/tests/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/test_core: $(BUILD)/test_core.o $(BUILD)/bd_core.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_hid: $(BUILD)/test_hid.o $(BUILD)/bd_hid.o $(BUILD)/bd_core.o
	$(CC) $(LDFLAGS) -o $@ $^

tests: $(TEST_BINARIES)

# Suíte completa: compila tudo com -Werror num diretório próprio e roda os blocos.
test:
	native/tests/run-tests.sh

# Só os testes unitários, com AddressSanitizer + UBSan.
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
test-asan:
	$(MAKE) BUILD=$(BUILD)/asan CFLAGS="-O1 -g $(SAN)" LDFLAGS="$(SAN)" tests
	$(BUILD)/asan/test_core
	$(BUILD)/asan/test_hid

VERSION ?= 0.1.0

appimage: all
	packaging/appimage.sh $(VERSION)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

.PHONY: appimage all tests test test-asan clean
