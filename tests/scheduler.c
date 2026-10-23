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

static void stack_arguments(void)
{
    // l32i a8,a1,0; l32i a9,a1,4; add a2,a2,a7; add a2,a2,a8;
    // add a2,a2,a9; ret.n. Reads CALL0 arguments seven and eight from SP.
    const uint8_t code[] = {0x82,0x21,0x00,0x92,0x21,0x01,0x70,0x22,0x80,
                            0x80,0x22,0x80,0x90,0x22,0x80,0x0d,0xf0};
    uint32_t args[] = {11,22,33,44,55,66,77,88};
    uint32_t saved[16];
    for (int i = 0; i < 16; i++) C.a[i] = 0xabc00000u + (uint32_t)i;
    C.a[1] = DRAM_BASE + DRAM_SIZE - 64;
    C.pc = IRAM_BASE + 0x500;
    C.waiting = 0;
    C.ps = PS_UM;
    E.irq_req = 0;
    E.next_event = E.slice_end = E.ccomp_tick = UINT64_MAX;
    memcpy(saved, C.a, sizeof saved);
    uint8_t before[32]; memset(before, 0x5a, sizeof before);
    hle_write(C.a[1] - sizeof before, before, sizeof before);
    hle_write(IRAM_BASE + 0x300, code, sizeof code);
    CHECK(guest_call(IRAM_BASE + 0x300, 8, args) == 242);
    CHECK(!memcmp(saved, C.a, sizeof saved));
    CHECK(C.pc == IRAM_BASE + 0x500 && E.call_depth == 0);
    uint8_t after[32]; hle_read(C.a[1] - sizeof after, after, sizeof after);
    CHECK(!memcmp(before, after, sizeof before));
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
    stack_arguments();
    printf("scheduler: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
