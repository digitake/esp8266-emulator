// SDK priority 32 must run before Arduino's continuously reposted priority 1.
#include "emu.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

static void task(uint32_t prio, uint32_t fn, uint32_t queue)
{
    C.a[0] = IRAM_BASE;
    C.a[2] = fn; C.a[3] = prio; C.a[4] = queue; C.a[5] = 1;
    rom_hle(0x40000dd0);
    CHECK(C.a[2] == 0);
}

static uint32_t post(uint32_t prio, uint32_t sig)
{
    C.a[0] = IRAM_BASE;
    C.a[2] = prio; C.a[3] = sig; C.a[4] = 0;
    rom_hle(0x40000e24);
    return C.a[2];
}

int main(void)
{
    memset(&E, 0, sizeof E);
    E.tpc = 2; E.cpu_mhz = 80;
    E.slice_end = E.ccomp_tick = UINT64_MAX;
    cpu_reset(); soc_power_on(); rom_init();
    task(1, IRAM_BASE + 0x100, DRAM_BASE + 0x100);
    task(32, IRAM_BASE + 0x200, DRAM_BASE + 0x200);
    CHECK(post(1, 11) == 0);
    CHECK(post(32, 12) == 0);
    CHECK(post(33, 99) == 1); // must not alias priority 1
    C.a[1] = DRAM_BASE + DRAM_SIZE - 32;
    rom_hle(0x40000e04); // ets_run
    uint32_t dispatcher = C.pc;
    rom_hle(dispatcher);
    CHECK(C.pc == IRAM_BASE + 0x200);
    CHECK(hle_rd32(C.a[2]) == 12);
    rom_hle(dispatcher);
    CHECK(C.pc == IRAM_BASE + 0x100);
    CHECK(hle_rd32(C.a[2]) == 11);
    printf("scheduler: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
