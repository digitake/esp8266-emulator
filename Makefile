# esp8266emu: GNU C11 with GCC or clang, no dependencies beyond libc/libm
# (WSL Ubuntu: gcc + make; macOS: the Xcode command line tools).
CC ?= gcc
# GCC picks its own LTO job count; clang, the cc of macOS, takes plain -flto.
LTO := $(if $(findstring clang,$(shell $(CC) --version 2>/dev/null)),-flto,-flto=auto)
CFLAGS ?= -O3 $(LTO) -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
NET_SRC = src/net.c src/net_profile.c src/host_net.c src/host_dns.c
SRC = src/cpu.c src/soc.c src/rom.c src/crypto.c src/main.c src/chip_profile.c $(NET_SRC)
HEADERS = $(wildcard src/*.h)
OUT = build/esp8266emu
TEST_CFLAGS ?= -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined -fno-sanitize-recover=all
SLIRP_CFLAGS ?= $(shell pkg-config --cflags slirp 2>/dev/null)
SLIRP_LIBS ?= $(shell pkg-config --libs slirp 2>/dev/null)

$(OUT): $(SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

build/esp8266emu-net: $(SRC) $(HEADERS)
	@mkdir -p build
	@test -n "$(SLIRP_LIBS)" || { printf '%s\n' 'Install libslirp-dev, libglib2.0-dev and pkg-config, or set SLIRP_CFLAGS and SLIRP_LIBS.' >&2; exit 2; }
	$(CC) $(CFLAGS) -DHAVE_SLIRP $(SLIRP_CFLAGS) -o $@ $(SRC) -lm $(SLIRP_LIBS)

network: build/esp8266emu-net

build/test-host-net: tests/host_net.c src/host_net.c src/host_dns.c $(HEADERS)
	@mkdir -p build
	@test -n "$(SLIRP_LIBS)" || { printf '%s\n' 'Install libslirp-dev, libglib2.0-dev and pkg-config, or set SLIRP_CFLAGS and SLIRP_LIBS.' >&2; exit 2; }
	$(CC) $(TEST_CFLAGS) -DHAVE_SLIRP $(SLIRP_CFLAGS) -Isrc -o $@ tests/host_net.c src/host_net.c src/host_dns.c $(SLIRP_LIBS)

check-network-backend: build/test-host-net
	ASAN_OPTIONS=detect_leaks=0 build/test-host-net

clean:
	rm -rf build

build/test-host.o: src/main.c $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) -Isrc -Dmain=emu_original_main -c $< -o $@

build/test-peripherals: tests/peripherals.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/peripherals.c src/cpu.c src/soc.c src/rom.c src/crypto.c src/chip_profile.c $(NET_SRC) build/test-host.o -lm

build/test-core: tests/core.c $(SRC) src/emu.h src/crypto.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/core.c src/soc.c src/crypto.c src/chip_profile.c $(NET_SRC) build/test-host.o -lm

build/test-scheduler: tests/scheduler.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/scheduler.c src/cpu.c src/soc.c src/rom.c src/crypto.c src/chip_profile.c $(NET_SRC) build/test-host.o -lm

build/test-mac: tests/mac.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/mac.c src/cpu.c src/soc.c src/rom.c src/crypto.c src/chip_profile.c $(NET_SRC) build/test-host.o -lm

build/test-predecode: tests/predecode.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/predecode.c src/soc.c src/rom.c src/crypto.c src/chip_profile.c $(NET_SRC) build/test-host.o -lm

build/test-watchdog: tests/watchdog.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/watchdog.c src/cpu.c src/soc.c src/rom.c src/crypto.c src/chip_profile.c $(NET_SRC) build/test-host.o -lm

build/test-chip-profile: tests/chip_profile.c src/chip_profile.c src/chip_profile.h
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/chip_profile.c src/chip_profile.c

build/test-net: tests/net.c src/net.c src/net_profile.c $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/net.c src/net.c src/net_profile.c

build/test-host-dns: tests/host_dns.c src/host_dns.c src/host_dns.h
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/host_dns.c src/host_dns.c

test: $(OUT) build/test-peripherals build/test-core build/test-scheduler build/test-mac build/test-predecode build/test-watchdog build/test-chip-profile build/test-net build/test-host-dns
	ASAN_OPTIONS=detect_leaks=0 build/test-peripherals
	ASAN_OPTIONS=detect_leaks=0 build/test-core
	ASAN_OPTIONS=detect_leaks=0 build/test-scheduler
	ASAN_OPTIONS=detect_leaks=0 build/test-mac
	ASAN_OPTIONS=detect_leaks=0 build/test-predecode
	ASAN_OPTIONS=detect_leaks=0 build/test-watchdog
	ASAN_OPTIONS=detect_leaks=0 build/test-chip-profile
	ASAN_OPTIONS=detect_leaks=0 build/test-net
	ASAN_OPTIONS=detect_leaks=0 build/test-host-dns
	python3 -I scripts/mask.py
	python3 -I tests/test_gui.py
	python3 -I tests/test_tools.py
	python3 -I tests/test_chip_cli.py
	python3 -I tests/test_network_profile.py
	python3 -I tests/test_network_services.py
	python3 -I tests/test_reference_build.py
	python3 -I tests/test_runner.py

check: $(OUT) test
	python3 -I scripts/watchdog_check.py --out build/watchdog-check.json

.PHONY: clean test check network check-network-backend
