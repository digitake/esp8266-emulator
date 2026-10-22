# esp8266emu: GNU C11 / GCC, no dependencies beyond libc/libm (WSL Ubuntu: gcc + make).
CC ?= gcc
CFLAGS ?= -O3 -flto=auto -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
SRC = src/cpu.c src/soc.c src/rom.c src/crypto.c src/main.c
OUT = build/esp8266emu
TEST_CFLAGS ?= -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined -fno-sanitize-recover=all

$(OUT): $(SRC) src/emu.h src/crypto.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -rf build

build/test-host.o: src/main.c src/emu.h
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) -Isrc -Dmain=emu_original_main -c $< -o $@

build/test-peripherals: tests/peripherals.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/peripherals.c src/cpu.c src/soc.c src/rom.c src/crypto.c build/test-host.o -lm

build/test-core: tests/core.c $(SRC) src/emu.h src/crypto.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/core.c src/soc.c src/crypto.c build/test-host.o -lm

build/test-scheduler: tests/scheduler.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/scheduler.c src/cpu.c src/soc.c src/rom.c src/crypto.c build/test-host.o -lm

build/test-mac: tests/mac.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/mac.c src/cpu.c src/soc.c src/rom.c src/crypto.c build/test-host.o -lm

build/test-predecode: tests/predecode.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/predecode.c src/soc.c src/rom.c src/crypto.c build/test-host.o -lm

build/test-watchdog: tests/watchdog.c $(SRC) src/emu.h build/test-host.o
	$(CC) $(TEST_CFLAGS) -Isrc -o $@ tests/watchdog.c src/cpu.c src/soc.c src/rom.c src/crypto.c build/test-host.o -lm

test: build/test-peripherals build/test-core build/test-scheduler build/test-mac build/test-predecode build/test-watchdog
	ASAN_OPTIONS=detect_leaks=0 build/test-peripherals
	ASAN_OPTIONS=detect_leaks=0 build/test-core
	ASAN_OPTIONS=detect_leaks=0 build/test-scheduler
	ASAN_OPTIONS=detect_leaks=0 build/test-mac
	ASAN_OPTIONS=detect_leaks=0 build/test-predecode
	ASAN_OPTIONS=detect_leaks=0 build/test-watchdog
	python3 -I scripts/mask.py
	python3 -I tests/test_gui.py
	python3 -I tests/test_tools.py

check: $(OUT) test
	python3 -I scripts/watchdog_check.py --out build/watchdog-check.json

.PHONY: clean test check
