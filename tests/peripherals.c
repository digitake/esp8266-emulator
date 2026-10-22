// Register-level scenarios; no firmware image or private peripheral state needed.
#include "emu.h"
#include <stdio.h>
#include <string.h>

static int failures;
static uint8_t flash[4096];
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static void reset(void)
{
    memset(&E, 0, sizeof E);
    E.flash = flash; E.flash_size = sizeof flash;
    memset(flash, 0xff, sizeof flash);
    E.tpc = 2; E.cpu_mhz = 80;
    E.slice_end = E.ccomp_tick = UINT64_MAX;
    cpu_reset();
    soc_power_on();
    soc_schedule();
}

static void uart_rx(void)
{
    reset();
    uint8_t data[] = {0x41, 0x42, 0x43};
    mem_st32(0x60000014, 80); // 1 Mbaud; timeout threshold 2 -> 16 us
    mem_st32(0x60000024, 0x82000002); // FULL when count > 2
    mem_st32(0x6000000c, 0x101);
    soc_uart_rx(0, data, 2);
    CHECK(!(mem_ld32(0x60000004) & 0x101));
    CHECK(!(E.irq_lines & (1u << INUM_UART)));
    E.tick = 16 * 160 - 1; soc_events();
    CHECK(!(mem_ld32(0x60000004) & 0x100));
    E.tick++; soc_events();
    CHECK(mem_ld32(0x60000008) & 0x100);
    CHECK(E.irq_lines & (1u << INUM_UART));
    CHECK(mem_ld32(0x60000000) == 0x41);
    CHECK(mem_ld32(0x60000000) == 0x42);
    CHECK(mem_ld32(0x60000004) & 0x100); // latched until W1C
    mem_st32(0x60000010, 0x101);
    CHECK(!(E.irq_lines & (1u << INUM_UART)));
    soc_uart_rx(0, data, 3);
    CHECK(mem_ld32(0x60000008) & 1);
    mem_ld32(0x60000000); // 2 left; clear now succeeds
    mem_st32(0x60000010, 1);
    CHECK(!(mem_ld32(0x60000004) & 1));
    mem_st32(0x60000020, 1u << 17); // FIFO reset cancels timeout
    E.tick += 100 * 160; soc_events();
    CHECK(!(mem_ld32(0x60000004) & 0x101));

    reset();
    mem_st32(0x60000024, 0); // FULL threshold zero
    soc_uart_rx(0, data, 1);
    CHECK(mem_ld32(0x60000004) & 1);
}

static void timers(void)
{
    reset();
    mem_st32(0x60000608, 0x80);
    mem_st32(0x60000600, 3);
    E.tick = 6; soc_events();
    CHECK(mem_ld32(0x60000604) == 0);
    CHECK(C.intpend & (1u << INUM_FRC1));
    C.intpend = 0; irq_update();
    E.tick += 1000000; soc_events();
    CHECK(mem_ld32(0x60000604) == 0);
    CHECK(!(C.intpend & (1u << INUM_FRC1))); // no wrap/retrigger
    mem_st32(0x60000600, 2);
    E.tick += 4; soc_events();
    CHECK(C.intpend & (1u << INUM_FRC1)); // explicit reload restarts

    reset();
    mem_st32(0x60000620, 5);
    mem_st32(0x60000630, 5);
    mem_st32(0x60000628, 0x80);
    CHECK(E.next_event == E.tick);
    soc_events();
    CHECK(C.intpend & (1u << INUM_FRC2));
    C.intpend = 0; irq_update();
    E.tick += 2; soc_events();
    CHECK(!(C.intpend & (1u << INUM_FRC2))); // equal alarm only once
}

static void gpio(void)
{
    reset();
    mem_st32(0x600007a0, 1u << 3); // Arduino INPUT_PULLDOWN_16
    CHECK(mem_ld32(0x6000078c) == 0);
    soc_set_input(16, 1); CHECK(mem_ld32(0x6000078c) == 1);
    soc_set_input(16, -1); CHECK(mem_ld32(0x6000078c) == 0);
    mem_st32(0x60000774, 1); mem_st32(0x60000768, 1);
    CHECK(mem_ld32(0x6000078c) == 1);
    mem_st32(0x60000768, 0); CHECK(mem_ld32(0x6000078c) == 0);

    mem_st32(0x60000338, 4u << 7); // GPIO4 low-level interrupt
    soc_set_input(4, 0);
    CHECK(mem_ld32(0x6000031c) & 16);
    mem_st32(0x60000324, 16);
    CHECK(mem_ld32(0x6000031c) & 16); // level reasserts while held
    soc_set_input(4, 1); mem_st32(0x60000324, 16);
    CHECK(!(mem_ld32(0x6000031c) & 16));
    mem_st32(0x60000338, 2u << 7); // falling edge
    soc_set_input(4, 0); CHECK(mem_ld32(0x6000031c) & 16);
    mem_st32(0x60000324, 16);
    CHECK(!(mem_ld32(0x6000031c) & 16)); // no repeated edge while held
}

static void flash_bounds(void)
{
    reset();
    uint8_t bytes[] = {0x00, 0x12}, out[] = {0x5a, 0xa5};
    CHECK(spi_flash_write(UINT32_MAX, bytes, 2) != 0);
    CHECK(flash[0] == 0xff);
    CHECK(guest_ptr(DRAM_BASE + 1, UINT32_MAX) == NULL);
    CHECK(guest_ptr(IRAM_BASE + 1, UINT32_MAX) == NULL);
    CHECK(guest_ptr(RTCMEM_BASE + 1, UINT32_MAX) == NULL);
    CHECK(spi_flash_read(UINT32_MAX, out, 2) != 0);
    CHECK(out[0] == 0x5a && out[1] == 0xa5);
    CHECK(spi_flash_erase(1, UINT32_MAX) != 0);
    CHECK(spi_flash_write(sizeof flash - 1, bytes, 2) != 0);
    CHECK(spi_flash_write(0, bytes, 2) == 0);
    CHECK(flash[0] == 0 && flash[1] == 0x12);
    CHECK(spi_flash_erase(0, sizeof flash) == 0);
    CHECK(flash[0] == 0xff);
}

int main(void)
{
    uart_rx(); timers(); gpio(); flash_bounds();
    printf("peripherals: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
