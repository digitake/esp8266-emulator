// Open high-level replacement for the ESP8266 mask ROM.
// Every ROM entry point the firmware can reach is a C function, entered when the CPU fetches from that
// ROM address (CALL0 ABI: args a2..a7 then stack, result a2[:a3], return to a0). ROM code that calls
// back into the firmware (putc1 hooks, interrupt handlers, ets_run tasks) does so with real guest calls.
#include "emu.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define A2 C.a[2]
#define A3 C.a[3]
#define A4 C.a[4]
#define A5 C.a[5]
#define A6 C.a[6]
#define A7 C.a[7]
#define RET(v) do { C.a[2] = (uint32_t)(v); C.pc = C.a[0]; } while (0)
#define RET64(v) do { uint64_t r_ = (uint64_t)(v); C.a[2] = (uint32_t)r_; C.a[3] = (uint32_t)(r_ >> 32); C.pc = C.a[0]; } while (0)
#define RETV() do { C.pc = C.a[0]; } while (0)

// ROM-private addresses (inside the ROM region, not used by any real entry point we provide)
#define XTOS_L1INT        0x4000FC00u
#define XTOS_UNHANDLED    0x4000FC04u
#define XTOS_C_WRAPPER    0x4000FC08u
#define IMAGE_RETURNED    0x4000FC0Cu
#define ETS_RUN_LOOP      0x4000FE00u
#define GUEST_RET_BASE    0x4000FD00u   // + 4 * depth
#define ROMFUNC_STUB_BASE 0x4000F000u   // phy_get_romfuncs() table entries point here
#define ROMFUNC_COUNT     256

// ROM data in DRAM (0x3FFFC000.. is reserved for the ROM by the SDK linker script)
#define EXC_TABLE        0x3FFFC000u
#define FLASHCHIP        0x3FFFC714u   // SpiFlashChip *flashchip
#define FLASHCHIP_DATA   0x3FFFC718u   // the struct it points to
#define TE0_TABLE        0x3FFFCCF0u
#define RCONS_TABLE      0x3FFFD0F0u
#define ROMFUNC_TABLE    0x3FFFD400u
#define USER_START_FPTR  0x3FFFDCD0u

#define ADDR_ETS_WRITE_CHAR 0x40001DA0u
#define ADDR_ETS_UART_PUTC1 0x40001DCCu
#define ADDR_ETS_PUTC       0x40002BE8u

static struct { uint32_t fn, arg; } isr[16];
static uint32_t c_handler[64];
static struct { uint32_t fn, queue, qlen, head, count; } task[33]; // SDK PP task uses priority 32 (highest), Arduino loop uses 1
static uint32_t idle_cb, idle_arg, run_sp;
static int idle_called;
static uint32_t wdt_saved_ctrl;
static uint32_t putc1, printf_uart, rom_cpu_freq = 80;
static uint8_t romfunc_seen[ROMFUNC_COUNT];

static uint32_t sarg(int i) { return hle_rd32(C.a[1] + 4u * (uint32_t)i); }   // 7th+ argument

// ---------------------------------------------------------------- guest calls
uint32_t guest_call(uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t saved[16], saved_pc = C.pc;
    memcpy(saved, C.a, sizeof saved);
    if (nargs < 0 || nargs > 64 || (nargs && !args)) emu_halt("invalid guest call arguments");
    if (E.call_depth >= 60) emu_halt("ROM->firmware call nesting too deep (pc %08x)", fn);
    // CALL0 passes six arguments in registers; remaining words begin at the
    // callee's entry SP. Reserve an aligned argument area below the caller's
    // frame and restore it afterward, rather than overwriting caller locals.
    uint8_t stack_saved[240];
    uint32_t stack_size = nargs > 6 ? ((uint32_t)(nargs - 6) * 4 + 15) & ~15u : 0;
    uint32_t stack = C.a[1] - stack_size;
    if (stack_size) {
        if (C.a[1] < stack_size || stack < DRAM_BASE ||
            stack_size > DRAM_SIZE || stack - DRAM_BASE > DRAM_SIZE - stack_size)
            emu_halt("guest call stack is outside DRAM");
        hle_read(stack, stack_saved, stack_size);
        for (int i = 6; i < nargs; i++) hle_wr32(stack + (uint32_t)(i - 6) * 4, args[i]);
        C.a[1] = stack;
    }
    uint32_t ret = GUEST_RET_BASE + 4u * (uint32_t)E.call_depth++;
    for (int i = 0; i < nargs && i < 6; i++) C.a[2 + i] = args[i];
    C.a[0] = ret;
    C.pc = fn;
    cpu_run(ret);
    uint32_t rv = C.a[2];
    E.call_depth--;
    if (stack_size) hle_write(stack, stack_saved, stack_size);
    memcpy(C.a, saved, sizeof saved);
    C.pc = saved_pc;
    return rv;
}

// ---------------------------------------------------------------- console
static void uart_putc_raw(int c) { uart_tx_byte(printf_uart & 1, (uint8_t)c); }
static void uart_putc1_native(int c) { if (c == '\n') uart_putc_raw('\r'); uart_putc_raw(c); }

static void out_char(uint32_t fn, int c);
static void write_char_native(int c) { if (putc1) out_char(putc1, c); }

static void out_char(uint32_t fn, int c)
{
    c &= 0xFF;
    if (fn == ADDR_ETS_UART_PUTC1) uart_putc1_native(c);
    else if (fn == ADDR_ETS_WRITE_CHAR || fn == ADDR_ETS_PUTC) write_char_native(c);
    else if (fn) { uint32_t a = (uint32_t)c; guest_call(fn, 1, &a); }
}

typedef struct {
    int va;                    // 0: plain varargs from registers/stack, 1: xtensa va_list
    uint32_t regs[6]; int nregs, idx; uint32_t stk;
    uint32_t va_stk, va_reg; int32_t va_ndx;
} argp_t;

static uint32_t arg_next(argp_t *ap)
{
    if (!ap->va) {
        int i = ap->idx++;
        return i < ap->nregs ? ap->regs[i] : hle_rd32(ap->stk + 4u * (uint32_t)(i - ap->nregs));
    }
    int32_t orig = ap->va_ndx, nd = orig + 4;
    if (nd <= 24) { ap->va_ndx = nd; return hle_rd32(ap->va_reg + (uint32_t)nd - 4); }
    if (orig <= 24) nd = 32 + 4;
    ap->va_ndx = nd;
    return hle_rd32(ap->va_stk + (uint32_t)nd - 4);
}

static uint64_t arg_next64(argp_t *ap)
{
    // Alignment is relative to a2, not the first variadic register (a3).
    if (!ap->va) { if ((ap->idx + 6 - ap->nregs) & 1) ap->idx++; }
    else ap->va_ndx = (ap->va_ndx + 7) & ~7;
    uint32_t lo = arg_next(ap), hi = arg_next(ap);
    return lo | ((uint64_t)hi << 32);
}

static int format(uint32_t putc_fn, uint32_t fmt, argp_t *ap)
{
    int n = 0;
    char buf[80];
    for (int guard = 0; guard < 4096; guard++) {
        int ch = hle_rd8(fmt++);
        if (!ch) break;
        if (ch != '%') { out_char(putc_fn, ch); n++; continue; }
        int left = 0, zero = 0, width = 0, prec = -1, lcount = 0;
        for (;;) {
            ch = hle_rd8(fmt);
            if (ch == '-') left = 1; else if (ch == '0') zero = 1; else if (ch == '+' || ch == ' ' || ch == '#') {} else break;
            fmt++;
        }
        if (ch == '*') { width = (int)arg_next(ap); fmt++; ch = hle_rd8(fmt); }
        while (ch >= '0' && ch <= '9') { width = width < 4096 ? width * 10 + ch - '0' : 4096; ch = hle_rd8(++fmt); }
        if (ch == '.') {
            prec = 0; ch = hle_rd8(++fmt);
            if (ch == '*') { prec = (int)arg_next(ap); ch = hle_rd8(++fmt); }
            while (ch >= '0' && ch <= '9') { prec = prec < 4096 ? prec * 10 + ch - '0' : 4096; ch = hle_rd8(++fmt); }
        }
        while (ch == 'l' || ch == 'h' || ch == 'z') { if (ch == 'l') lcount++; ch = hle_rd8(++fmt); }
        fmt++;
        const char *s = buf; int len = 0;
        switch (ch) {
        case 'd': case 'i': {
            int64_t v = lcount >= 2 ? (int64_t)arg_next64(ap) : (int64_t)(int32_t)arg_next(ap);
            len = snprintf(buf, sizeof buf, "%lld", (long long)v); break;
        }
        case 'u': {
            uint64_t v = lcount >= 2 ? arg_next64(ap) : arg_next(ap);
            len = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v); break;
        }
        case 'x': case 'X': case 'p': {
            uint64_t v = lcount >= 2 ? arg_next64(ap) : arg_next(ap);
            len = snprintf(buf, sizeof buf, ch == 'X' ? "%llX" : "%llx", (unsigned long long)v);
            if (ch == 'p') { memmove(buf + 2, buf, (size_t)len + 1); buf[0] = '0'; buf[1] = 'x'; len += 2; }
            break;
        }
        case 'o': len = snprintf(buf, sizeof buf, "%o", arg_next(ap)); break;
        case 'c': buf[0] = (char)arg_next(ap); len = 1; break;
        case 's': {
            uint32_t sp = arg_next(ap);
            static char sbuf[1024];
            if (!sp) { strcpy(sbuf, "(null)"); len = 6; }
            else for (len = 0; len < (int)sizeof sbuf - 1 && (prec < 0 || len < prec); len++) { char c = (char)hle_rd8(sp + (uint32_t)len); if (!c) break; sbuf[len] = c; }
            s = sbuf; zero = 0;
            break;
        }
        case 'f': case 'g': case 'e': {
            uint64_t bits = arg_next64(ap); double d; memcpy(&d, &bits, 8);
            len = snprintf(buf, sizeof buf, "%.*f", prec < 0 ? 6 : prec, d); break;
        }
        case '%': buf[0] = '%'; len = 1; break;
        default: buf[0] = '%'; buf[1] = (char)ch; len = 2; break;
        }
        // snprintf reports the untruncated length; never read beyond its local buffer.
        if (s == buf && len >= (int)sizeof buf) len = (int)sizeof buf - 1;
        int pad = width > len ? width - len : 0;
        if (!left) for (int i = 0; i < pad; i++) { out_char(putc_fn, zero ? '0' : ' '); n++; }
        for (int i = 0; i < len; i++) { out_char(putc_fn, s[i]); n++; }
        if (left) for (int i = 0; i < pad; i++) { out_char(putc_fn, ' '); n++; }
    }
    return n;
}

static void rom_printf_native(const char *fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    for (char *p = b; *p; p++) uart_putc_raw(*p);
}

// ---------------------------------------------------------------- memory/string helpers
static uint32_t g_strlen(uint32_t s, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && hle_rd8(s + n)) n++;
    return n;
}

static void g_memmove(uint32_t dst, uint32_t src, uint32_t n)
{
    if (!n) return;
    uint8_t *d = guest_ptr(dst, n), *s = guest_ptr(src, n);
    if (d && s) { memmove(d, s, n); return; }
    uint8_t *tmp = malloc(n);
    hle_read(src, tmp, n);
    hle_write(dst, tmp, n);
    free(tmp);
}

static int g_memcmp(uint32_t a, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int x = hle_rd8(a + i), y = hle_rd8(b + i);
        if (x != y) return x - y;
    }
    return 0;
}

static int g_strncmp(uint32_t a, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int x = hle_rd8(a + i), y = hle_rd8(b + i);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static uint8_t *g_copy_in(uint32_t a, uint32_t n)
{
    uint8_t *p = malloc(n ? n : 1);
    hle_read(a, p, n);
    return p;
}

static char *g_cstr(uint32_t a)
{
    uint32_t n = g_strlen(a, 4096);
    char *p = malloc(n + 1);
    hle_read(a, p, n);
    p[n] = 0;
    return p;
}

// ---------------------------------------------------------------- floats
static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static uint32_t u32f(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static double f64(uint32_t lo, uint32_t hi) { uint64_t v = lo | ((uint64_t)hi << 32); double d; memcpy(&d, &v, 8); return d; }
static uint64_t u64d(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static uint32_t fix_u32(double d) { if (!(d > 0)) return 0; if (d >= 4294967296.0) return 0xFFFFFFFFu; return (uint32_t)d; }
static int32_t fix_s32(double d) { if (d != d) return 0; if (d >= 2147483647.0) return INT32_MAX; if (d <= -2147483648.0) return INT32_MIN; return (int32_t)d; }

// ---------------------------------------------------------------- ROM functions
#define FN(name) static void name(void)

FN(r_reset_vector) { longjmp(E.reset_jmp, 3); }
FN(r_image_returned) { emu_halt("boot image returned to the ROM (pc %08x)", C.a[0]); }

FN(r_memset)  { uint32_t d = A2, n = A4; uint8_t *p = guest_ptr(d, n); if (p) memset(p, (int)A3, n); else for (uint32_t i = 0; i < n; i++) hle_wr8(d + i, (uint8_t)A3); RET(d); }
FN(r_bzero)   { uint32_t d = A2, n = A3; uint8_t *p = guest_ptr(d, n); if (p) memset(p, 0, n); else for (uint32_t i = 0; i < n; i++) hle_wr8(d + i, 0); RETV(); }
FN(r_memcpy)  { g_memmove(A2, A3, A4); RET(A2); }
FN(r_memmove) { g_memmove(A2, A3, A4); RET(A2); }
FN(r_memcmp)  { RET(g_memcmp(A2, A3, A4)); }
FN(r_strlen)  { RET(g_strlen(A2, 0x100000)); }
FN(r_strcmp)  { RET(g_strncmp(A2, A3, 0xFFFFFFFFu)); }
FN(r_strncmp) { RET(g_strncmp(A2, A3, A4)); }
FN(r_strcpy)  { uint32_t d = A2, s = A3, i = 0; uint8_t c; do { c = hle_rd8(s + i); hle_wr8(d + i, c); i++; } while (c && i < 0x100000); RET(d); }
FN(r_strncpy) { uint32_t d = A2, s = A3, n = A4, i = 0; int end = 0; for (; i < n; i++) { uint8_t c = end ? 0 : hle_rd8(s + i); if (!c) end = 1; hle_wr8(d + i, c); } RET(d); }
FN(r_strstr)  {
    uint32_t h = A2, nd = A3, nl = g_strlen(nd, 4096), hl = g_strlen(h, 0x100000);
    if (!nl) { RET(h); return; }
    for (uint32_t i = 0; i + nl <= hl; i++) if (!g_strncmp(h + i, nd, nl)) { RET(h + i); return; }
    RET(0);
}

FN(r_udivsi3) { RET(A3 ? A2 / A3 : 0); }
FN(r_umodsi3) { RET(A3 ? A2 % A3 : 0); }
FN(r_divsi3)  { int32_t a = (int32_t)A2, b = (int32_t)A3; RET(b ? (a == INT32_MIN && b == -1 ? a : a / b) : 0); }
FN(r_muldi3)  { RET64(((uint64_t)A2 | ((uint64_t)A3 << 32)) * ((uint64_t)A4 | ((uint64_t)A5 << 32))); }
FN(r_udivdi3) { uint64_t a = A2 | ((uint64_t)A3 << 32), b = A4 | ((uint64_t)A5 << 32); RET64(b ? a / b : 0); }
FN(r_umoddi3) { uint64_t a = A2 | ((uint64_t)A3 << 32), b = A4 | ((uint64_t)A5 << 32); RET64(b ? a % b : 0); }
FN(r_divdi3)  { int64_t a = (int64_t)(A2 | ((uint64_t)A3 << 32)), b = (int64_t)(A4 | ((uint64_t)A5 << 32)); RET64(b ? (uint64_t)(a == INT64_MIN && b == -1 ? a : a / b) : 0); }
FN(r_umulsidi3) { RET64((uint64_t)A2 * A3); }

FN(r_addsf3) { RET(u32f(f32(A2) + f32(A3))); }
FN(r_subsf3) { RET(u32f(f32(A2) - f32(A3))); }
FN(r_mulsf3) { RET(u32f(f32(A2) * f32(A3))); }
FN(r_fixunssfsi) { RET(fix_u32(f32(A2))); }
FN(r_floatsisf) { RET(u32f((float)(int32_t)A2)); }
FN(r_floatunsisf) { RET(u32f((float)A2)); }
FN(r_adddf3) { RET64(u64d(f64(A2, A3) + f64(A4, A5))); }
FN(r_subdf3) { RET64(u64d(f64(A2, A3) - f64(A4, A5))); }
FN(r_muldf3) { RET64(u64d(f64(A2, A3) * f64(A4, A5))); }
FN(r_divdf3) { RET64(u64d(f64(A2, A3) / f64(A4, A5))); }
FN(r_fixdfsi) { RET((uint32_t)fix_s32(f64(A2, A3))); }
FN(r_fixunsdfsi) { RET(fix_u32(f64(A2, A3))); }
FN(r_truncdfsf2) { RET(u32f((float)f64(A2, A3))); }
FN(r_extendsfdf2) { RET64(u64d((double)f32(A2))); }
FN(r_floatsidf) { RET64(u64d((double)(int32_t)A2)); }
FN(r_floatunsidf) { RET64(u64d((double)A2)); }

// console
FN(r_ets_putc)          { write_char_native((int)A2); RETV(); }
FN(r_ets_write_char)    { write_char_native((int)A2); RETV(); }
FN(r_ets_uart_putc1)    { uart_putc1_native((int)A2); RETV(); }
FN(r_install_putc1)     { putc1 = A2; RETV(); }
FN(r_install_uart_printf) { putc1 = ADDR_ETS_UART_PUTC1; RETV(); }
FN(r_uart_buff_switch)  { printf_uart = A2 & 1; RETV(); }
FN(r_uart_init)         { RETV(); }
FN(r_uart_attach)       { RETV(); }
FN(r_uart_div_modify)   { hle_wr32((A2 & 1) ? 0x60000F14u : 0x60000014u, A3); RETV(); }
FN(r_uart_baud_detect)  { RET(0); }
FN(r_ets_printf) {
    uint32_t ra = C.a[0];
    argp_t ap = { 0 };
    for (int i = 0; i < 5; i++) ap.regs[i] = C.a[3 + i];     // fmt is a2: variadic args are a3..a7, then the stack
    ap.nregs = 5; ap.stk = C.a[1];
    int n = format(ADDR_ETS_WRITE_CHAR, A2, &ap);
    C.a[2] = (uint32_t)n; C.pc = ra;
}
FN(r_ets_vprintf) {
    uint32_t ra = C.a[0];
    argp_t ap = { .va = 1, .va_stk = A4, .va_reg = A5, .va_ndx = (int32_t)A6 };
    int n = format(A2, A3, &ap);
    C.a[2] = (uint32_t)n; C.pc = ra;
}

// scheduler
FN(r_ets_task) {
    uint32_t prio = A3;
    if (prio > 32) { RET(1); return; }
    task[prio].fn = A2; task[prio].queue = A4; task[prio].qlen = A5 ? A5 : 1;
    task[prio].head = task[prio].count = 0;
    if (prio == 1 && A5 == 1) E.loop_task = A2;     // Arduino core: ets_task(loop_task, 1, q, 1)
    RET(0);
}
FN(r_ets_post) {
    uint32_t prio = A2;
    if (prio > 32) { RET(1); return; }
    if (!task[prio].fn || task[prio].count >= task[prio].qlen) { RET(1); return; }
    uint32_t slot = (task[prio].head + task[prio].count) % task[prio].qlen;
    hle_wr32(task[prio].queue + slot * 8, A3);
    hle_wr32(task[prio].queue + slot * 8 + 4, A4);
    task[prio].count++;
    RET(0);
}
FN(r_ets_set_idle_cb) { idle_cb = A2; idle_arg = A3; RETV(); }
FN(r_ets_run) {
    run_sp = C.a[1];
    if (E.stage < STAGE_SDK_MAIN) ev_stage(STAGE_SDK_MAIN);
    C.pc = ETS_RUN_LOOP;
}
static void ets_run_loop(void)
{
    for (int p = 32; p >= 0; p--) {
        if (!task[p].count) continue;
        uint32_t ev = task[p].queue + task[p].head * 8;
        task[p].head = (task[p].head + 1) % task[p].qlen;
        task[p].count--;
        if (task[p].fn == E.loop_task) E.loop_dispatches++;
        idle_called = 0;
        C.a[1] = run_sp;
        C.a[2] = ev;
        C.a[0] = ETS_RUN_LOOP;
        C.ps = (C.ps & ~(PS_INTLEVEL | PS_EXCM)) | PS_UM;
        C.pc = task[p].fn;
        return;
    }
    if (idle_cb && !idle_called) {
        idle_called = 1;
        C.a[1] = run_sp;
        C.a[2] = idle_arg;
        C.a[0] = ETS_RUN_LOOP;
        C.ps = (C.ps & ~(PS_INTLEVEL | PS_EXCM)) | PS_UM;
        C.pc = idle_cb;
        return;
    }
    idle_called = 0;
    C.ps = (C.ps & ~(PS_INTLEVEL | PS_EXCM)) | PS_UM;
    C.waiting = 1;           // WAITI 0: sleep until an interrupt, then come back here
    C.pc = ETS_RUN_LOOP;
}

// interrupts and exceptions
FN(r_isr_attach) { uint32_t n = A2 & 15; isr[n].fn = A3; isr[n].arg = A4; RETV(); }
FN(r_isr_mask)   { C.intenable &= ~A2; irq_update(); RETV(); }
FN(r_isr_unmask) { C.intenable |= A2; irq_update(); RETV(); }
FN(r_set_exc_handler) {
    uint32_t cause = A2 & 63, old = c_handler[cause];
    c_handler[cause] = A3;
    hle_wr32(EXC_TABLE + cause * 4, A3 ? XTOS_C_WRAPPER : XTOS_UNHANDLED);
    RET(old);
}

// Exception frame used by the SDK vector and the Arduino core (struct __exception_frame):
// +0 epc, +4 ps, +8 sar, +12 unused, +16 a0, +20 a2 .. +72 a15, +76 cause. a1 = frame + 256.
static void frame_save(uint32_t frame, const uint32_t *regs)
{
    hle_wr32(frame + 0, C.epc[1]);
    hle_wr32(frame + 4, C.ps);
    hle_wr32(frame + 8, C.sar);
    hle_wr32(frame + 16, regs[0]);
    for (int i = 2; i < 16; i++) hle_wr32(frame + 20 + 4u * (uint32_t)(i - 2), regs[i]);
    hle_wr32(frame + 76, C.exccause);
}

static void frame_entry_regs(uint32_t frame, uint32_t *regs)
{
    memcpy(regs, C.a, 16 * 4);
    regs[1] = frame + 256;
    regs[2] = hle_rd32(frame + 20);
    regs[3] = hle_rd32(frame + 24);
    regs[4] = hle_rd32(frame + 28);
}

static void xtos_l1int(void)
{
    uint32_t frame = C.a[1], regs[16];
    frame_entry_regs(frame, regs);
    uint32_t epc1 = C.epc[1], ps = C.ps, sar = C.sar, exccause = C.exccause;
    frame_save(frame, regs);
    for (int guard = 0; guard < 32; guard++) {
        uint32_t pend = (C.intpend | E.irq_lines) & C.intenable & L1_INT_MASK;
        if (!pend) break;
        int n = __builtin_ctz(pend);
        if ((1u << n) & EDGE_INT_MASK) { C.intpend &= ~(1u << n); irq_update(); }
        if (!isr[n].fn) {
            C.intenable &= ~(1u << n); irq_update();
            ev_log("interrupt %d pending with no handler: masked", n);
            continue;
        }
        if (E.trace && n != INUM_FRC2) ev_log("irq %d -> %08x (intenable %08x)", n, isr[n].fn, C.intenable);
        C.a[1] = frame;
        C.ps = (ps & ~(PS_EXCM | PS_INTLEVEL)) | 1;
        uint32_t args[2] = { isr[n].arg, frame };
        guest_call(isr[n].fn, 2, args);
    }
    memcpy(C.a, regs, sizeof regs);
    C.sar = sar; C.epc[1] = epc1; C.exccause = exccause;
    C.ps = ps & ~PS_EXCM;      // RFE
    C.pc = epc1;
}

static void xtos_c_wrapper(void)
{
    uint32_t frame = C.a[1], regs[16];
    frame_entry_regs(frame, regs);
    uint32_t cause = C.exccause & 63, ps = C.ps;
    frame_save(frame, regs);
    C.a[1] = frame;
    C.ps = (ps & ~(PS_EXCM | PS_INTLEVEL)) | 1;
    uint32_t args[2] = { frame, cause };
    guest_call(c_handler[cause], 2, args);
    // the handler may have edited the frame (e.g. to skip the faulting instruction)
    C.epc[1] = hle_rd32(frame + 0);
    uint32_t nps = hle_rd32(frame + 4);
    C.sar = hle_rd32(frame + 8);
    C.a[0] = hle_rd32(frame + 16);
    for (int i = 2; i < 16; i++) C.a[i] = hle_rd32(frame + 20 + 4u * (uint32_t)(i - 2));
    C.a[1] = frame + 256;
    C.ps = nps & ~PS_EXCM;
    C.pc = C.epc[1];
}

static void xtos_unhandled(void)
{
    emu_halt("unhandled exception cause %u at epc1 %08x, excvaddr %08x", C.exccause, C.epc[1], C.excvaddr);
}

// flash
FN(r_spi_read) {
    uint32_t n = A4;
    if (A2 > E.flash_size || n > E.flash_size - A2) { RET(1); return; }
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf) { RET(1); return; }
    uint32_t status = spi_flash_read(A2, buf, n);
    if (!status) hle_write(A3, buf, n);
    free(buf); RET(status);
}
FN(r_spi_write) {
    uint32_t n = A4;
    if (A2 > E.flash_size || n > E.flash_size - A2) { RET(1); return; }
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf) { RET(1); return; }
    hle_read(A3, buf, n);
    uint32_t status = spi_flash_write(A2, buf, n);
    free(buf); RET(status);
}
FN(r_spi_erase_sector) { if (A2 > UINT32_MAX / 0x1000u) { RET(1); return; } RET(spi_flash_erase(A2 * 0x1000u, 0x1000)); }
FN(r_spi_erase_block)  { if (A2 > UINT32_MAX / 0x10000u) { RET(1); return; } RET(spi_flash_erase(A2 * 0x10000u, 0x10000)); }
FN(r_spi_read_status)  { hle_wr32(A3, 0); RET(0); }
FN(r_spi_ok)           { RET(0); }
FN(r_cache_read_enable) { E.flash_map_off = ((A2 & 1) ? 0x100000u : 0) + ((A3 & 1) ? 0x200000u : 0); cpu_fetch_invalidate(); RETV(); }
FN(r_cache_read_disable) { RETV(); }

// eboot brackets flash copying with this no-argument disable/restore pair.
FN(r_wdt_disable) {
    wdt_saved_ctrl = hle_rd32(0x60000900);
    hle_wr32(0x60000900, wdt_saved_ctrl & ~1u);
    RETV();
}
FN(r_wdt_enable) { hle_wr32(0x60000900, wdt_saved_ctrl); RETV(); }

// misc
FN(r_delay_us) {
    uint64_t us = A2;
    E.tick += us * 160;
    E.cycles += us * E.cpu_mhz;
    RETV();
}
FN(r_update_cpu_freq) { rom_cpu_freq = A2; RETV(); }
FN(r_get_cpu_freq)    { RET(rom_cpu_freq); }
FN(r_reset_reason)    { RET(E.reset_reason); }
FN(r_roundup2)        { uint32_t n = A3 ? A3 : 1; RET((A2 + n - 1) & ~(n - 1)); }
FN(r_nop)             { RETV(); }
FN(r_ret0)            { RET(0); }
FN(r_gpio_output_set) {
    hle_wr32(0x60000304, A2); hle_wr32(0x60000308, A3);
    hle_wr32(0x60000310, A4); hle_wr32(0x60000314, A5);
    RETV();
}
FN(r_gpio_input_get)  { RET(hle_rd32(0x60000318)); }
FN(r_phy_get_romfuncs) { RET(ROMFUNC_TABLE); }

// DMA descriptor chains (lldesc_t: word0 = size:12 | length:12 << 12 | ... | eof << 30 | owner << 31,
// word1 = buffer, word2 = next)
FN(r_lldesc_build_chain) {
    uint32_t desc = A2, desclen = A3, buf = A4, buflen = A5, blksz = A6, owner = A7 & 1;
    uint32_t headp = sarg(0), tailp = sarg(1);
    uint32_t nd = desclen / 12, nb = blksz ? (buflen + blksz - 1) / blksz : 0;
    if (nb > nd) nb = nd;
    uint32_t prev = 0, first = 0;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t d = desc + 12 * i, sz = blksz;
        if (i == nb - 1 && buflen % blksz) sz = buflen % blksz;
        hle_wr32(d, (sz & 0xFFF) | (owner << 31));
        hle_wr32(d + 4, buf + i * blksz);
        hle_wr32(d + 8, 0);
        if (prev) hle_wr32(prev + 8, d); else first = d;
        prev = d;
    }
    if (headp) hle_wr32(headp, first);
    if (tailp) hle_wr32(tailp, prev);
    RETV();
}
FN(r_lldesc_num2link) {
    uint32_t d = A2, n = A3 & 0xFFFF;
    for (uint32_t i = 1; i < n && d; i++) d = hle_rd32(d + 8);
    RET(d);
}

// crypto
FN(r_md5_init) { md5_ctx c; md5_init(&c); hle_write(A2, &c, sizeof c); RETV(); }
FN(r_md5_update) {
    md5_ctx c; hle_read(A2, &c, sizeof c);
    uint8_t *d = g_copy_in(A3, A4); md5_update(&c, d, A4); free(d);
    hle_write(A2, &c, sizeof c); RETV();
}
FN(r_md5_final) { md5_ctx c; uint8_t out[16]; hle_read(A3, &c, sizeof c); md5_final(out, &c); hle_write(A2, out, 16); hle_write(A3, &c, sizeof c); RETV(); }
FN(r_hmac_md5) {
    uint8_t *k = g_copy_in(A2, A3), *d = g_copy_in(A4, A5), mac[16];
    hmac_md5_buf(k, A3, d, A5, mac); hle_write(A6, mac, 16); free(k); free(d); RET(0);
}
FN(r_hmac_sha1) {
    uint8_t *k = g_copy_in(A2, A3), *d = g_copy_in(A4, A5), mac[20];
    const uint8_t *a[1] = { d }; size_t l[1] = { A5 };
    hmac_sha1_vec(k, A3, 1, a, l, mac); hle_write(A6, mac, 20); free(k); free(d); RET(0);
}
FN(r_hmac_sha1_vector) {
    uint32_t num = A4 > 6 ? 6 : A4;
    uint8_t *k = g_copy_in(A2, A3), *bufs[6], mac[20]; const uint8_t *a[6]; size_t l[6];
    for (uint32_t i = 0; i < num; i++) { l[i] = hle_rd32(A6 + 4 * i); bufs[i] = g_copy_in(hle_rd32(A5 + 4 * i), (uint32_t)l[i]); a[i] = bufs[i]; }
    hmac_sha1_vec(k, A3, num, a, l, mac); hle_write(A7, mac, 20);
    for (uint32_t i = 0; i < num; i++) free(bufs[i]);
    free(k); RET(0);
}
FN(r_pbkdf2_sha1) {
    char *pass = g_cstr(A2); uint8_t *ssid = g_copy_in(A3, A4), *out = malloc(A7 ? A7 : 1);
    pbkdf2_sha1_buf(pass, ssid, A4, (int)A5, out, A7); hle_write(A6, out, A7);
    free(pass); free(ssid); free(out); RET(0);
}
FN(r_sha1_prf) {
    uint32_t buf_len = sarg(0);
    uint8_t *k = g_copy_in(A2, A3), *d = g_copy_in(A5, A6), *out = malloc(buf_len ? buf_len : 1);
    char *label = g_cstr(A4);
    sha1_prf_buf(k, A3, label, d, A6, out, buf_len); hle_write(A7, out, buf_len);
    free(k); free(d); free(out); free(label); RET(0);
}
FN(r_rc4_skip) {
    uint8_t *k = g_copy_in(A2, A3), *d = g_copy_in(A5, A6);
    rc4_skip_buf(k, A3, A4, d, A6); hle_write(A5, d, A6); free(k); free(d); RET(0);
}

// RF analog registers behind the internal I2C bus, reached through phy_get_romfuncs() slots 36..39:
// rom_i2c_readReg(block, host, reg), rom_i2c_readReg_Mask(block, host, reg, msb, lsb),
// rom_i2c_writeReg(block, host, reg, data), rom_i2c_writeReg_Mask(block, host, reg, msb, lsb, data).
// Plain storage, except block 98 (0x62) reg 7 bit 7, which libphy's wait_rfpll_cal_end() polls for "PLL calibrated".
static uint8_t i2c_reg[256][8][256];
static void rf_i2c(uint32_t k)
{
    uint32_t blk = A2 & 0xFF, host = A3 & 7, reg = A4 & 0xFF;
    uint8_t *r = &i2c_reg[blk][host][reg];
    uint8_t v = *r;
    if (blk == 98 && reg == 7) v |= 0x80;
    if (k == 36) { RET(v); return; }
    uint32_t msb = A5 & 7, lsb = A6 & 7, mask = (msb >= lsb) ? ((1u << (msb - lsb + 1)) - 1) : 0;
    if (k == 37) { RET((v >> lsb) & mask); return; }
    if (k == 38) { *r = (uint8_t)A5; RETV(); return; }
    *r = (uint8_t)((*r & ~(mask << lsb)) | ((A7 & mask) << lsb));
    RETV();
}

typedef struct { uint32_t addr; const char *name; void (*fn)(void); } rom_entry_t;
static const rom_entry_t rom_table[] = {
    { 0x40000080, "_ResetVector", r_reset_vector },
    { 0x40000454, "_xtos_set_exception_handler", r_set_exc_handler },
    { 0x40000650, "__muldi3", r_muldi3 },
    { 0x40000dc0, "ets_set_idle_cb", r_ets_set_idle_cb },
    { 0x40000dd0, "ets_task", r_ets_task },
    { 0x40000e04, "ets_run", r_ets_run },
    { 0x40000e24, "ets_post", r_ets_post },
    { 0x40000f88, "ets_isr_attach", r_isr_attach },
    { 0x40000f98, "ets_isr_mask", r_isr_mask },
    { 0x40000fa8, "ets_isr_unmask", r_isr_unmask },
    { 0x400018a4, "ets_memset", r_memset },
    { 0x400018b4, "ets_memcpy", r_memcpy },
    { 0x400018c4, "ets_memmove", r_memmove },
    { 0x400018d4, "ets_memcmp", r_memcmp },
    { 0x40001da0, "ets_write_char", r_ets_write_char },
    { 0x40001dcc, "ets_uart_putc1", r_ets_uart_putc1 },
    { 0x40001f00, "ets_vprintf", r_ets_vprintf },
    { 0x4000242c, "ets_install_putc1", r_install_putc1 },
    { 0x40002438, "ets_install_uart_printf", r_install_uart_printf },
    { 0x400024cc, "ets_printf", r_ets_printf },
    { 0x400025e0, "rtc_get_reset_reason", r_reset_reason },
    { 0x40002a88, "ets_strcpy", r_strcpy },
    { 0x40002a98, "ets_strncpy", r_strncpy },
    { 0x40002ab8, "ets_strncmp", r_strncmp },
    { 0x40002ac8, "ets_strlen", r_strlen },
    { 0x40002ae8, "ets_bzero", r_bzero },
    { 0x40002be8, "ets_putc", r_ets_putc },
    { 0x40002ecc, "ets_delay_us", r_delay_us },
    { 0x40002f04, "ets_update_cpu_frequency", r_update_cpu_freq },
    { 0x40002f0c, "ets_get_cpu_frequency", r_get_cpu_freq },
    { 0x40002fa0, "ets_wdt_enable", r_wdt_enable },
    { 0x400030f0, "ets_wdt_disable", r_wdt_disable },
    { 0x400031b4, "roundup2", r_roundup2 },
    { 0x40003368, "UartDwnLdProc", r_ret0 },
    { 0x4000383c, "uartAttach", r_uart_attach },
    { 0x400038a4, "uart_buff_switch", r_uart_buff_switch },
    { 0x40003924, "uart_baudrate_detect", r_uart_baud_detect },
    { 0x400039d8, "uart_div_modify", r_uart_div_modify },
    { 0x40003a14, "Uart_Init", r_uart_init },
    { 0x400043c8, "SPI_read_status", r_spi_read_status },
    { 0x40004400, "SPI_write_status", r_spi_ok },
    { 0x4000443c, "SPI_write_enable", r_spi_ok },
    { 0x4000448c, "Wait_SPI_Idle", r_spi_ok },
    { 0x400044c0, "Enable_QMode", r_spi_ok },
    { 0x40004678, "Cache_Read_Enable", r_cache_read_enable },
    { 0x400047f0, "Cache_Read_Disable", r_cache_read_disable },
    { 0x400049b4, "SPIEraseBlock", r_spi_erase_block },
    { 0x40004a00, "SPIEraseSector", r_spi_erase_sector },
    { 0x40004a4c, "SPIWrite", r_spi_write },
    { 0x40004b1c, "SPIRead", r_spi_read },
    { 0x40004cd0, "gpio_output_set", r_gpio_output_set },
    { 0x40004cf0, "gpio_input_get", r_gpio_input_get },
    { 0x40004e90, "gpio_pin_wakeup_enable", r_nop },
    { 0x40004ed4, "gpio_pin_wakeup_disable", r_nop },
    { 0x40004f40, "lldesc_build_chain", r_lldesc_build_chain },
    { 0x40005050, "lldesc_num2link", r_lldesc_num2link },
    { 0x400060d0, "rom_chip_v5_disable_cca", r_nop },
    { 0x400060ec, "rom_chip_v5_enable_cca", r_nop },
    { 0x40006b08, "phy_get_romfuncs", r_phy_get_romfuncs },
    { 0x40009818, "MD5Init", r_md5_init },
    { 0x40009834, "MD5Update", r_md5_update },
    { 0x40009900, "MD5Final", r_md5_final },
    { 0x4000a2cc, "hmac_md5", r_hmac_md5 },
    { 0x4000b840, "pbkdf2_sha1", r_pbkdf2_sha1 },
    { 0x4000b8b4, "hmac_sha1_vector", r_hmac_sha1_vector },
    { 0x4000ba28, "hmac_sha1", r_hmac_sha1 },
    { 0x4000ba48, "sha1_prf", r_sha1_prf },
    { 0x4000bdc8, "strcmp", r_strcmp },
    { 0x4000bf4c, "strlen", r_strlen },
    { 0x4000bfa8, "strncmp", r_strncmp },
    { 0x4000c180, "__addsf3", r_addsf3 },
    { 0x4000c268, "__subsf3", r_subsf3 },
    { 0x4000c3dc, "__mulsf3", r_mulsf3 },
    { 0x4000c4c4, "__fixunssfsi", r_fixunssfsi },
    { 0x4000c538, "__adddf3", r_adddf3 },
    { 0x4000c688, "__subdf3", r_subdf3 },
    { 0x4000c8f0, "__muldf3", r_muldf3 },
    { 0x4000cb94, "__divdf3", r_divdf3 },
    { 0x4000ccb8, "__fixdfsi", r_fixdfsi },
    { 0x4000cd00, "__fixunsdfsi", r_fixunsdfsi },
    { 0x4000cd5c, "__truncdfsf2", r_truncdfsf2 },
    { 0x4000cdfc, "__extendsfdf2", r_extendsfdf2 },
    { 0x4000ce60, "__divdi3", r_divdi3 },
    { 0x4000d310, "__udivdi3", r_udivdi3 },
    { 0x4000d770, "__umoddi3", r_umoddi3 },
    { 0x4000dc88, "__divsi3", r_divsi3 },
    { 0x4000dcf0, "__umulsidi3", r_umulsidi3 },
    { 0x4000dd68, "rc4_skip", r_rc4_skip },
    { 0x4000dea8, "memcmp", r_memcmp },
    { 0x4000df48, "memcpy", r_memcpy },
    { 0x4000e04c, "memmove", r_memmove },
    { 0x4000e190, "memset", r_memset },
    { 0x4000e1e0, "strstr", r_strstr },
    { 0x4000e21c, "__udivsi3", r_udivsi3 },
    { 0x4000e268, "__umodsi3", r_umodsi3 },
    { 0x4000e2a4, "__floatunsisf", r_floatunsisf },
    { 0x4000e2ac, "__floatsisf", r_floatsisf },
    { 0x4000e2e8, "__floatunsidf", r_floatunsidf },
    { 0x4000e2f0, "__floatsidf", r_floatsidf },
    { XTOS_L1INT, "_xtos_l1int_handler", xtos_l1int },
    { XTOS_UNHANDLED, "_xtos_unhandled_exception", xtos_unhandled },
    { XTOS_C_WRAPPER, "_xtos_c_wrapper_handler", xtos_c_wrapper },
    { IMAGE_RETURNED, "(image returned)", r_image_returned },
    { ETS_RUN_LOOP, "ets_run (loop)", ets_run_loop },
};
#define ROM_N (sizeof rom_table / sizeof rom_table[0])
static int16_t rom_index[ROM_SIZE];

const char *rom_name(uint32_t pc)
{
    if (pc - ROM_BASE >= ROM_SIZE) return NULL;
    int i = rom_index[pc - ROM_BASE];
    return i >= 0 ? rom_table[i].name : NULL;
}

void rom_hle(uint32_t pc)
{
    int i = rom_index[pc - ROM_BASE];
    if (i >= 0) { rom_table[i].fn(); return; }
    if (pc >= ROMFUNC_STUB_BASE && pc < ROMFUNC_STUB_BASE + 4 * ROMFUNC_COUNT) {
        uint32_t k = (pc - ROMFUNC_STUB_BASE) / 4;
        if (k >= 36 && k <= 39) { rf_i2c(k); return; }
        if (!romfunc_seen[k]) { romfunc_seen[k] = 1; ev_log("phy romfunc[%u] called (a2=%08x a3=%08x), returns 0", k, A2, A3); }
        RET(0);
        return;
    }
    if (pc >= GUEST_RET_BASE && pc < GUEST_RET_BASE + 0x100)
        emu_halt("firmware returned to ROM call level %u out of order (pc %08x)", (pc - GUEST_RET_BASE) / 4, C.pc);
    if (E.strict) emu_halt("unimplemented ROM entry %08x called from %08x", pc, C.a[0]);
    ev_log("unimplemented ROM entry %08x called from %08x: returning 0", pc, C.a[0]);
    RET(0);
}

// ---------------------------------------------------------------- AES T-table (ROM data Te0) and rcon
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t p = 0; while (b) { if (b & 1) p ^= a; a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); b >>= 1; } return p; }
// FIPS 197, sections 4 and 5.1.1: field inversion followed by the affine map.
// https://csrc.nist.gov/pubs/fips/197/final
// Compute x^254 by square-and-multiply; zero maps to zero before the affine map.
static uint8_t aes_inverse(uint8_t value)
{
    if (!value) return 0;
    uint8_t product = 1;
    for (unsigned exponent = 254; exponent; exponent >>= 1) {
        if (exponent & 1) product = gmul(product, value);
        value = gmul(value, value);
    }
    return product;
}

static uint8_t aes_sub_byte(uint8_t value)
{
    uint8_t inverse = aes_inverse(value);
    uint8_t result = 0x63;
    for (unsigned bit = 0; bit < 8; bit++) {
        unsigned parity = (inverse >> bit) ^ (inverse >> ((bit + 4) & 7))
            ^ (inverse >> ((bit + 5) & 7)) ^ (inverse >> ((bit + 6) & 7))
            ^ (inverse >> ((bit + 7) & 7));
        result ^= (uint8_t)((parity & 1) << bit);
    }
    return result;
}

static void build_aes_tables(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t s = aes_sub_byte((uint8_t)i);
        uint32_t te0 = ((uint32_t)gmul(s, 2) << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | gmul(s, 3);
        hle_wr32(TE0_TABLE + 4u * (uint32_t)i, te0);
    }
    uint32_t r = 1;
    for (int i = 0; i < 10; i++) { hle_wr32(RCONS_TABLE + 4u * (uint32_t)i, r << 24); r = gmul((uint8_t)r, 2); }
}

// ---------------------------------------------------------------- boot loader
static void rom_data_init(void)
{
    for (int i = 0; i < 64; i++) hle_wr32(EXC_TABLE + 4u * (uint32_t)i, i == EXC_LEVEL1_INT ? XTOS_L1INT : XTOS_UNHANDLED);
    hle_wr32(FLASHCHIP, FLASHCHIP_DATA);
    hle_wr32(FLASHCHIP_DATA + 0, E.flash_id);       // deviceId
    hle_wr32(FLASHCHIP_DATA + 4, E.flash_size);     // chip_size
    hle_wr32(FLASHCHIP_DATA + 8, 0x10000);          // block_size
    hle_wr32(FLASHCHIP_DATA + 12, 0x1000);          // sector_size
    hle_wr32(FLASHCHIP_DATA + 16, 0x100);           // page_size
    hle_wr32(FLASHCHIP_DATA + 20, 0xFFFF);          // status_mask
    for (int i = 0; i < ROMFUNC_COUNT; i++) hle_wr32(ROMFUNC_TABLE + 4u * (uint32_t)i, ROMFUNC_STUB_BASE + 4u * (uint32_t)i);
    hle_wr32(USER_START_FPTR, 0);
    build_aes_tables();
}

void rom_init(void)
{
    for (uint32_t i = 0; i < ROM_SIZE; i++) rom_index[i] = -1;
    for (unsigned i = 0; i < ROM_N; i++) rom_index[rom_table[i].addr - ROM_BASE] = (int16_t)i;
}

void rom_boot(void)
{
    memset(isr, 0, sizeof isr);
    memset(c_handler, 0, sizeof c_handler);
    memset(task, 0, sizeof task);
    idle_cb = idle_arg = 0; idle_called = 0;
    putc1 = ADDR_ETS_UART_PUTC1; printf_uart = 0; rom_cpu_freq = 52;
    memset(romfunc_seen, 0, sizeof romfunc_seen);
    E.loop_task = 0;
    E.loop_dispatches = 0;
    E.setup_calls = E.loop_calls = 0;
    cpu_reset();
    cpu_set_freq(80);
    ev_stage(STAGE_ROM);
    rom_data_init();

    rom_printf_native("\r\n ets Jan  8 2013,rst cause:%d, boot mode:(3,6)\r\n\r\n", E.reset_reason);
    if (E.flash_size < 16 || E.flash[0] != 0xE9) {
        rom_printf_native("ets_main.c \r\n");
        emu_halt("no bootable image at flash 0x0 (first byte %02x, expected E9)", E.flash_size ? E.flash[0] : 0);
    }
    uint32_t nseg = E.flash[1], entry, off = 8;
    memcpy(&entry, E.flash + 4, 4);
    uint8_t csum = 0xEF;
    for (uint32_t s = 0; s < nseg; s++) {
        uint32_t addr, len;
        if (off > E.flash_size || 8 > E.flash_size - off) emu_halt("image header runs past the end of flash");
        memcpy(&addr, E.flash + off, 4); memcpy(&len, E.flash + off + 4, 4);
        off += 8;
        if (len > E.flash_size - off) emu_halt("image segment runs past the end of flash");
        rom_printf_native("load 0x%08x, len %u, room 16 \r\n", addr, len);
        hle_write(addr, E.flash + off, len);
        for (uint32_t i = 0; i < len; i++) csum ^= E.flash[off + i];
        off += len;
        rom_printf_native("tail %u\r\nchksum 0x%02x\r\n", len & 15, csum);
    }
    uint32_t pad_end = (off + 16) & ~15u;
    uint8_t stored = pad_end - 1 < E.flash_size ? E.flash[pad_end - 1] : 0;
    rom_printf_native("csum 0x%02x\r\n", stored);
    if (stored != csum) rom_printf_native("csum err\r\n");

    C.a[1] = 0x3FFFFFF0u;
    C.a[0] = IMAGE_RETURNED;
    C.ps = PS_UM;
    C.vecbase = ROM_BASE;
    C.pc = entry;
    ev_stage(entry >= 0x4010F000u ? STAGE_EBOOT : STAGE_APP);
}

// --watch ADDR=NAME: log calls of firmware functions (entry PC) with their first arguments. Bitmap over IRAM and the
// 1 MiB flash window so the per-instruction test is one load and a mask.
static uint8_t watch_bits[(IRAM_SIZE + FLASHMAP_SIZE) / 8];
static struct { uint32_t pc; char name[96]; uint32_t hits; } watches[64];
static int nwatch;

static int watch_index(uint32_t pc)
{
    if (pc - IRAM_BASE < IRAM_SIZE) return (int)(pc - IRAM_BASE);
    if (pc - FLASHMAP_BASE < FLASHMAP_SIZE) return (int)(IRAM_SIZE + pc - FLASHMAP_BASE);
    return -1;
}

int watch_test(uint32_t pc)
{
    int i = watch_index(pc);
    return i >= 0 && (watch_bits[i >> 3] >> (i & 7) & 1);
}

void watch_add(uint32_t pc, const char *name)
{
    int i = watch_index(pc);
    if (i < 0 || nwatch >= 64) return;
    watch_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    watches[nwatch].pc = pc;
    snprintf(watches[nwatch].name, sizeof watches[nwatch].name, "%s", name);
    nwatch++;
    E.watch_any = 1;
}

void watch_hit(uint32_t pc)
{
    for (int i = 0; i < nwatch; i++) {
        if (watches[i].pc != pc) continue;
        if (++watches[i].hits <= 200 || (watches[i].hits & 1023) == 0)
            ev_printf("{\"t\":\"call\",\"fn\":\"%s\",\"n\":%u,\"a2\":\"%08x\",\"a3\":\"%08x\",\"a4\":\"%08x\",\"ra\":\"%08x\",\"vt_us\":%llu}",
                      watches[i].name, watches[i].hits, C.a[2], C.a[3], C.a[4], C.a[0], (unsigned long long)now_us());
    }
    if (pc == E.sym_setup) { E.setup_total++; if (!E.setup_calls++) ev_stage(STAGE_SETUP); }
    if (pc == E.sym_loop) { E.loop_total++; if (!E.loop_calls++) ev_stage(STAGE_LOOP); }
}
