// esp8266emu: host side. Loads a flash image, boots it through the open ROM, streams what the chip does as
// JSON lines on stdout and takes commands on stdin (see README "Event protocol").
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

static uint64_t total_us(void);

extern int pins_dirty;
extern uint32_t pc_ring[64];
extern unsigned pc_ring_i;
void soc_pin_state(int pin, int *level, char *mode);
uint32_t soc_uart_baud(int u);
int soc_uart_on_pad(int u);

enum { RJ_POWER = 1, RJ_EXT = 2, RJ_SOFT = 3, RJ_WDT = 4, RJ_HALT = 9, RJ_QUIT = 10 };

static const char *flash_path, *flash_out, *uart_log_path, *report_path, *profile_path;
#define PROF_N 65536
static uint32_t prof_pc[PROF_N], prof_cnt[PROF_N];

static void prof_sample(uint32_t pc)
{
    uint32_t h = (pc * 2654435761u) & (PROF_N - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (PROF_N - 1)) {
        if (prof_pc[h] == pc) { prof_cnt[h]++; return; }
        if (!prof_pc[h]) { prof_pc[h] = pc; prof_cnt[h] = 1; return; }
    }
}

static void prof_dump(void)
{
    if (!profile_path) return;
    FILE *f = fopen(profile_path, "w");
    if (!f) return;
    for (int i = 0; i < PROF_N; i++) if (prof_pc[i]) fprintf(f, "%08x %u\n", prof_pc[i], prof_cnt[i]);
    fclose(f);
}

static FILE *uart_log;
static double speed = 1.0;          // 1.0 = real time, 0 = as fast as possible
static double max_vsec = 0;         // stop after this much virtual time (0 = never)
static uint64_t until_loops = 0;    // stop after loop() ran this many times (needs --sym-loop)
static int quiet;
static char halt_msg[512];
static uint64_t wall_ref_ns, tick_ref;
static uint64_t last_out_ns, last_stat_ns, stat_insn_ref, stat_ns_ref;
static uint8_t ubuf[2][8192];
static int ulen[2], uwire[2];
static uint64_t uart_total[2], uart_wire[2];
static char lastpins[2][18];
static char edges[256 * 40];
static int edges_len, edges_n, edges_dropped;

// Every pin level/mode change, timestamped when it happens (not sampled per slice). Flushed with the UART.
void pin_edge(int pin, int level, char mode)
{
    if (edges_n >= 256 || edges_len > (int)sizeof edges - 48) { edges_dropped++; return; }
    edges_len += snprintf(edges + edges_len, sizeof edges - (size_t)edges_len, "%s[%llu,%d,%d,\"%c\"]",
                          edges_n ? "," : "", (unsigned long long)total_us(), pin, level, mode);
    edges_n++;
}

static void flush_edges(void)
{
    if (!edges_n && !edges_dropped) return;
    ev_printf("{\"t\":\"edges\",\"e\":[%s],\"dropped\":%d}", edges, edges_dropped);
    edges_len = edges_n = edges_dropped = 0;
    edges[0] = 0;
}

static uint64_t wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void out_line(const char *s)
{
    if (quiet) return;
    if (fputs(s, stdout) < 0 || fputc('\n', stdout) < 0) exit(0);
}

void ev_printf(const char *fmt, ...)
{
    char b[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    out_line(b);
}

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        else dst[o++] = (char)c;
    }
    dst[o] = 0;
}

void ev_log(const char *fmt, ...)
{
    char b[1024], e[2100];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    json_escape(e, sizeof e, b);
    ev_printf("{\"t\":\"log\",\"m\":\"%s\",\"vt_us\":%llu}", e, (unsigned long long)now_us());
    if (quiet) fprintf(stderr, "log: %s\n", b);
}

static const char *stage_name(int s)
{
    static const char *n[] = { "off", "rom", "eboot", "app", "sdk", "setup", "loop", "halted" };
    return s >= 0 && s <= STAGE_HALTED ? n[s] : "?";
}

void ev_stage(int stage)
{
    E.stage = stage;
    ev_printf("{\"t\":\"stage\",\"s\":\"%s\",\"boot\":%llu,\"reason\":%d,\"vt_us\":%llu,\"t_us\":%llu,\"insn\":%llu}",
              stage_name(stage), (unsigned long long)E.boot_count, E.reset_reason,
              (unsigned long long)now_us(), (unsigned long long)total_us(), (unsigned long long)(E.cycles - E.boot_cycles));
    if (quiet) fprintf(stderr, "stage %s at %llu us\n", stage_name(stage), (unsigned long long)now_us());
}

void emu_halt(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(halt_msg, sizeof halt_msg, fmt, ap); va_end(ap);
    longjmp(E.reset_jmp, RJ_HALT);
}

static void flush_one(int u)
{
    static char hex[sizeof ubuf[0] * 2 + 64];
    if (!ulen[u]) return;
    int o = sprintf(hex, "{\"t\":\"uart\",\"u\":%d,\"w\":%d,\"d\":\"", u, uwire[u]);
    for (int i = 0; i < ulen[u]; i++) o += sprintf(hex + o, "%02x", ubuf[u][i]);
    strcpy(hex + o, "\"}");
    out_line(hex);
    ulen[u] = 0;
}

void uart_tx_byte(int u, uint8_t b)
{
    u &= 1;
    int w = soc_uart_on_pad(u);
    if (ulen[u] && w != uwire[u]) flush_one(u);
    uwire[u] = w;
    if (uart_log) fputc(b, uart_log);
    uart_total[u]++;
    if (w) uart_wire[u]++;
    ubuf[u][ulen[u]++] = b;
    if (ulen[u] == (int)sizeof ubuf[u]) flush_one(u);
}

static void flush_uart(void)
{
    flush_edges();
    flush_one(0);
    flush_one(1);
    if (uart_log) fflush(uart_log);
}

static void emit_pins(int force)
{
    char lv[18], md[18];
    for (int p = 0; p <= 16; p++) { int l; char m; soc_pin_state(p, &l, &m); lv[p] = (char)('0' + l); md[p] = m; }
    lv[17] = md[17] = 0;
    if (!force && !strcmp(lv, lastpins[0]) && !strcmp(md, lastpins[1])) return;
    memcpy(lastpins[0], lv, 18); memcpy(lastpins[1], md, 18);
    ev_printf("{\"t\":\"pins\",\"lv\":\"%s\",\"md\":\"%s\",\"vt_us\":%llu,\"t_us\":%llu}", lv, md,
              (unsigned long long)now_us(), (unsigned long long)total_us());
}

static void emit_stat(uint64_t nowns)
{
    double dt = (double)(nowns - stat_ns_ref) / 1e9;
    double mips = dt > 0 ? (double)(E.cycles - stat_insn_ref) / dt / 1e6 : 0;
    static uint64_t stat_tick_ref;
    double ratio = dt > 0 && stat_tick_ref ? (double)(E.tick - stat_tick_ref) / 160e6 / dt : 0;  // emulated s per wall s
    stat_ns_ref = nowns; stat_insn_ref = E.cycles; stat_tick_ref = E.tick;
    ev_printf("{\"t\":\"stat\",\"vt_us\":%llu,\"t_us\":%llu,\"insn\":%llu,\"mips\":%.1f,\"stage\":\"%s\",\"boot\":%llu,"
              "\"loop_task\":%u,\"setup_calls\":%llu,\"loop_calls\":%llu,\"baud0\":%u,\"baud1\":%u,\"cpu_mhz\":%u,"
              "\"uart0_bytes\":%llu,\"speed\":%g,\"ratio\":%.3f}",
              (unsigned long long)now_us(), (unsigned long long)total_us(), (unsigned long long)(E.cycles - E.boot_cycles), mips, stage_name(E.stage),
              (unsigned long long)E.boot_count, E.loop_dispatches, (unsigned long long)E.setup_calls,
              (unsigned long long)E.loop_calls, soc_uart_baud(0), soc_uart_baud(1), E.cpu_mhz,
              (unsigned long long)uart_total[0], speed, ratio);
}

static void save_flash(void)
{
    if (!flash_out || !E.flash_dirty) return;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", flash_out);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(E.flash, 1, E.flash_size, f);
    fclose(f);
    rename(tmp, flash_out);
    E.flash_dirty = false;
}

static void write_report(const char *why);
static void handle_command(char *line);

static void handle_command(char *line)
{
    char cmd[32] = { 0 };
    if (sscanf(line, "%31s", cmd) != 1) return;
    if (!strcmp(cmd, "in")) {
        int pin, lv;
        if (sscanf(line, "%*s %d %d", &pin, &lv) == 2) soc_set_input(pin, lv);
    } else if (!strcmp(cmd, "rx")) {
        int u; char hex[2048];
        if (sscanf(line, "%*s %d %2047s", &u, hex) == 2) {
            uint8_t d[1024]; int n = 0;
            for (char *p = hex; p[0] && p[1] && n < 1024; p += 2) { unsigned v; sscanf(p, "%2x", &v); d[n++] = (uint8_t)v; }
            soc_uart_rx(u, d, n);
        }
    } else if (!strcmp(cmd, "speed")) {
        double s; if (sscanf(line, "%*s %lf", &s) == 1) { speed = s; wall_ref_ns = wall_ns(); tick_ref = E.tick; }
    } else if (!strcmp(cmd, "pins")) {
        emit_pins(1);
    } else if (!strcmp(cmd, "reset")) {
        longjmp(E.reset_jmp, RJ_EXT);
    } else if (!strcmp(cmd, "power")) {
        longjmp(E.reset_jmp, RJ_POWER);
    } else if (!strcmp(cmd, "quit")) {
        longjmp(E.reset_jmp, RJ_QUIT);
    } else if (!strcmp(cmd, "trace")) {
        E.trace = !E.trace;
    } else if (!strcmp(cmd, "flashw")) {
        // flashw <addr> <file>: what Updater leaves in flash (sectors erased, then programmed), for OTA rehearsals
        unsigned long addr; char path[1024];
        if (sscanf(line, "%*s %lx %1023s", &addr, path) == 2) {
            FILE *f = fopen(path, "rb");
            if (!f) { ev_log("flashw: cannot open %s", path); return; }
            static uint8_t buf[1 << 24];
            size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            spi_flash_erase((uint32_t)addr & ~0xFFFu, (uint32_t)((n + (addr & 0xFFF) + 0xFFF) & ~0xFFFu));
            spi_flash_write((uint32_t)addr, buf, (uint32_t)n);
            ev_log("flashw: %zu bytes at 0x%06lx", n, addr);
        }
    } else if (!strcmp(cmd, "rtcw")) {
        // rtcw <offset into RTC memory 0x60001000> <hex bytes>
        unsigned off; char hex[1024];
        if (sscanf(line, "%*s %x %1023s", &off, hex) == 2)
            for (char *p = hex; p[0] && p[1] && off < RTCMEM_SIZE; p += 2, off++) { unsigned v; sscanf(p, "%2x", &v); mem_rtc[off] = (uint8_t)v; }
    } else if (!strcmp(cmd, "softreset")) {
        longjmp(E.reset_jmp, RJ_SOFT);     // what ESP.restart() does: jump to the ROM reset vector, RTC memory kept
    }
}

static void poll_input(int timeout_ms)
{
    static char buf[8192];
    static int blen;
    struct pollfd pf = { .fd = 0, .events = POLLIN };
    while (poll(&pf, 1, timeout_ms) > 0) {
        timeout_ms = 0;
        if (pf.revents & (POLLHUP | POLLERR) && !(pf.revents & POLLIN)) longjmp(E.reset_jmp, RJ_QUIT);
        ssize_t n = read(0, buf + blen, sizeof buf - 1 - (size_t)blen);
        if (n <= 0) longjmp(E.reset_jmp, RJ_QUIT);
        blen += (int)n;
        buf[blen] = 0;
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            handle_command(start);
            start = nl + 1;
        }
        blen = (int)strlen(start);
        memmove(buf, start, (size_t)blen);
        if (blen >= (int)sizeof buf - 1) blen = 0;
    }
}

static int stdin_live = 1;

// --script FILE: lines "<virtual ms> <command>" run when emulated time reaches them (deterministic tests)
typedef struct { uint64_t at_us; char cmd[2048]; } script_ev_t;
static script_ev_t *script;
static int script_n, script_i;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open script %s\n", path); exit(2); }
    static char line[2100];
    while (fgets(line, sizeof line, f)) {
        double ms; int off = 0;
        if (line[0] == '#' || sscanf(line, "%lf %n", &ms, &off) < 1) continue;
        script = realloc(script, sizeof *script * (size_t)(script_n + 1));
        script[script_n].at_us = (uint64_t)(ms * 1000);
        snprintf(script[script_n].cmd, sizeof script[script_n].cmd, "%s", line + off);
        script[script_n].cmd[strcspn(script[script_n].cmd, "\r\n")] = 0;
        script_n++;
    }
    fclose(f);
}

static uint64_t total_us(void) { return (E.tick - E.tick_first) / 160; }

// --calltrace FROM_US:TO_US -> stderr "T <us> <caller> <target>"; --mmiotrace LO:HI -> stderr "R|W <us> <pc> <addr> <val>"
void ctrace_log(uint32_t from, uint32_t to)
{
    uint64_t t = total_us();
    if (t >= E.ctrace_from && t <= E.ctrace_to) fprintf(stderr, "T %llu %08x %08x\n", (unsigned long long)t, from, to);
}

void mtrace_log(int write, uint32_t addr, uint32_t val)
{
    uint64_t t = total_us();
    if (E.ctrace && (t < E.ctrace_from || t > E.ctrace_to)) return;
    fprintf(stderr, "%c %llu %08x %08x %08x\n", write ? 'W' : 'R', (unsigned long long)t, C.pc, addr, val);
}

static void run_script(void)
{
    while (script_i < script_n && total_us() >= script[script_i].at_us) {
        char *c = script[script_i++].cmd;
        ev_log("script: %s", c);
        handle_command(c);
    }
}

void host_service(void)
{
    uint64_t nowns = wall_ns();
    if (profile_path) prof_sample(C.waiting ? 1 : C.pc);
    if (script_i < script_n) run_script();
    if (max_vsec > 0 && total_us() >= (uint64_t)(max_vsec * 1e6)) longjmp(E.reset_jmp, RJ_QUIT);
    if (until_loops && E.loop_total >= until_loops) longjmp(E.reset_jmp, RJ_QUIT);
    if (pins_dirty) { pins_dirty = 0; emit_pins(0); }
    if (edges_n >= 200) flush_edges();
    if (nowns - last_out_ns > 10000000ull) {
        last_out_ns = nowns;
        flush_uart();
        if (!quiet) fflush(stdout);
        if (stdin_live) poll_input(0);
    }
    if (nowns - last_stat_ns > 500000000ull) {
        last_stat_ns = nowns;
        emit_stat(nowns);
        save_flash();
    }
    if (speed > 0) {
        uint64_t virt_ns = (E.tick - tick_ref) * 25 / 4;              // tick = 6.25 ns
        uint64_t wall_el = (uint64_t)((double)(nowns - wall_ref_ns) * speed);
        if (virt_ns > wall_el + 2000000ull) {
            uint64_t ahead = (uint64_t)((double)(virt_ns - wall_el) / speed);
            if (ahead > 50000000ull) ahead = 50000000ull;
            struct timespec ts = { 0, (long)ahead };
            nanosleep(&ts, NULL);
        } else if (wall_el > virt_ns + 500000000ull) {
            wall_ref_ns = nowns; tick_ref = E.tick;                      // too far behind: do not try to catch up
        }
    }
}

static void halted_loop(void)
{
    ev_stage(STAGE_HALTED);
    char e[1100]; json_escape(e, sizeof e, halt_msg);
    ev_printf("{\"t\":\"halt\",\"m\":\"%s\",\"pc\":\"%08x\",\"vt_us\":%llu}", e, C.pc, (unsigned long long)now_us());
    if (quiet) fprintf(stderr, "HALT: %s (pc %08x)\n", halt_msg, C.pc);
    char ring[64 * 10 + 1]; int o = 0;
    for (int i = 0; i < 16; i++) o += sprintf(ring + o, "%08x ", pc_ring[(pc_ring_i - 16 + (unsigned)i) & 63]);
    ev_log("last PCs: %s", ring);
    if (quiet) fprintf(stderr, "last PCs: %s\n", ring);
    flush_uart();
    fflush(stdout);
    if (!stdin_live || max_vsec > 0) longjmp(E.reset_jmp, RJ_QUIT);
    for (;;) { poll_input(200); flush_uart(); fflush(stdout); }
}

static int flash_size_from_header(uint8_t b3)
{
    switch (b3 >> 4) {
    case 0: return 0x80000; case 1: return 0x40000; case 2: return 0x100000; case 3: return 0x200000;
    case 4: return 0x400000; case 5: return 0x200000; case 6: return 0x400000; case 8: return 0x800000;
    case 9: return 0x1000000; default: return 0x400000;
    }
}

static const struct chip_profile *selected_chip;

static uint32_t flash_option(const char *text, const char *option)
{
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || end == text || *end || text[0] == '-' || value > UINT32_MAX) {
        fprintf(stderr, "%s requires an unsigned 32-bit number\n", option);
        exit(2);
    }
    return (uint32_t)value;
}

static void load_flash(uint32_t forced_size, uint32_t forced_id)
{
    FILE *f = fopen(flash_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", flash_path, strerror(errno)); exit(2); }
    if (fseek(f, 0, SEEK_END)) { fprintf(stderr, "cannot seek flash image\n"); exit(2); }
    long n = ftell(f);
    if (n < 4) { fprintf(stderr, "image too small\n"); exit(2); }
    fseek(f, 0, SEEK_SET);
    uint8_t hdr[4] = { 0 };
    if (fread(hdr, 1, 4, f) != 4) { fprintf(stderr, "image too small\n"); exit(2); }
    fseek(f, 0, SEEK_SET);
    E.chip_profile = selected_chip ? selected_chip : chip_profile_find("esp8266");
    uint32_t size = forced_size ? forced_size : (selected_chip ? selected_chip->default_flash_size :
                                               (uint32_t)flash_size_from_header(hdr[3]));
    char error[180];
    if (!chip_profile_validate(E.chip_profile, size, (uint64_t)n, error, sizeof error)) {
        fprintf(stderr, "flash: %s\n", error);
        exit(2);
    }
    E.flash = malloc(size + 8);
    if (!E.flash) { fprintf(stderr, "cannot allocate flash\n"); exit(2); }
    memset(E.flash, 0xFF, size + 8);
    if (fread(E.flash, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read error\n"); exit(2); }
    fclose(f);
    E.flash_size = size;
    int cap = 0; while ((1u << cap) < size) cap++;
    E.flash_id = forced_id ? forced_id : 0xEF | (0x40 << 8) | ((uint32_t)cap << 16);
}

static void write_report(const char *why)
{
    if (!report_path) return;
    FILE *f = fopen(report_path, "w");
    if (!f) return;
    char e[1100]; json_escape(e, sizeof e, halt_msg);
    fprintf(f, "{\n  \"end\": \"%s\",\n  \"halt\": \"%s\",\n  \"stage\": \"%s\",\n  \"boots\": %llu,\n"
               "  \"virtual_us\": %llu,\n  \"instructions_since_boot\": %llu,\n  \"loop_task_dispatches\": %u,\n"
               "  \"setup_calls\": %llu,\n  \"loop_calls\": %llu,\n  \"loop_calls_all_boots\": %llu,\n"
               "  \"uart0_bytes\": %llu,\n  \"uart1_bytes\": %llu,\n"
               "  \"uart0_bytes_on_wire\": %llu,\n  \"cpu_mhz\": %u,\n  \"flash_size\": %u,\n"
               "  \"chip\": \"%s\",\n  \"flash_id\": %u\n}\n",
            why, e, stage_name(E.stage), (unsigned long long)E.boot_count, (unsigned long long)now_us(),
            (unsigned long long)(E.cycles - E.boot_cycles), E.loop_dispatches, (unsigned long long)E.setup_calls,
            (unsigned long long)E.loop_calls, (unsigned long long)E.loop_total, (unsigned long long)uart_total[0],
            (unsigned long long)uart_total[1], (unsigned long long)uart_wire[0], E.cpu_mhz, E.flash_size,
            E.chip_profile ? E.chip_profile->name : "esp8266", E.flash_id);
    fclose(f);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: esp8266emu --flash IMAGE.bin [options]\n"
        "  --chip NAME            esp8266, esp12e, esp12f, esp8285, esp8285h16\n"
        "  --flash-size BYTES     set physical capacity (fixed for ESP8285 profiles)\n"
        "  --flash-id HEX         set the 24-bit JEDEC flash ID\n"
        "  --flash-out FILE       persist flash writes (config, OTA) to FILE\n"
        "  --speed X              1 = real time (default), 0 = as fast as possible\n"
        "  --max-seconds S        stop after S seconds of emulated time\n"
        "  --sym-setup ADDR --sym-loop ADDR   count setup()/loop() entries (from the ELF)\n"
        "  --until-loops N        stop once loop() has run N times\n"
        "  --uart-log FILE        raw copy of everything the UARTs sent\n"
        "  --report FILE          JSON summary at exit\n"
        "  --no-stdin             do not read commands\n"
        "  --quiet                no event stream (stderr summary only)\n"
        "  --strict               stop at the first unimplemented ROM entry\n");
    exit(2);
}

int main(int argc, char **argv)
{
    uint32_t forced_size = 0, forced_id = 0;
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--flash") && v) { flash_path = v; i++; }
        else if (!strcmp(a, "--chip") && v) {
            selected_chip = chip_profile_find(v);
            if (!selected_chip) { fprintf(stderr, "unknown chip profile: %s\n", v); return 2; }
            i++;
        }
        else if (!strcmp(a, "--flash-size") && v) {
            forced_size = flash_option(v, a);
            if (!forced_size) { fprintf(stderr, "flash size must be greater than zero\n"); return 2; }
            i++;
        }
        else if (!strcmp(a, "--flash-id") && v) {
            forced_id = flash_option(v, a);
            if (!forced_id || forced_id > 0xffffffu) {
                fprintf(stderr, "flash ID must be a nonzero 24-bit value\n"); return 2;
            }
            i++;
        }
        else if (!strcmp(a, "--flash-out") && v) { flash_out = v; i++; }
        else if (!strcmp(a, "--speed") && v) { speed = atof(v); i++; }
        else if (!strcmp(a, "--max-seconds") && v) { max_vsec = atof(v); i++; }
        else if (!strcmp(a, "--sym-setup") && v) { E.sym_setup = (uint32_t)strtoul(v, NULL, 0); i++; }
        else if (!strcmp(a, "--sym-loop") && v) { E.sym_loop = (uint32_t)strtoul(v, NULL, 0); i++; }
        else if (!strcmp(a, "--until-loops") && v) { until_loops = strtoull(v, NULL, 0); i++; }
        else if (!strcmp(a, "--uart-log") && v) { uart_log_path = v; i++; }
        else if (!strcmp(a, "--report") && v) { report_path = v; i++; }
        else if (!strcmp(a, "--profile") && v) { profile_path = v; i++; }
        else if (!strcmp(a, "--script") && v) { load_script(v); i++; }
        else if (!strcmp(a, "--calltrace") && v) { unsigned long long f = 0, t = 0; sscanf(v, "%llu:%llu", &f, &t); E.ctrace_from = f; E.ctrace_to = t; E.ctrace = 1; i++; }
        else if (!strcmp(a, "--mmiotrace") && v) { unsigned long lo = 0, hi = 0; sscanf(v, "%lx:%lx", &lo, &hi); E.mtrace_lo = (uint32_t)lo; E.mtrace_hi = (uint32_t)hi; i++; }
        else if (!strcmp(a, "--watch") && v) { char nm[96] = "fn"; uint32_t ad = (uint32_t)strtoul(v, NULL, 0); const char *eq = strchr(v, '='); if (eq) snprintf(nm, sizeof nm, "%s", eq + 1); watch_add(ad, nm); i++; }
        else if (!strcmp(a, "--no-stdin")) stdin_live = 0;
        else if (!strcmp(a, "--quiet")) quiet = 1;
        else if (!strcmp(a, "--strict")) E.strict = true;
        else if (!strcmp(a, "--trace")) E.trace = true;
        else usage();
    }
    if (!flash_path) usage();
    load_flash(forced_size, forced_id);
    if (uart_log_path) uart_log = fopen(uart_log_path, "wb");
    rom_init();
    wall_ref_ns = stat_ns_ref = wall_ns();

    int code = setjmp(E.reset_jmp);
    switch (code) {
    case 0: case RJ_POWER:
        E.reset_reason = 1; soc_power_on(); E.tick_epoch = E.tick; E.cycles = 0; break;
    case RJ_EXT:
        E.reset_reason = 2; soc_reset(true); E.tick_epoch = E.tick; E.cycles = 0; break;
    case RJ_SOFT:
        break;                                  // jump to the reset vector: peripherals and RAM keep their state
    case RJ_WDT:
        E.reset_reason = 4; soc_reset(true); E.tick_epoch = E.tick; E.cycles = 0; break;
    case RJ_HALT:
        halted_loop();
        break;
    case RJ_QUIT:
    default:
        flush_uart();
        save_flash();
        write_report(halt_msg[0] ? "halt" : "quit");
        prof_dump();
        fflush(stdout);
        if (quiet) fprintf(stderr, "end: stage %s, %llu us virtual, %llu insn, loop() %llu, loop task %u\n",
                           stage_name(E.stage), (unsigned long long)now_us(), (unsigned long long)(E.cycles - E.boot_cycles),
                           (unsigned long long)E.loop_calls, E.loop_dispatches);
        return 0;
    }
    E.boot_count++;
    E.call_depth = 0;
    E.fault_jmp = NULL;
    E.boot_cycles = E.cycles;
    // A rapid watchdog reset must not postpone host I/O and --max-seconds forever.
    if (code != RJ_WDT) E.slice_end = E.tick + 160000;
    cpu_fetch_invalidate();
    if (code != RJ_SOFT) emit_pins(1);
    rom_boot();
    cpu_run(0);
    return 0;
}

void emu_watchdog_reset(void)
{
    longjmp(E.reset_jmp, RJ_WDT);
}
