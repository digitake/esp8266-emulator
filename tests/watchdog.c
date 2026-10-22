// Hardware watchdog register/interrupt scenarios and a real guest busy loop.
#include "emu.h"
#include <stdio.h>
#include <string.h>

static int failures;
static uint8_t flash[4096];
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)
#define WDT 0x60000900u
#define TICKS0 (2ull << 16)

static void reset(void)
{
    memset(&E, 0, sizeof E);
    E.flash = flash; E.flash_size = sizeof flash;
    E.tpc = 2; E.cpu_mhz = 80;
    E.slice_end = E.ccomp_tick = UINT64_MAX;
    cpu_reset(); soc_power_on(); soc_schedule();
}

static void enable(unsigned control)
{
    mem_st32(WDT + 4, 0);
    mem_st32(WDT + 8, 0);
    mem_st32(0x3ff00004, 1);
    mem_st32(WDT, control);
}

static void two_stage_and_feed(void)
{
    reset(); enable(0x39);
    cpu_set_freq(160); // watchdog remains on its fixed 80 MHz peripheral clock
    CHECK(mem_ld32(WDT + 12) == 65536);
    E.tick = TICKS0 - 2; soc_events();
    CHECK(mem_ld32(WDT + 12) == 1);
    CHECK(!(C.intpend & (1u << INUM_WDT)));
    E.tick = TICKS0; soc_events();
    CHECK(mem_ld32(WDT + 16) == 1);
    CHECK(C.intpend & (1u << INUM_WDT));
    E.tick += 20;
    mem_st32(WDT + 20, 0x72); // wrong magic does not feed
    CHECK(mem_ld32(WDT + 16) == 1);
    CHECK(mem_ld32(WDT + 12) == 65526);
    mem_st32(WDT + 20, 0x73);
    CHECK(mem_ld32(WDT + 16) == 0);
    CHECK(mem_ld32(WDT + 12) == 65536);
    E.tick += TICKS0; soc_events();
    CHECK(mem_ld32(WDT + 16) == 1);
    if (!setjmp(E.reset_jmp)) {
        E.tick += TICKS0; soc_events();
        CHECK(0); // a missing hardware reset would return here
    }
}

static void stage_reset_and_disable(void)
{
    reset(); enable(0x39);
    E.tick = TICKS0 + 10; soc_events();
    uint32_t count = mem_ld32(WDT + 12);
    mem_st32(WDT + 24, 0x73);
    CHECK(mem_ld32(WDT + 16) == 0);
    CHECK(mem_ld32(WDT + 12) == count);
    mem_st32(WDT, 0x38);
    E.tick += 100 * TICKS0; soc_events();
    CHECK(mem_ld32(WDT + 12) == count);
    mem_st32(WDT, 0x39);
    CHECK(mem_ld32(WDT + 12) == 65536);
    CHECK(mem_ld32(WDT + 16) == 0);

    mem_st32(0x3ff00018, 8); // clockgate_watchdog(0)
    E.tick += TICKS0 * 4; soc_events();
    CHECK(mem_ld32(WDT + 12) == 65536);
    mem_st32(0x3ff00018, 0);
    E.tick += TICKS0; soc_events();
    CHECK(mem_ld32(WDT + 16) == 1);
}

static void modes_and_irq_gate(void)
{
    reset(); enable(0x3d); // bit 2 set: reset at first deadline
    if (!setjmp(E.reset_jmp)) {
        E.tick = TICKS0; soc_events();
        CHECK(0);
    }
    CHECK(!(C.intpend & (1u << INUM_WDT)));

    reset(); enable(0x3b); // bit 1 set: stage 1 never resets
    mem_st32(0x3ff00004, 0); // edge interrupt gate disabled
    E.tick = TICKS0; soc_events();
    CHECK(mem_ld32(WDT + 16) == 1);
    CHECK(!(C.intpend & (1u << INUM_WDT)));
    E.tick += 10 * TICKS0; soc_events();
    CHECK(mem_ld32(WDT + 16) == 1);
    CHECK(mem_ld32(WDT + 12) == 65536);
}

static void eboot_disable_restore(void)
{
    reset(); enable(0x39); rom_init();
    C.a[0] = IRAM_BASE;
    rom_hle(0x400030f0); // ets_wdt_disable
    CHECK(!(mem_ld32(WDT) & 1));
    E.tick += 100 * TICKS0; soc_events();
    rom_hle(0x40002fa0); // paired ets_wdt_enable used by eboot
    CHECK(mem_ld32(WDT) == 0x39);
    CHECK(mem_ld32(WDT + 12) == 65536);
}

static void real_guest_hang(void)
{
    reset(); enable(0x39);
    // J to itself; interrupts masked. The hardware reset must unwind cpu_run.
    mem_iram[0] = 0x06; mem_iram[1] = 0xff; mem_iram[2] = 0xff;
    C.pc = IRAM_BASE;
    int code = setjmp(E.reset_jmp);
    if (!code) {
        cpu_run(0);
        CHECK(0);
    } else {
        CHECK(code == 4); // main.c RJ_WDT
        CHECK(E.tick == 2 * TICKS0);
        CHECK(C.intpend & (1u << INUM_WDT));
    }
}

static void mac_timer_irq_and_rearm(void)
{
    reset();
    E.tick_epoch = 19; E.tick = 19 + 100 * 160;
    CHECK(mem_ld32(0x3ff21048) == 100);
    mem_st32(0x3ff20c18, 1u << 26);
    mem_st32(0x3ff21058, 1u << 31);
    mem_st32(0x3ff210cc, 110);
    mem_st32(0x3ff210d0, 0);
    mem_st32(0x3ff210c8, 1u << 31);
    CHECK(E.next_event == 19 + 110 * 160);
    E.tick = 19 + 110 * 160 - 1; soc_events();
    CHECK(!(E.irq_lines & 1));
    E.tick++; soc_events();
    CHECK(E.irq_lines & 1);
    CHECK(mem_ld32(0x3ff20c20) == 1u << 26);
    mem_st32(0x3ff20c24, 1u << 26);
    CHECK(!(E.irq_lines & 1));
    E.tick += 1000 * 160; soc_events();
    CHECK(!(E.irq_lines & 1)); // one shot; SDK must rearm
}

int main(void)
{
    two_stage_and_feed(); stage_reset_and_disable(); modes_and_irq_gate();
    eboot_disable_restore(); real_guest_hang(); mac_timer_irq_and_rearm();
    printf("watchdog: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
