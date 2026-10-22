// ESP8266 chip emulator: shared state.
// One emulated chip per process; all state lives in the global E (and cpu fields inside it).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>

#define DRAM_BASE      0x3FFE8000u
#define DRAM_SIZE      0x00018000u   // 96 KiB, 0x3FFE8000..0x3FFFFFFF
#define IRAM_BASE      0x40100000u
#define IRAM_SIZE      0x00010000u   // 64 KiB window (32 KiB IRAM + cache-as-IRAM area + eboot at 0x4010F000)
#define ROM_BASE       0x40000000u
#define ROM_SIZE       0x00010000u
#define FLASHMAP_BASE  0x40200000u
#define FLASHMAP_SIZE  0x00100000u   // 1 MiB cache window
#define RTCMEM_BASE    0x60001000u
#define RTCMEM_SIZE    0x00001000u
#define DPORT_BASE     0x3FF00000u
#define DPORT_SIZE     0x00040000u   // DPORT + WDEV/MAC register space, plain registers unless modelled
#define PERI_BASE      0x60000000u
#define PERI_SIZE      0x00010000u   // 0x60000000..0x6000FFFF (RTC memory at 0x60001000 is checked first)

// Exception causes
#define EXC_ILLEGAL        0
#define EXC_SYSCALL        1
#define EXC_IFETCH_ERR     2
#define EXC_LOADSTORE_ERR  3
#define EXC_LEVEL1_INT     4
#define EXC_DIV0           6
#define EXC_IFETCH_PROHIB  20
#define EXC_LOAD_PROHIB    28
#define EXC_STORE_PROHIB   29

// PS fields
#define PS_INTLEVEL   0x0000000Fu
#define PS_EXCM       0x00000010u
#define PS_UM         0x00000020u

// Interrupt numbers (lx106 / ESP8266)
#define INUM_WDEV    0
#define INUM_SLC     1
#define INUM_SPI     2
#define INUM_GPIO    4
#define INUM_UART    5
#define INUM_CCOMP   6
#define INUM_SOFT    7
#define INUM_WDT     8
#define INUM_FRC1    9
#define INUM_FRC2    10
#define INUM_NMI     14
#define L1_INT_MASK  0x00003FFFu   // interrupts 0..13 are level 1
#define EDGE_INT_MASK 0x00003F80u  // 7 soft, 8..13 edge: latched in intpend

typedef struct {
    uint32_t a[16];
    uint32_t pc;
    uint32_t sar, ps, vecbase, exccause, excvaddr, depc;
    uint32_t epc[4], eps[4], excsave[4];   // index 1..3
    uint32_t intenable;
    uint32_t intpend;      // latched bits: CCOMPARE (6), soft (7), edge (8..13), NMI (14)
    uint32_t ccompare;
    uint32_t ccount_adj;   // CCOUNT = (uint32_t)cycles + ccount_adj
    uint32_t litbase, scompare1, threadptr, debugcause;
    uint32_t misc[4];
    uint32_t sr_other[256]; // unmodelled special registers read back what was written
    int waiting;           // WAITI in progress
} cpu_t;

enum stop_reason { STOP_NONE = 0, STOP_RET, STOP_RESET, STOP_HALT, STOP_QUIT };

enum boot_stage {
    STAGE_OFF = 0, STAGE_ROM, STAGE_EBOOT, STAGE_APP, STAGE_SDK_MAIN, STAGE_SETUP, STAGE_LOOP, STAGE_HALTED
};

typedef struct {
    uint64_t cycles;        // CPU cycles (1 per instruction)
    uint64_t tick;          // virtual time in 1/160 us units
    uint32_t tpc;           // ticks per cycle: 1 at 160 MHz, 2 at 80 MHz
    uint32_t cpu_mhz;
    uint64_t next_event;    // tick of the next timer/peripheral event
    uint64_t slice_end;     // tick when control returns to the host loop for I/O
    uint64_t ccomp_tick;    // tick when CCOUNT reaches CCOMPARE0
    uint32_t cur_pc;        // PC of the instruction being executed (fault reporting)
    uint32_t irq_lines;     // level-triggered lines from peripherals
    uint32_t irq_req;       // (intpend | irq_lines) & intenable, plus NMI bit
    volatile int stop;      // stop request (enum stop_reason) checked by the run loop
    int call_depth;         // nesting of HLE -> guest calls
    jmp_buf *fault_jmp;     // innermost run loop (guest memory faults abort the instruction)
    jmp_buf reset_jmp;      // top level (reset / power cycle unwinds everything)

    uint8_t *flash; uint32_t flash_size;
    uint32_t flash_map_off;          // flash offset mapped at 0x40200000
    uint32_t flash_id;
    bool flash_dirty;

    int stage;
    int reset_reason;       // ROM rtc_get_reset_reason(): 1 power-on, 2 external, 4 watchdog, ... (see rom.c)
    uint64_t boot_count;
    uint64_t boot_cycles;   // E.cycles when the current boot started
    uint64_t tick_epoch;    // E.tick at the last hardware reset (guest-visible time origin)
    uint64_t tick_first;    // E.tick at process start (always 0)
    uint32_t loop_task, loop_dispatches;
    uint32_t sym_setup, sym_loop;    // optional, from the ELF
    int watch_any;                   // --watch list non-empty: check the watch bitmap on every instruction
    int ctrace;                      // --calltrace window set
    uint64_t ctrace_from, ctrace_to; // emulated us since start
    uint32_t mtrace_lo, mtrace_hi;   // --mmiotrace address range (0 = off)
    uint64_t setup_calls, loop_calls;   // this boot
    uint64_t setup_total, loop_total;   // all boots
    bool trace;
    bool strict;            // stop on unimplemented ROM entry instead of returning 0
} emu_t;

extern emu_t E;
// Separate objects (not members of E) so the compiler can tell guest-memory byte stores never alias CPU registers.
extern cpu_t C;
extern uint8_t mem_dram[DRAM_SIZE + 8], mem_iram[IRAM_SIZE + 8], mem_rom[ROM_SIZE + 8], mem_rtc[RTCMEM_SIZE + 8];
void cpu_fetch_invalidate(void);

// cpu.c
void cpu_reset(void);
int  cpu_run(uint32_t stop_pc);
void cpu_exception(uint32_t cause, uint32_t vaddr, uint32_t fault_pc);
void irq_update(void);
void irq_raise_edge(int n);
uint32_t cpu_ccount(void);
void cpu_set_freq(uint32_t mhz);
void cpu_reschedule_ccompare(void);

// soc.c (memory map + peripherals)
uint32_t mem_ld32(uint32_t a);
uint32_t mem_ld16(uint32_t a);
uint32_t mem_ld8(uint32_t a);
void mem_st32(uint32_t a, uint32_t v);
void mem_st16(uint32_t a, uint32_t v);
void mem_st8(uint32_t a, uint32_t v);
const uint8_t *fetch_ptr(uint32_t pc);
// non-faulting accessors for HLE code
uint32_t hle_rd32(uint32_t a);
uint8_t  hle_rd8(uint32_t a);
void hle_wr32(uint32_t a, uint32_t v);
void hle_wr8(uint32_t a, uint8_t v);
void hle_read(uint32_t a, void *dst, uint32_t n);
void hle_write(uint32_t a, const void *src, uint32_t n);
uint8_t *guest_ptr(uint32_t a, uint32_t n);  // contiguous RAM pointer or NULL

void soc_power_on(void);
void soc_reset(bool keep_rtc);
void soc_events(void);
void soc_schedule(void);
uint64_t now_us(void);
void soc_set_input(int pin, int level);   // external drive: 0/1, -1 = released (pull-up)
void soc_uart_rx(int u, const uint8_t *d, int n);
void soc_flash_command(uint32_t cmd);
uint32_t spi_flash_read(uint32_t addr, void *dst, uint32_t n);
uint32_t spi_flash_write(uint32_t addr, const void *src, uint32_t n);
uint32_t spi_flash_erase(uint32_t addr, uint32_t n);
void uart_tx_byte(int u, uint8_t b);

// rom.c
void rom_init(void);
void rom_boot(void);           // ROM loader: banner, load image at flash 0x0, jump
void watch_hit(uint32_t pc);
void ctrace_log(uint32_t from, uint32_t to);
void mtrace_log(int write, uint32_t addr, uint32_t val);
int watch_test(uint32_t pc);
void watch_add(uint32_t pc, const char *name);
void rom_hle(uint32_t pc);     // PC is inside the ROM region
uint32_t guest_call(uint32_t fn, int nargs, const uint32_t *args);
const char *rom_name(uint32_t pc);

// host.c (main)
void host_service(void);       // called at slice ends: I/O, pacing
void ev_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ev_stage(int stage);
void ev_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void emu_halt(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

void emu_watchdog_reset(void) __attribute__((noreturn));
