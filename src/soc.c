// ESP8266 memory map and peripheral register models.
#include "emu.h"
#include <string.h>
#include <stdio.h>

#define SLICE_TICKS (160u * 1000u)   // return to the host loop every 1 ms of virtual time

static uint32_t peri[PERI_SIZE / 4];      // 0x60000000..0x60000FFF backing store
static uint32_t dport[DPORT_SIZE / 4];    // 0x3FF00000..0x3FF3FFFF backing store
static uint32_t rng_state = 0x9E3779B9u;

static inline uint32_t rd32le(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

uint64_t now_us(void) { return (E.tick - E.tick_epoch) / 160; }

// ---------------------------------------------------------------- fault helpers
static void __attribute__((noreturn)) fault(uint32_t cause, uint32_t addr)
{
    cpu_exception(cause, addr, C.pc);
    if (E.fault_jmp) longjmp(*E.fault_jmp, 1);
    emu_halt("guest fault outside the run loop: cause %u addr %08x", cause, addr);
}

// ---------------------------------------------------------------- UART
#define U_RXFIFO_FULL 0
#define U_TXFIFO_EMPTY 1
#define U_RXFIFO_OVF 4
#define U_RXFIFO_TOUT 8
typedef struct {
    uint32_t raw, ena, clkdiv, conf0, conf1;
    uint8_t rx[512]; int rx_head, rx_count;
    uint64_t last_rx, timeout_tick;
} uart_t;
static uart_t uart[2];

static void uart_update(int u)
{
    uart_t *U = &uart[u];
    uint32_t full_thr = U->conf1 & 0x7F, tout_en = U->conf1 >> 31;
    // ESP8266 TRM 11.4.3: FULL is count > threshold (including threshold 0).
    // Timeout counts eight UART bit periods per threshold unit, from the last RX.
    if (U->rx_count > (int)full_thr) U->raw |= 1u << U_RXFIFO_FULL;
    U->timeout_tick = UINT64_MAX;
    if (U->rx_count && tout_en && U->clkdiv && !(U->raw & (1u << U_RXFIFO_TOUT))) {
        uint64_t delay = 16ull * U->clkdiv * ((U->conf1 >> 24) & 0x7f);
        U->timeout_tick = U->last_rx + (delay ? delay : 1);
        if (E.tick >= U->timeout_tick) {
            U->raw |= 1u << U_RXFIFO_TOUT;
            U->timeout_tick = UINT64_MAX;
        }
    }
    U->raw |= 1u << U_TXFIFO_EMPTY;     // TX drains instantly
    uint32_t line = (uart[0].raw & uart[0].ena) | (uart[1].raw & uart[1].ena);
    if (line) E.irq_lines |= 1u << INUM_UART; else E.irq_lines &= ~(1u << INUM_UART);
    irq_update();
    soc_schedule();
}

void soc_uart_rx(int u, const uint8_t *d, int n)
{
    uart_t *U = &uart[u & 1];
    for (int i = 0; i < n; i++) {
        if (U->rx_count >= 128) { U->raw |= 1u << U_RXFIFO_OVF; break; }
        U->rx[(U->rx_head + U->rx_count++) % sizeof U->rx] = d[i];
        U->last_rx = E.tick;
    }
    uart_update(u & 1);
}

static uint32_t uart_read(int u, uint32_t off)
{
    uart_t *U = &uart[u];
    switch (off) {
    case 0x00: {
        if (!U->rx_count) return 0;
        uint8_t b = U->rx[U->rx_head];
        U->rx_head = (U->rx_head + 1) % sizeof U->rx;
        U->rx_count--;
        uart_update(u);
        return b;
    }
    case 0x04: return U->raw;
    case 0x08: return U->raw & U->ena;
    case 0x0C: return U->ena;
    case 0x14: return U->clkdiv;
    case 0x1C: return (uint32_t)(U->rx_count & 0xFF) | (1u << 15) | (1u << 31) | (1u << 13) | (1u << 14);
    case 0x20: return U->conf0;
    case 0x24: return U->conf1;
    case 0x78: return 0x062000;
    case 0x7C: return 0x0500;
    default:   return 0;
    }
}

static void uart_write(int u, uint32_t off, uint32_t v)
{
    uart_t *U = &uart[u];
    switch (off) {
    case 0x00: uart_tx_byte(u, (uint8_t)v); return;
    case 0x0C: U->ena = v; break;
    case 0x10: U->raw &= ~v; break;
    case 0x14: U->clkdiv = v & 0xFFFFF; break;
    case 0x20:
        U->conf0 = v;
        if (v & (1u << 17)) { U->rx_count = 0; U->rx_head = 0; U->raw &= ~((1u << U_RXFIFO_FULL) | (1u << U_RXFIFO_TOUT)); }
        break;
    case 0x24: U->conf1 = v; break;
    default: return;
    }
    uart_update(u);
}

uint32_t soc_uart_baud(int u) { return uart[u].clkdiv ? 80000000u / uart[u].clkdiv : 0; }

static int pin_func(int pin);
// Is this UART's TX routed to a pad? U0TXD = GPIO1 function 0 (or GPIO15 function 4 after Serial.swap());
// U1TXD = GPIO2 function 2. Bytes sent while unrouted never reach the wire on a real board.
int soc_uart_on_pad(int u)
{
    if (u == 0) return pin_func(1) == 0 || pin_func(15) == 4;
    return pin_func(2) == 2;
}

// ---------------------------------------------------------------- GPIO
static uint32_t gpio_out, gpio_en, gpio_status, gpio_pin[16];
static uint32_t gp16_out, gp16_en, gp16_conf, gp16_func;
static int ext_level[17];          // -1 = not driven (pull-up), 0/1 = driven by the outside world
static uint32_t level_now;         // current pin levels 0..16
int pins_dirty;

static const uint8_t iomux_off[16] = { 0x34, 0x18, 0x38, 0x14, 0x3C, 0x40, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x04, 0x08, 0x0C, 0x10 };

static int pin_func(int pin)
{
    uint32_t v = peri[(0x800 + iomux_off[pin]) / 4];
    return (int)(((v >> 4) & 3) | ((v >> 6) & 4));
}
static int gpio_func_of(int pin) { return (pin == 0 || pin == 2 || pin == 4 || pin == 5) ? 0 : 3; }

// Pin mode as the outside world sees it: 'o' GPIO output, 'p' GPIO input with the internal pull-up, 'z' GPIO input
// floating (no pull-up: what the board's own resistors make of it is hardware-dependent), 'u' UART/other function.
static char mode_now[17];

static char pin_mode(int pin)
{
    if (pin == 16) return (gp16_en & 1) ? 'o' : 'z';
    if (pin_func(pin) != gpio_func_of(pin)) return 'u';
    if (gpio_en >> pin & 1) return 'o';
    return (peri[(0x800 + iomux_off[pin]) / 4] & 0x80) ? 'p' : 'z';
}

void soc_pin_state(int pin, int *level, char *mode)
{
    *level = (level_now >> pin) & 1;
    *mode = mode_now[pin] ? mode_now[pin] : pin_mode(pin);
}

static int pin_level(int pin)
{
    if (pin == 16) {
        if (gp16_en & 1) return gp16_out & 1;
        return ext_level[16] >= 0 ? ext_level[16] : (gp16_func & (1u << 3)) ? 0 : 1;
    }
    int is_gpio = pin_func(pin) == gpio_func_of(pin);
    if (is_gpio && (gpio_en >> pin & 1)) return (gpio_out >> pin) & 1;
    if (pin == 1 && !is_gpio) return 1;                         // U0TXD idles high
    if (ext_level[pin] >= 0) return ext_level[pin];
    if (pin == 15) return 0;                                    // boot strap pull-down on modules
    return 1;                                                   // pulled up (or floating, read as high)
}

static void gpio_irq_line(void)
{
    if (gpio_status) E.irq_lines |= 1u << INUM_GPIO; else E.irq_lines &= ~(1u << INUM_GPIO);
    irq_update();
}

void pin_edge(int pin, int level, char mode);   // main.c: timestamped transition record

static void gpio_eval(void)
{
    uint32_t nl = 0;
    for (int p = 0; p <= 16; p++) nl |= (uint32_t)pin_level(p) << p;
    uint32_t changed = nl ^ level_now;
    for (int p = 0; p < 16; p++) {
        uint32_t type = (gpio_pin[p] >> 7) & 7, lv = (nl >> p) & 1, ch = (changed >> p) & 1;
        int hit = (type == 1 && ch && lv) || (type == 2 && ch && !lv) || (type == 3 && ch) ||
                  (type == 4 && !lv) || (type == 5 && lv);
        if (hit) gpio_status |= 1u << p;
        if (E.trace && ch) ev_log("gpio%d -> %u type %u hit %d status %04x", p, lv, type, hit, gpio_status);
    }
    level_now = nl;
    for (int p = 0; p <= 16; p++) {
        char m = pin_mode(p);
        if (m != mode_now[p] || (changed >> p & 1)) {
            mode_now[p] = m;
            pin_edge(p, (int)((nl >> p) & 1), m);
            pins_dirty = 1;
        }
    }
    gpio_irq_line();
}

void soc_set_input(int pin, int level)
{
    if (pin < 0 || pin > 16) return;
    ext_level[pin] = level;
    gpio_eval();
}

static uint32_t gpio_read(uint32_t off)
{
    switch (off) {
    case 0x00: return gpio_out;
    case 0x0C: return gpio_en;
    case 0x18: return (level_now & 0xFFFF) | (3u << 16);       // strapping: boot from flash
    case 0x1C: return gpio_status;
    case 0x70: return (1u << 31) | 0x4E20;                      // RTC_CALIB_VALUE: ready
    default:
        if (off >= 0x28 && off < 0x68) return gpio_pin[(off - 0x28) / 4];
        return peri[(0x300 + off) / 4];
    }
}

static void gpio_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00: gpio_out = v & 0xFFFF; break;
    case 0x04: gpio_out |= v & 0xFFFF; break;
    case 0x08: gpio_out &= ~v; break;
    case 0x0C: gpio_en = v & 0xFFFF; pins_dirty = 1; break;
    case 0x10: gpio_en |= v & 0xFFFF; pins_dirty = 1; break;
    case 0x14: gpio_en &= ~v; pins_dirty = 1; break;
    case 0x1C: gpio_status = v & 0xFFFF; break;
    case 0x20: gpio_status |= v & 0xFFFF; break;
    case 0x24: gpio_status &= ~v; break;
    default:
        if (off >= 0x28 && off < 0x68) { gpio_pin[(off - 0x28) / 4] = v; break; }
        peri[(0x300 + off) / 4] = v;
        return;
    }
    gpio_eval();
}

// ---------------------------------------------------------------- FRC1 / FRC2 timers
typedef struct { uint32_t load, ctrl, count_at; uint64_t ref, expire; int running; } frc1_t;
typedef struct { uint32_t ctrl, count_at, alarm; uint64_t ref, alarm_tick; } frc2_t;
static frc1_t frc1;
static frc2_t frc2;

static uint32_t frc_period(uint32_t ctrl)
{
    uint32_t ps = (ctrl >> 2) & 3;
    return 2u * (ps == 0 ? 1 : ps == 1 ? 16 : 256);
}

static uint32_t frc1_count(void)
{
    if (!frc1.running) return frc1.count_at;
    uint64_t el = (E.tick - frc1.ref) / frc_period(frc1.ctrl);
    return el >= frc1.count_at ? 0 : frc1.count_at - (uint32_t)el;
}

static void frc1_start(void)
{
    frc1.ref = E.tick;
    frc1.running = (frc1.ctrl & 0x80) && frc1.count_at;
    if (frc1.running) frc1.expire = E.tick + (uint64_t)frc1.count_at * frc_period(frc1.ctrl);
}

static uint32_t frc2_count(void)
{
    if (!(frc2.ctrl & 0x80)) return frc2.count_at;
    return frc2.count_at + (uint32_t)((E.tick - frc2.ref) / frc_period(frc2.ctrl));
}

static void frc2_rebase(void)
{
    frc2.count_at = frc2_count();
    frc2.ref = E.tick;
}

static void frc2_sched(void)
{
    if (!(frc2.ctrl & 0x80)) { frc2.alarm_tick = UINT64_MAX; return; }
    uint32_t now = frc2_count();
    uint32_t d = frc2.alarm - now;
    uint64_t per = frc_period(frc2.ctrl);
    // ticks already elapsed in the current FRC2 period are kept so the alarm lands on the count edge
    uint64_t base = frc2.ref + (uint64_t)(now - frc2.count_at) * per;
    frc2.alarm_tick = base + (uint64_t)d * per;
    if (frc2.alarm_tick < E.tick) frc2.alarm_tick = E.tick;
}

static uint32_t timer_read(uint32_t off)
{
    switch (off) {
    case 0x00: return frc1.load;
    case 0x04: return frc1_count();
    case 0x08: return frc1.ctrl;
    case 0x20: return frc2.count_at;
    case 0x24: return frc2_count();
    case 0x28: return frc2.ctrl;
    case 0x30: return frc2.alarm;
    default:   return peri[(0x600 + off) / 4];
    }
}

static void timer_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00: frc1.load = v & 0x7FFFFF; frc1.count_at = frc1.load; frc1_start(); break;
    case 0x08: frc1.count_at = frc1_count(); frc1.ctrl = v; frc1_start(); break;
    case 0x0C: break;                                       // FRC1 int clear
    case 0x20: frc2.count_at = v; frc2.ref = E.tick; frc2_sched(); break;
    case 0x28: frc2_rebase(); frc2.ctrl = v; frc2_sched(); break;
    case 0x2C: break;                                       // FRC2 int clear
    case 0x30: frc2.alarm = v; frc2_sched(); break;
    default:   peri[(0x600 + off) / 4] = v; return;
    }
    soc_schedule();
}

// ---------------------------------------------------------------- hardware watchdog
// Register semantics: Cesanta's measured esp_hw_wdt_register.h (a231435), checked
// against this SDK's wdt_init. Bit 2 disables stage 1; reload n = 2^(16+n)
// 80 MHz APB clocks, independent of the CPU's 80/160 MHz selection.
static struct {
    uint32_t ctrl, reload[2], stage, count;
    uint64_t due;
} wdt;

static uint32_t wdt_count(void)
{
    if (!(wdt.ctrl & 1) || wdt.due == UINT64_MAX) return wdt.count;
    return E.tick >= wdt.due ? 0 : (uint32_t)((wdt.due - E.tick + 1) / 2);
}

static uint32_t wdt_reload(unsigned stage)
{
    return 1u << (16 + (wdt.reload[stage] & 15));
}

static void wdt_feed(void)
{
    wdt.stage = 0;
    wdt.count = wdt_reload(0);
    wdt.due = (wdt.ctrl & 1) && !(dport[0x18 / 4] & 8)
              ? E.tick + 2ull * wdt.count : UINT64_MAX;
}

static uint32_t wdt_read(uint32_t off)
{
    switch (off) {
    case 0: return wdt.ctrl;
    case 4: return wdt.reload[0];
    case 8: return wdt.reload[1];
    case 12: return wdt_count();
    case 16: return wdt.stage;
    default: return 0;
    }
}

static void wdt_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case 0: {
        uint32_t old = wdt.ctrl;
        wdt.count = wdt_count();
        wdt.ctrl = v;
        if (!(v & 1)) wdt.due = UINT64_MAX;
        else if (!(old & 1)) wdt_feed();
        break;
    }
    case 4: wdt.reload[0] = v & 15; break;
    case 8: wdt.reload[1] = v & 15; break;
    case 20: if ((v & 0xff) == 0x73) wdt_feed(); break;
    case 24: if ((v & 0xff) == 0x73) wdt.stage = 0; break;
    }
    soc_schedule();
}

static void wdt_event(void)
{
    while (E.tick >= wdt.due) {
        if ((wdt.ctrl & 4) || (wdt.stage && !(wdt.ctrl & 2))) emu_watchdog_reset();
        if (!wdt.stage) {
            wdt.stage = 1;
            if (dport[1] & 1) irq_raise_edge(INUM_WDT);
        }
        wdt.count = wdt_reload((wdt.ctrl & 2) ? 0 : 1);
        wdt.due += 2ull * wdt.count;
    }
}

// ---------------------------------------------------------------- SPI flash
uint32_t spi_flash_read(uint32_t addr, void *dst, uint32_t n)
{
    if (addr > E.flash_size || n > E.flash_size - addr) return 1;
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++) d[i] = E.flash[addr + i];
    return 0;
}

uint32_t spi_flash_write(uint32_t addr, const void *src, uint32_t n)
{
    if (addr > E.flash_size || n > E.flash_size - addr) return 1;
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++)
        E.flash[addr + i] &= s[i];   // NOR: program clears bits only
    E.flash_dirty = true;
    return 0;
}

uint32_t spi_flash_erase(uint32_t addr, uint32_t n)
{
    if (addr > E.flash_size || n > E.flash_size - addr) return 1;
    memset(E.flash + addr, 0xFF, n);
    E.flash_dirty = true;
    return 0;
}

#define SPI0 (0x200 / 4)
void soc_flash_command(uint32_t cmd)
{
    uint32_t addr = peri[SPI0 + 1] & 0xFFFFFF, len = peri[SPI0 + 1] >> 24;
    uint8_t buf[64];
    if (len > 64) len = 64;
    if (cmd & (1u << 31)) {                                 // READ
        memset(buf, 0, sizeof buf);
        spi_flash_read(addr, buf, len);
        memcpy(&peri[SPI0 + 16], buf, 64);
    } else if (cmd & (1u << 28)) {                          // RDID
        peri[SPI0 + 16] = E.flash_id;
    } else if (cmd & (1u << 27)) {                          // RDSR
        peri[SPI0 + 4] = 0;
        peri[SPI0 + 16] = 0;
    } else if (cmd & (1u << 25)) {                          // PP
        memcpy(buf, &peri[SPI0 + 16], 64);
        spi_flash_write(addr, buf, len);
    } else if (cmd & (1u << 24)) {                          // SE
        spi_flash_erase(addr & ~0xFFFu, 0x1000);
    } else if (cmd & (1u << 23)) {                          // BE
        spi_flash_erase(addr & ~0xFFFFu, 0x10000);
    } else if (cmd & (1u << 22)) {                          // CE
        spi_flash_erase(0, E.flash_size);
    } else if (cmd & (1u << 18)) {                          // USR: user-defined command
        uint32_t op = peri[SPI0 + 9] & 0xFFFF;              // SPI_USER2 command value
        if (op == 0x9F) peri[SPI0 + 16] = E.flash_id;
        else if (op == 0x05 || op == 0x35 || op == 0x15) peri[SPI0 + 16] = 0;
        else if (op == 0x03 || op == 0x0B) { memset(buf, 0, sizeof buf); spi_flash_read(addr, buf, 64); memcpy(&peri[SPI0 + 16], buf, 64); }
    }
    peri[SPI0] = 0;
}

// ---------------------------------------------------------------- DPORT / eFuse / WDEV
static uint32_t efuse[4];
// SDK 2.2.2-dev wDev_EnableTransmit writes these eight queue-control registers.
// Bits 31:30 hand a descriptor to the MAC; bits 26:24 select its ACK policy.
// This focused model completes only no-ACK frames (beacons/broadcast), with
// idealized 300 us airtime. It does not implement reception or acknowledged TX.
#define MAC_INT_ENA  0x20c18u
#define MAC_INT_ST   0x20c20u
#define MAC_INT_CLR  0x20c24u
#define MAC_TX_RESULT 0x20c84u
#define MAC_TX_CTL0  0x20dc4u
#define MAC_TX_STRIDE 24u
#define MAC_TX_QUEUES 8u
#define MAC_TX_ENABLE 0xc0000000u
#define MAC_TX_ACK_POLICY 0x07000000u
#define MAC_TX_START_DONE 0x000c0000u
static uint64_t mac_tx_due[MAC_TX_QUEUES];
static uint64_t mac_timer1_due = UINT64_MAX;

static void mac_timer1_schedule(void)
{
    if (!(dport[0x210c8 / 4] & 0x80000000u)) {
        mac_timer1_due = UINT64_MAX;
        return;
    }
    uint64_t target = ((uint64_t)dport[0x210d0 / 4] << 32) | dport[0x210cc / 4];
    uint64_t current = now_us();
    mac_timer1_due = target <= current ? E.tick : E.tick - (E.tick - E.tick_epoch) % 160 + (target - current) * 160;
}


static void mac_irq(void)
{
    // Despite its name, wDev_ProcessFiq is attached by this SDK to interrupt 0.
    // Its NMI vector services timer1; routing MAC status to NMI loses completion.
    if (dport[MAC_INT_ST / 4] & dport[MAC_INT_ENA / 4]) E.irq_lines |= 1u << INUM_WDEV;
    else E.irq_lines &= ~(1u << INUM_WDEV);
    irq_update();
}

static uint32_t dport_read(uint32_t off)
{
    switch (off) {
    case 0x50: case 0x54: case 0x58: case 0x5C: return efuse[(off - 0x50) / 4];
    case 0x20C00: return (uint32_t)now_us();                 // WDEV free-running microsecond counter
    // SDK beacon scheduling reads this 64-bit TSF counter. A static zero makes
    // wDev_Get_Next_TBTT loop through a 32-bit beacon-interval wrap each call.
    case 0x21048: return (uint32_t)now_us();
    case 0x2104c: return (uint32_t)(now_us() >> 32);
    case 0x20E44:                                            // WDEV random number register
        rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
        return rng_state;
    default: return dport[off / 4];
    }
}

static void dport_write(uint32_t off, uint32_t v)
{
    uint32_t old = dport[off / 4];
    if (off == MAC_INT_CLR) {
        dport[MAC_INT_ST / 4] &= ~v;       // write-one-to-clear, before SDK dispatch
        mac_irq();
        soc_schedule();
        return;
    }
    dport[off / 4] = v;
    if (off == MAC_INT_ENA) mac_irq();
    if (off == 0x210c8 || off == 0x210cc || off == 0x210d0) {
        mac_timer1_schedule();
        soc_schedule();
    }
    if (off == 0x18 && ((old ^ v) & 8)) {
        if (v & 8) { wdt.count = wdt_count(); wdt.due = UINT64_MAX; }
        else if (wdt.ctrl & 1) wdt.due = E.tick + 2ull * wdt.count;
        soc_schedule();
    }
    if (off <= MAC_TX_CTL0 && off >= MAC_TX_CTL0 - (MAC_TX_QUEUES - 1) * MAC_TX_STRIDE &&
        (MAC_TX_CTL0 - off) % MAC_TX_STRIDE == 0) {
        unsigned q = (MAC_TX_CTL0 - off) / MAC_TX_STRIDE;
        if ((v & MAC_TX_ENABLE) != MAC_TX_ENABLE || (v & MAC_TX_ACK_POLICY))
            mac_tx_due[q] = UINT64_MAX;
        else if ((old & MAC_TX_ENABLE) != MAC_TX_ENABLE)
            mac_tx_due[q] = E.tick + 300u * 160u;
        soc_schedule();
    }
    if (off == 0x14 && ((v & 1) ? 160u : 80u) != E.cpu_mhz) cpu_set_freq((v & 1) ? 160 : 80);
}

// ---------------------------------------------------------------- peripheral dispatch
static uint32_t peri_read(uint32_t off)
{
    if (off >= 0x900 && off <= 0x918) return wdt_read(off - 0x900);
    if (off < 0x100) return uart_read(0, off);
    if (off >= 0xF00 && off < 0x1000) return uart_read(1, off - 0xF00);
    if (off >= 0x300 && off < 0x400) return gpio_read(off - 0x300);
    if (off >= 0x600 && off < 0x700) return timer_read(off - 0x600);
    switch (off) {
    case 0x768: return gp16_out;
    case 0x774: return gp16_en;
    case 0x78C: return (level_now >> 16) & 1;
    case 0x790: return gp16_conf;
    case 0x7A0: return gp16_func;
    case 0x71C: return (uint32_t)(now_us() * 150 / 1000);     // RTC slow counter (~150 kHz)
    case 0x728: return peri[0x728 / 4] | 1;                   // RTC wake status: pm_wait4wakeup() polls bits 1:0
    }
    return peri[off / 4];
}

static void peri_write(uint32_t off, uint32_t v)
{
    if (off >= 0x900 && off <= 0x918) { wdt_write(off - 0x900, v); return; }
    if (off < 0x100) { uart_write(0, off, v); return; }
    if (off >= 0xF00 && off < 0x1000) { uart_write(1, off - 0xF00, v); return; }
    if (off >= 0x300 && off < 0x400) { gpio_write(off - 0x300, v); return; }
    if (off >= 0x600 && off < 0x700) { timer_write(off - 0x600, v); return; }
    peri[off / 4] = v;
    switch (off) {
    case 0x200: soc_flash_command(v); break;                  // SPI0 CMD
    case 0x768: gp16_out = v; gpio_eval(); break;
    case 0x774: gp16_en = v; pins_dirty = 1; gpio_eval(); break;
    case 0x790: gp16_conf = v; break;
    case 0x7A0: gp16_func = v; gpio_eval(); break;
    default:
        if (off >= 0x800 && off < 0x844) { pins_dirty = 1; gpio_eval(); }   // IO_MUX: function/pull-up change
        break;
    }
}

// ---------------------------------------------------------------- memory map
const uint8_t *fetch_ptr(uint32_t pc)
{
    if (pc - IRAM_BASE < IRAM_SIZE) return mem_iram + (pc - IRAM_BASE);
    if (pc - FLASHMAP_BASE < FLASHMAP_SIZE) {
        uint32_t fo = E.flash_map_off + (pc - FLASHMAP_BASE);
        if (fo + 4 <= E.flash_size) return E.flash + fo;
    }
    return NULL;
}

static uint32_t mmio_ld32(uint32_t a, int *ok)
{
    *ok = 1;
    if (a - DRAM_BASE < DRAM_SIZE) return rd32le(mem_dram + (a - DRAM_BASE));
    if (a - IRAM_BASE < IRAM_SIZE) return rd32le(mem_iram + (a - IRAM_BASE));
    if (a - FLASHMAP_BASE < FLASHMAP_SIZE) {
        uint32_t fo = E.flash_map_off + (a - FLASHMAP_BASE);
        return fo + 4 <= E.flash_size ? rd32le(E.flash + fo) : 0xFFFFFFFFu;
    }
    if (a - ROM_BASE < ROM_SIZE) return rd32le(mem_rom + (a - ROM_BASE));
    if (a - RTCMEM_BASE < RTCMEM_SIZE) return rd32le(mem_rtc + (a - RTCMEM_BASE));
    if (a - PERI_BASE < PERI_SIZE) return peri_read((a - PERI_BASE) & ~3u);
    if (a - DPORT_BASE < DPORT_SIZE) return dport_read((a - DPORT_BASE) & ~3u);
    *ok = 0;
    return 0;
}

static int mmio_st32(uint32_t a, uint32_t v)
{
    if (a - DRAM_BASE < DRAM_SIZE) { memcpy(mem_dram + (a - DRAM_BASE), &v, 4); return 1; }
    if (a - IRAM_BASE < IRAM_SIZE) { memcpy(mem_iram + (a - IRAM_BASE), &v, 4); return 1; }
    if (a - RTCMEM_BASE < RTCMEM_SIZE) { memcpy(mem_rtc + (a - RTCMEM_BASE), &v, 4); return 1; }
    if (a - PERI_BASE < PERI_SIZE) { peri_write((a - PERI_BASE) & ~3u, v); return 1; }
    if (a - DPORT_BASE < DPORT_SIZE) { dport_write((a - DPORT_BASE) & ~3u, v); return 1; }
    return 0;
}

static int is_mmio(uint32_t a) { return a - PERI_BASE < PERI_SIZE || a - DPORT_BASE < DPORT_SIZE; }

uint32_t mem_ld32(uint32_t a)
{
    int ok; uint32_t v = mmio_ld32(a, &ok);
    if (E.mtrace_hi && a >= E.mtrace_lo && a <= E.mtrace_hi) mtrace_log(0, a, v);
    if (!ok) fault(EXC_LOAD_PROHIB, a);
    return v;
}

uint32_t mem_ld16(uint32_t a)
{
    int ok; uint32_t v;
    if (is_mmio(a)) { v = mmio_ld32(a & ~3u, &ok); v >>= (a & 2) * 8; }
    else { uint8_t lo = (uint8_t)mmio_ld32(a, &ok); if (!ok) fault(EXC_LOAD_PROHIB, a); uint8_t hi = (uint8_t)mmio_ld32(a + 1, &ok); v = lo | (hi << 8); }
    if (!ok) fault(EXC_LOAD_PROHIB, a);
    return v & 0xFFFF;
}

uint32_t mem_ld8(uint32_t a)
{
    int ok; uint32_t v;
    if (is_mmio(a)) { v = mmio_ld32(a & ~3u, &ok) >> ((a & 3) * 8); }
    else v = mmio_ld32(a, &ok);
    if (!ok) fault(EXC_LOAD_PROHIB, a);
    return v & 0xFF;
}

void mem_st32(uint32_t a, uint32_t v)
{
    if (E.mtrace_hi && a >= E.mtrace_lo && a <= E.mtrace_hi) mtrace_log(1, a, v);
    if (!mmio_st32(a, v)) fault(EXC_STORE_PROHIB, a);
}

static void st_sub(uint32_t a, uint32_t v, int bytes)
{
    if (is_mmio(a)) {
        uint32_t al = a & ~3u, sh = (a & 3) * 8, mask = (bytes == 1 ? 0xFFu : 0xFFFFu) << sh;
        int ok; uint32_t old = mmio_ld32(al, &ok);
        if (al - PERI_BASE < 4 || al - (PERI_BASE + 0xF00) < 4) old = 0;   // UART FIFO: write the byte itself
        mmio_st32(al, (old & ~mask) | ((v << sh) & mask));
        return;
    }
    uint8_t *p = NULL;
    if (a - DRAM_BASE < DRAM_SIZE) p = mem_dram + (a - DRAM_BASE);
    else if (a - IRAM_BASE < IRAM_SIZE) p = mem_iram + (a - IRAM_BASE);
    else if (a - RTCMEM_BASE < RTCMEM_SIZE) p = mem_rtc + (a - RTCMEM_BASE);
    if (!p) fault(EXC_STORE_PROHIB, a);
    p[0] = (uint8_t)v;
    if (bytes == 2) p[1] = (uint8_t)(v >> 8);
}
void mem_st16(uint32_t a, uint32_t v) { st_sub(a, v, 2); }
void mem_st8(uint32_t a, uint32_t v) { st_sub(a, v, 1); }

// Non-faulting accessors for HLE code (bad pointers are logged, reads return 0).
uint8_t *guest_ptr(uint32_t a, uint32_t n)
{
    if (a - DRAM_BASE < DRAM_SIZE && n <= DRAM_SIZE - (a - DRAM_BASE)) return mem_dram + (a - DRAM_BASE);
    if (a - IRAM_BASE < IRAM_SIZE && n <= IRAM_SIZE - (a - IRAM_BASE)) return mem_iram + (a - IRAM_BASE);
    if (a - RTCMEM_BASE < RTCMEM_SIZE && n <= RTCMEM_SIZE - (a - RTCMEM_BASE)) return mem_rtc + (a - RTCMEM_BASE);
    return NULL;
}

uint8_t hle_rd8(uint32_t a)
{
    int ok; uint32_t v;
    if (is_mmio(a)) v = mmio_ld32(a & ~3u, &ok) >> ((a & 3) * 8);
    else v = mmio_ld32(a, &ok);
    if (!ok) { ev_log("HLE read from unmapped %08x (pc %08x)", a, C.pc); return 0; }
    return (uint8_t)v;
}

uint32_t hle_rd32(uint32_t a)
{
    int ok; uint32_t v = mmio_ld32(a, &ok);
    if (!ok) { ev_log("HLE read from unmapped %08x (pc %08x)", a, C.pc); return 0; }
    return v;
}

void hle_wr8(uint32_t a, uint8_t v)
{
    uint8_t *p = guest_ptr(a, 1);
    if (p) { *p = v; return; }
    if (is_mmio(a)) {
        int ok; uint32_t al = a & ~3u, sh = (a & 3) * 8, old = mmio_ld32(al, &ok);
        mmio_st32(al, (old & ~(0xFFu << sh)) | ((uint32_t)v << sh));
        return;
    }
    ev_log("HLE write to unmapped %08x (pc %08x)", a, C.pc);
}

void hle_wr32(uint32_t a, uint32_t v)
{
    if (!mmio_st32(a, v)) ev_log("HLE write to unmapped %08x (pc %08x)", a, C.pc);
}

void hle_read(uint32_t a, void *dst, uint32_t n)
{
    uint8_t *p = guest_ptr(a, n);
    if (p) { memcpy(dst, p, n); return; }
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = hle_rd8(a + i);
}

void hle_write(uint32_t a, const void *src, uint32_t n)
{
    uint8_t *p = guest_ptr(a, n);
    if (p) { memcpy(p, src, n); return; }
    for (uint32_t i = 0; i < n; i++) hle_wr8(a + i, ((const uint8_t *)src)[i]);
}

// ---------------------------------------------------------------- scheduling
void soc_schedule(void)
{
    uint64_t n = E.slice_end;
    if (wdt.due < n) n = wdt.due;
    if (mac_timer1_due < n) n = mac_timer1_due;
    for (int u = 0; u < 2; u++) if (uart[u].timeout_tick < n) n = uart[u].timeout_tick;
    if (E.ccomp_tick < n) n = E.ccomp_tick;
    if (frc1.running && frc1.expire < n) n = frc1.expire;
    if (frc2.alarm_tick < n) n = frc2.alarm_tick;
    // TX_RESULT holds one completion. Preserve it until the driver acknowledges
    // the status, even if another queue has already reached its completion time.
    if (!(dport[MAC_INT_ST / 4] & MAC_TX_START_DONE))
        for (unsigned q = 0; q < MAC_TX_QUEUES; q++) if (mac_tx_due[q] < n) n = mac_tx_due[q];
    E.next_event = n;
}

void soc_events(void)
{
    uint64_t now = E.tick;
    if (now >= wdt.due) wdt_event();
    if (now >= mac_timer1_due) {
        mac_timer1_due = UINT64_MAX;
        dport[0x210c8 / 4] &= ~0x80000000u;
        if (dport[0x21058 / 4] & 0x80000000u) {
            dport[MAC_INT_ST / 4] |= 1u << 26;
            mac_irq();
        }
    }
    if (!(dport[MAC_INT_ST / 4] & MAC_TX_START_DONE)) {
        for (unsigned q = 0; q < MAC_TX_QUEUES; q++) if (now >= mac_tx_due[q]) {
            mac_tx_due[q] = UINT64_MAX;
            dport[(MAC_TX_CTL0 - q * MAC_TX_STRIDE) / 4] &= ~MAC_TX_ENABLE;
            dport[MAC_TX_RESULT / 4] = q << 12;  // queue number; result bits 31:28 = success
            // The SDK handles coalesced TX-start/TX-done: it first records the
            // active frame, then runs lmacProcessTxSuccess and releases it itself.
            dport[MAC_INT_ST / 4] |= MAC_TX_START_DONE;
            mac_irq();
            break;
        }
    }
    for (int u = 0; u < 2; u++) if (now >= uart[u].timeout_tick) uart_update(u);
    if (now >= E.ccomp_tick) {
        C.intpend |= 1u << INUM_CCOMP;
        irq_update();
        E.ccomp_tick += 0x100000000ull * E.tpc;
    }
    if (frc1.running && now >= frc1.expire) {
        int nmi = dport[0] & 1;
        irq_raise_edge(nmi ? INUM_NMI : INUM_FRC1);
        if ((frc1.ctrl & 0x40) && frc1.load) {
            frc1.count_at = frc1.load;
            frc1.ref = frc1.expire;
            frc1.expire += (uint64_t)frc1.load * frc_period(frc1.ctrl);
        } else {
            frc1.count_at = 0;
            frc1.running = 0;
        }
    }
    if (now >= frc2.alarm_tick) {
        irq_raise_edge(INUM_FRC2);
        frc2.alarm_tick += 0x100000000ull * frc_period(frc2.ctrl);
    }
    if (now >= E.slice_end) {
        E.slice_end = now + SLICE_TICKS;
        soc_schedule();
        host_service();
    }
    soc_schedule();
}

// ---------------------------------------------------------------- reset
void soc_reset(bool keep_rtc)
{
    memset(peri, 0, sizeof peri);
    memset(dport, 0, sizeof dport);
    memset(&wdt, 0, sizeof wdt);
    wdt.due = mac_timer1_due = UINT64_MAX;
    for (unsigned q = 0; q < MAC_TX_QUEUES; q++) mac_tx_due[q] = UINT64_MAX;
    memset(uart, 0, sizeof uart);
    uart[0].timeout_tick = uart[1].timeout_tick = UINT64_MAX;
    gpio_out = gpio_en = gpio_status = 0;
    memset(gpio_pin, 0, sizeof gpio_pin);
    gp16_out = gp16_en = gp16_conf = gp16_func = 0;
    memset(&frc1, 0, sizeof frc1);
    memset(&frc2, 0, sizeof frc2);
    frc2.alarm_tick = UINT64_MAX;
    // IO_MUX reset: GPIO1/GPIO3 are U0TXD/U0RXD (function 0), pull-ups on
    for (int p = 0; p < 16; p++) peri[(0x800 + iomux_off[p]) / 4] = (p == 1 || p == 3) ? 0x80 : (gpio_func_of(p) == 0 ? 0x80 : 0x80);
    uart[0].clkdiv = uart[1].clkdiv = 80000000 / 74880 * 26 / 40;  // ROM sets 74880 baud on a 26 MHz crystal
    uart[0].conf0 = uart[1].conf0 = 0x1C;
    if (!keep_rtc) memset(mem_rtc, 0, sizeof mem_rtc);
    memset(mem_dram, 0, sizeof mem_dram);
    memset(mem_iram, 0, sizeof mem_iram);
    E.irq_lines = 0;
    E.flash_map_off = 0;
    cpu_fetch_invalidate();
    // eFuse as the SDK's call_user_start_local reads it (base 0x3FEFFE00 + 0x250..0x25C): word 2 bit 15 = ESP8266
    // family (else "Firmware ONLY supports ESP8266!!!"), bit 12 clear = default OUI 18:FE:34, so the MAC is
    // 18:FE:34:EE:B0:01 (NIC bytes made up). Chip identity is independent of
    // capacity: an ESP8266 can have 1 MiB external flash, and ESP8285H16 has 2 MiB.
    efuse[0] = 0x01000000u | (E.chip_profile && E.chip_profile->embedded_flash ? 0x10 : 0);
    efuse[1] = 0x0000EEB0u;
    efuse[2] = 0x00008000u;
    efuse[3] = 0;
    level_now = 0;
    memset(mode_now, 0, sizeof mode_now);
    gpio_eval();
    gpio_status = 0;
    gpio_irq_line();
    pins_dirty = 1;
}

void soc_power_on(void)
{
    for (int i = 0; i <= 16; i++) ext_level[i] = -1;
    soc_reset(false);
}
