// Xtensa LX106 interpreter (ESP8266 configuration: CALL0 ABI, code density, MUL16/MUL32, NSA,
// exceptions, level-1 interrupts 0..13, NMI 14 at level 3, one CCOMPARE timer, no windows/loops/FPU/MAC16).
// Options the chip lacks (DIV32, MUL32_HIGH, MIN/MAX, SEXT, CLAMPS, S32C1I) are implemented anyway: the
// compiler never emits them for lx106, so they cost nothing, and a stray one does the obvious thing.
#include "emu.h"
#include <string.h>
#include <stdio.h>

emu_t E;
cpu_t C;
uint8_t mem_dram[DRAM_SIZE + 8], mem_iram[IRAM_SIZE + 8], mem_rom[ROM_SIZE + 8], mem_rtc[RTCMEM_SIZE + 8];

uint32_t pc_ring[64];
unsigned pc_ring_i;

static inline uint32_t rd32le(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t sext(uint32_t v, int bits) { return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits)); }

static const int32_t B4CONST[16]  = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256 };
static const uint32_t B4CONSTU[16] = { 32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256 };

// Instruction fetch: IRAM is a fixed window; the mapped flash window is cached (re-derived after a remap).
// Code bounces between the two all the time, so both are checked inline.
static const uint8_t *fw_base;
static uint32_t fw_len;

void cpu_fetch_invalidate(void) { fw_len = 0; }

static const uint8_t *fetch_slow(uint32_t pc)
{
    const uint8_t *p = fetch_ptr(pc);
    if (p && pc - FLASHMAP_BASE < FLASHMAP_SIZE) {
        uint32_t avail = E.flash_size > E.flash_map_off ? E.flash_size - E.flash_map_off : 0;
        if (avail > FLASHMAP_SIZE) avail = FLASHMAP_SIZE;
        fw_base = E.flash + E.flash_map_off;
        fw_len = avail >= 4 ? avail - 4 : 0;
    }
    return p;
}

static inline const uint8_t *fetch(uint32_t pc)
{
    if (pc - IRAM_BASE < IRAM_SIZE) return mem_iram + (pc - IRAM_BASE);
    uint32_t off = pc - FLASHMAP_BASE;
    if (off < fw_len) return fw_base + off;
    return fetch_slow(pc);
}

uint32_t cpu_ccount(void) { return (uint32_t)E.cycles + C.ccount_adj; }

void cpu_reschedule_ccompare(void)
{
    uint32_t d = C.ccompare - cpu_ccount();
    uint64_t dc = d ? d : 0x100000000ull;
    E.ccomp_tick = E.tick + dc * E.tpc;
    soc_schedule();
}

void cpu_set_freq(uint32_t mhz)
{
    if (mhz != 80 && mhz != 160) mhz = 80;
    E.cpu_mhz = mhz;
    E.tpc = 160 / mhz;
    cpu_reschedule_ccompare();
}

void irq_update(void)
{
    uint32_t pend = C.intpend | E.irq_lines;
    E.irq_req = (pend & C.intenable & L1_INT_MASK) | (C.intpend & (1u << INUM_NMI));
}

void irq_raise_edge(int n)
{
    C.intpend |= 1u << n;
    irq_update();
}

void cpu_reset(void)
{
    memset(&C, 0, sizeof C);
    C.ps = 0x1F;
    C.vecbase = ROM_BASE;
    C.pc = 0x40000080;
    E.irq_req = 0;
}

void cpu_exception(uint32_t cause, uint32_t vaddr, uint32_t fault_pc)
{
    C.waiting = 0;
    C.exccause = cause;
    C.excvaddr = vaddr;
    if (C.ps & PS_EXCM) {
        C.depc = fault_pc;
        C.pc = C.vecbase + 0x70;
    } else {
        C.epc[1] = fault_pc;
        C.ps |= PS_EXCM;
        C.pc = C.vecbase + ((C.ps & PS_UM) ? 0x50 : 0x30);
    }
    if (E.trace) fprintf(stderr, "exception cause %u vaddr %08x pc %08x\n", cause, vaddr, fault_pc);
}

static void debug_exception(uint32_t pc, uint32_t dcause)
{
    if ((C.ps & PS_INTLEVEL) >= 2) return;
    ev_log("debug exception (BREAK) at pc %08x: the SDK vector parks the CPU here (hang until watchdog)", pc);
    C.epc[2] = pc;
    C.eps[2] = C.ps;
    C.debugcause = dcause;
    C.ps = (C.ps & ~PS_INTLEVEL) | 2 | PS_EXCM;
    C.pc = C.vecbase + 0x10;
}

static uint32_t rsr(int sr)
{
    switch (sr) {
    case 3:   return C.sar;
    case 5:   return C.litbase;
    case 12:  return C.scompare1;
    case 177: return C.epc[1];
    case 178: return C.epc[2];
    case 179: return C.epc[3];
    case 192: return C.depc;
    case 194: return C.eps[2];
    case 195: return C.eps[3];
    case 209: return C.excsave[1];
    case 210: return C.excsave[2];
    case 211: return C.excsave[3];
    case 226: return C.intpend | E.irq_lines;
    case 228: return C.intenable;
    case 230: return C.ps;
    case 231: return C.vecbase;
    case 232: return C.exccause;
    case 233: return C.debugcause;
    case 234: return cpu_ccount();
    case 235: return 0;            // PRID
    case 238: return C.excvaddr;
    case 240: return C.ccompare;
    case 244: case 245: case 246: case 247: return C.misc[sr - 244];
    default:  return C.sr_other[sr & 255];
    }
}

static void wsr(int sr, uint32_t v)
{
    switch (sr) {
    case 3:   C.sar = v & 63; break;
    case 5:   C.litbase = v; break;
    case 12:  C.scompare1 = v; break;
    case 177: C.epc[1] = v; break;
    case 178: C.epc[2] = v; break;
    case 179: C.epc[3] = v; break;
    case 192: C.depc = v; break;
    case 194: C.eps[2] = v; break;
    case 195: C.eps[3] = v; break;
    case 209: C.excsave[1] = v; break;
    case 210: C.excsave[2] = v; break;
    case 211: C.excsave[3] = v; break;
    case 226: C.intpend |= v & (1u << INUM_SOFT); irq_update(); break;           // INTSET
    case 227: C.intpend &= ~(v & EDGE_INT_MASK); irq_update(); break;            // INTCLEAR
    case 228: C.intenable = v; irq_update(); break;
    case 230: C.ps = v; break;
    case 231: C.vecbase = v; if (E.stage == STAGE_EBOOT || E.stage == STAGE_ROM) ev_stage(STAGE_APP); break;
    case 232: C.exccause = v; break;
    case 233: C.debugcause = v; break;
    case 234: C.ccount_adj = v - (uint32_t)E.cycles; cpu_reschedule_ccompare(); break;
    case 238: C.excvaddr = v; break;
    case 240: C.ccompare = v; C.intpend &= ~(1u << INUM_CCOMP); irq_update(); cpu_reschedule_ccompare(); break;
    case 244: case 245: case 246: case 247: C.misc[sr - 244] = v; break;
    default:  C.sr_other[sr & 255] = v; break;
    }
}

static inline uint32_t nsau(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t nsa(uint32_t x)
{
    int32_t s = (int32_t)x;
    uint32_t y = s < 0 ? ~x : x;
    return y ? (uint32_t)__builtin_clz(y) - 1 : 31;
}

// Fast-path memory for DRAM/IRAM, everything else through soc.c.
static inline uint32_t ld32(uint32_t a)
{
    if (a - DRAM_BASE < DRAM_SIZE) return rd32le(mem_dram + (a - DRAM_BASE));
    if (a - IRAM_BASE < IRAM_SIZE) return rd32le(mem_iram + (a - IRAM_BASE));
    return mem_ld32(a);
}
static inline void st32(uint32_t a, uint32_t v)
{
    if (a - DRAM_BASE < DRAM_SIZE) { memcpy(mem_dram + (a - DRAM_BASE), &v, 4); return; }
    if (a - IRAM_BASE < IRAM_SIZE) { memcpy(mem_iram + (a - IRAM_BASE), &v, 4); return; }
    mem_st32(a, v);
}
static inline uint32_t ld8(uint32_t a)
{
    if (a - DRAM_BASE < DRAM_SIZE) return mem_dram[a - DRAM_BASE];
    return mem_ld8(a);
}
static inline uint32_t ld16(uint32_t a)
{
    if (a - DRAM_BASE < DRAM_SIZE) { uint16_t v; memcpy(&v, mem_dram + (a - DRAM_BASE), 2); return v; }
    return mem_ld16(a);
}
static inline void st8(uint32_t a, uint32_t v)
{
    if (a - DRAM_BASE < DRAM_SIZE) { mem_dram[a - DRAM_BASE] = (uint8_t)v; return; }
    mem_st8(a, v);
}
static inline void st16(uint32_t a, uint32_t v)
{
    if (a - DRAM_BASE < DRAM_SIZE) { uint16_t h = (uint16_t)v; memcpy(mem_dram + (a - DRAM_BASE), &h, 2); return; }
    mem_st16(a, v);
}

#define ILLEGAL() do { cpu_exception(EXC_ILLEGAL, 0, pc); return; } while (0)

static inline int take_irq(void)
{
    uint32_t req = E.irq_req;
    if (req & (1u << INUM_NMI)) {
        C.intpend &= ~(1u << INUM_NMI);
        irq_update();
        C.waiting = 0;
        C.epc[3] = C.pc;
        C.eps[3] = C.ps;
        C.ps = (C.ps & ~PS_INTLEVEL) | 3 | PS_EXCM;
        C.pc = C.vecbase + 0x20;
        return 1;
    }
    if ((req & L1_INT_MASK) && !(C.ps & PS_EXCM) && (C.ps & PS_INTLEVEL) < 1) {
        C.waiting = 0;
        C.epc[1] = C.pc;
        C.exccause = EXC_LEVEL1_INT;
        C.ps |= PS_EXCM;
        C.pc = C.vecbase + ((C.ps & PS_UM) ? 0x50 : 0x30);
        return 1;
    }
    return 0;
}

static void step_slow(void)
{
    uint32_t pc = C.pc;
    const uint8_t *p = fetch(pc);
    if (!p) { cpu_exception(EXC_IFETCH_PROHIB, pc, pc); return; }
    uint32_t insn = p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
    uint32_t op0 = insn & 15;
    uint32_t t = (insn >> 4) & 15, s = (insn >> 8) & 15, r = (insn >> 12) & 15;
    uint32_t *A = C.a;

    if (op0 >= 8) {                     // 16-bit density instructions
        uint32_t npc = pc + 2;
        switch (op0) {
        case 8:  A[t] = ld32(A[s] + (r << 2)); break;                          // L32I.N
        case 9:  st32(A[s] + (r << 2), A[t]); break;                           // S32I.N
        case 10: A[r] = A[s] + A[t]; break;                                    // ADD.N
        case 11: A[r] = A[s] + (t ? t : (uint32_t)-1); break;                  // ADDI.N
        case 12:
            if (!(t & 8)) {                                                    // MOVI.N
                int32_t imm = (int32_t)(((t & 7) << 4) | r);
                if ((imm & 0x60) == 0x60) imm -= 128;
                A[s] = (uint32_t)imm;
            } else {                                                           // BEQZ.N / BNEZ.N
                uint32_t imm6 = ((t & 3) << 4) | r;
                int take = (t & 4) ? (A[s] != 0) : (A[s] == 0);
                if (take) npc = pc + 4 + imm6;
            }
            break;
        case 13:
            if (r == 0) { A[t] = A[s]; break; }                                // MOV.N
            if (r == 15) {
                switch (t) {
                case 0: npc = A[0]; pc_ring[pc_ring_i++ & 63] = pc; break;     // RET.N
                case 2: C.pc = npc; debug_exception(pc, 1u << 4); return;      // BREAK.N
                case 3: break;                                                 // NOP.N
                default: ILLEGAL();                                            // RETW.N, ILL.N
                }
                break;
            }
            ILLEGAL();
        default: ILLEGAL();
        }
        C.pc = npc;
        return;
    }

    uint32_t npc = pc + 3;
    uint32_t op1 = (insn >> 16) & 15, op2 = (insn >> 20) & 15;
    switch (op0) {
    case 0:  // QRST
        switch (op1) {
        case 0:  // RST0
            switch (op2) {
            case 0:  // ST0
                switch (r) {
                case 0: {  // SNM0
                    uint32_t m = (insn >> 6) & 3, n = (insn >> 4) & 3;
                    if (m == 2 && n == 0) { npc = A[0]; pc_ring[pc_ring_i++ & 63] = pc; break; }   // RET
                    if (m == 2 && n == 2) { npc = A[s]; pc_ring[pc_ring_i++ & 63] = pc; break; }   // JX
                    if (m == 3 && n == 0) { uint32_t tg = A[s]; A[0] = pc + 3; npc = tg; pc_ring[pc_ring_i++ & 63] = pc; if (E.ctrace) ctrace_log(pc, tg); break; } // CALLX0
                    ILLEGAL();
                }
                case 2:  // SYNC group: ISYNC RSYNC ESYNC DSYNC EXCW MEMW EXTW NOP
                    break;
                case 3:  // RFEI
                    if (t == 0) {
                        if (s == 0) { C.ps &= ~PS_EXCM; npc = C.epc[1]; break; }   // RFE
                        if (s == 2) { C.ps &= ~PS_EXCM; npc = C.depc; break; }     // RFDE
                        ILLEGAL();
                    }
                    if (t == 1 && s >= 2 && s <= 3) { C.ps = C.eps[s]; npc = C.epc[s]; break; } // RFI
                    ILLEGAL();
                case 4:  // BREAK
                    C.pc = npc; debug_exception(pc, 1u << 3); return;
                case 5:  // SYSCALL / SIMCALL
                    if (s == 0) { cpu_exception(EXC_SYSCALL, 0, pc); return; }
                    break;
                case 6:  { uint32_t old = C.ps; C.ps = (C.ps & ~PS_INTLEVEL) | s; A[t] = old; break; } // RSIL
                case 7:  C.ps = (C.ps & ~PS_INTLEVEL) | s; C.waiting = 1; break;  // WAITI
                default: ILLEGAL();
                }
                break;
            case 1:  A[r] = A[s] & A[t]; break;                                // AND
            case 2:  A[r] = A[s] | A[t]; break;                                // OR
            case 3:  A[r] = A[s] ^ A[t]; break;                                // XOR
            case 4:  // ST1
                switch (r) {
                case 0:  C.sar = A[s] & 31; break;                             // SSR
                case 1:  C.sar = 32 - (A[s] & 31); break;                      // SSL
                case 2:  C.sar = (A[s] & 3) * 8; break;                        // SSA8L
                case 3:  C.sar = 32 - (A[s] & 3) * 8; break;                   // SSA8B
                case 4:  C.sar = s | ((t & 1) << 4); break;                    // SSAI
                case 14: A[t] = nsa(A[s]); break;                              // NSA
                case 15: A[t] = nsau(A[s]); break;                             // NSAU
                default: ILLEGAL();
                }
                break;
            case 6:  // RT0
                if (s == 0) { A[r] = 0u - A[t]; break; }        // NEG
                if (s == 1) { int32_t v = (int32_t)A[t]; A[r] = v < 0 ? 0u - A[t] : A[t]; break; } // ABS
                ILLEGAL();
            case 8:  A[r] = A[s] + A[t]; break;                                // ADD
            case 9:  A[r] = (A[s] << 1) + A[t]; break;                         // ADDX2
            case 10: A[r] = (A[s] << 2) + A[t]; break;                         // ADDX4
            case 11: A[r] = (A[s] << 3) + A[t]; break;                         // ADDX8
            case 12: A[r] = A[s] - A[t]; break;                                // SUB
            case 13: A[r] = (A[s] << 1) - A[t]; break;                         // SUBX2
            case 14: A[r] = (A[s] << 2) - A[t]; break;                         // SUBX4
            case 15: A[r] = (A[s] << 3) - A[t]; break;                         // SUBX8
            default: ILLEGAL();
            }
            break;
        case 1:  // RST1
            switch (op2) {
            case 0: case 1: {                                                  // SLLI
                uint32_t sh = 32 - (((op2 & 1) << 4) | t);
                A[r] = sh >= 32 ? 0 : A[s] << sh;
                break;
            }
            case 2: case 3: A[r] = (uint32_t)((int32_t)A[t] >> (((op2 & 1) << 4) | s)); break; // SRAI
            case 4:  A[r] = A[t] >> s; break;                                  // SRLI
            case 6: { uint32_t tmp = A[t]; A[t] = rsr((r << 4) | s); wsr((r << 4) | s, tmp); break; } // XSR
            case 8:  A[r] = (uint32_t)((((uint64_t)A[s] << 32) | A[t]) >> (C.sar & 63)); break; // SRC
            case 9:  A[r] = (uint32_t)((uint64_t)A[t] >> (C.sar & 63)); break; // SRL
            case 10: A[r] = (uint32_t)((uint64_t)A[s] << ((32 - C.sar) & 63)); break; // SLL
            case 11: A[r] = (uint32_t)((int64_t)(int32_t)A[t] >> (C.sar & 63)); break; // SRA
            case 12: A[r] = (A[s] & 0xFFFF) * (A[t] & 0xFFFF); break;          // MUL16U
            case 13: A[r] = (uint32_t)((int32_t)(int16_t)A[s] * (int32_t)(int16_t)A[t]); break; // MUL16S
            default: ILLEGAL();
            }
            break;
        case 2:  // RST2
            switch (op2) {
            case 8:  A[r] = A[s] * A[t]; break;                                // MULL
            case 10: A[r] = (uint32_t)(((uint64_t)A[s] * A[t]) >> 32); break;  // MULUH
            case 11: A[r] = (uint32_t)(((int64_t)(int32_t)A[s] * (int32_t)A[t]) >> 32); break; // MULSH
            case 12: case 13: case 14: case 15:
                if (A[t] == 0) { cpu_exception(EXC_DIV0, 0, pc); return; }
                if (op2 == 12) A[r] = A[s] / A[t];
                else if (op2 == 14) A[r] = A[s] % A[t];
                else {
                    int32_t a = (int32_t)A[s], b = (int32_t)A[t];
                    if (a == INT32_MIN && b == -1) A[r] = op2 == 13 ? (uint32_t)a : 0;
                    else A[r] = (uint32_t)(op2 == 13 ? a / b : a % b);
                }
                break;
            default: ILLEGAL();
            }
            break;
        case 3:  // RST3
            switch (op2) {
            case 0:  A[t] = rsr((r << 4) | s); break;                          // RSR
            case 1:  wsr((r << 4) | s, A[t]); break;                           // WSR
            case 2:  A[r] = sext(A[s], t + 8); break;                          // SEXT
            case 3: {                                                          // CLAMPS
                int32_t v = (int32_t)A[s], hi = (1 << (t + 7)) - 1, lo = -(1 << (t + 7));
                A[r] = (uint32_t)(v > hi ? hi : v < lo ? lo : v);
                break;
            }
            case 4:  A[r] = (int32_t)A[s] < (int32_t)A[t] ? A[s] : A[t]; break; // MIN
            case 5:  A[r] = (int32_t)A[s] > (int32_t)A[t] ? A[s] : A[t]; break; // MAX
            case 6:  A[r] = A[s] < A[t] ? A[s] : A[t]; break;                  // MINU
            case 7:  A[r] = A[s] > A[t] ? A[s] : A[t]; break;                  // MAXU
            case 8:  if (A[t] == 0) A[r] = A[s]; break;                        // MOVEQZ
            case 9:  if (A[t] != 0) A[r] = A[s]; break;                        // MOVNEZ
            case 10: if ((int32_t)A[t] < 0) A[r] = A[s]; break;                // MOVLTZ
            case 11: if ((int32_t)A[t] >= 0) A[r] = A[s]; break;               // MOVGEZ
            case 14: A[r] = ((s << 4) | t) == 231 ? C.threadptr : 0; break;    // RUR
            case 15: if (((r << 4) | s) == 231) C.threadptr = A[t]; break;     // WUR
            default: ILLEGAL();
            }
            break;
        case 4: case 5: {                                                      // EXTUI
            uint32_t sh = ((op1 & 1) << 4) | s, bits = op2 + 1;
            A[r] = (A[t] >> sh) & ((1u << bits) - 1);
            break;
        }
        default: ILLEGAL();
        }
        break;

    case 1: {                                                                  // L32R
        uint32_t off = (0xFFFF0000u | (insn >> 8)) << 2;
        uint32_t base = (C.litbase & 1) ? (C.litbase & 0xFFFFF000u) : ((pc + 3) & ~3u);
        A[t] = ld32(base + off);
        break;
    }

    case 2: {  // LSAI
        uint32_t imm8 = insn >> 16, base = A[s];
        switch (r) {
        case 0:  A[t] = ld8(base + imm8); break;                               // L8UI
        case 1:  A[t] = ld16(base + (imm8 << 1)); break;                       // L16UI
        case 2:  A[t] = ld32(base + (imm8 << 2)); break;                       // L32I
        case 4:  st8(base + imm8, A[t]); break;                                // S8I
        case 5:  st16(base + (imm8 << 1), A[t]); break;                        // S16I
        case 6:  st32(base + (imm8 << 2), A[t]); break;                        // S32I
        case 7:  break;                                                        // cache ops
        case 9:  A[t] = sext(ld16(base + (imm8 << 1)), 16); break;             // L16SI
        case 10: A[t] = sext((s << 8) | imm8, 12); break;                      // MOVI
        case 11: A[t] = ld32(base + (imm8 << 2)); break;                       // L32AI
        case 12: A[t] = base + sext(imm8, 8); break;                           // ADDI
        case 13: A[t] = base + (sext(imm8, 8) << 8); break;                    // ADDMI
        case 14: {                                                             // S32C1I
            uint32_t ad = base + (imm8 << 2), old = ld32(ad);
            if (old == C.scompare1) st32(ad, A[t]);
            A[t] = old;
            break;
        }
        case 15: st32(base + (imm8 << 2), A[t]); break;                        // S32RI
        default: ILLEGAL();
        }
        break;
    }

    case 5: {  // CALLN
        if (((insn >> 4) & 3) != 0) ILLEGAL();                                 // CALL4/8/12: no windows
        uint32_t off = sext(insn >> 6, 18);
        A[0] = pc + 3;
        npc = (pc & ~3u) + (off << 2) + 4;
        pc_ring[pc_ring_i++ & 63] = pc;
        if (E.ctrace) ctrace_log(pc, npc);
        break;
    }

    case 6: {  // SI
        uint32_t n = (insn >> 4) & 3, m = (insn >> 6) & 3;
        switch (n) {
        case 0: npc = pc + 4 + sext(insn >> 6, 18); break;                     // J
        case 1: {                                                              // BZ
            uint32_t tg = pc + 4 + sext(insn >> 12, 12);
            int32_t v = (int32_t)A[s];
            int take = m == 0 ? v == 0 : m == 1 ? v != 0 : m == 2 ? v < 0 : v >= 0;
            if (take) npc = tg;
            break;
        }
        case 2: {                                                              // BI0
            uint32_t tg = pc + 4 + sext(insn >> 16, 8);
            int32_t v = (int32_t)A[s], c = B4CONST[r];
            int take = m == 0 ? v == c : m == 1 ? v != c : m == 2 ? v < c : v >= c;
            if (take) npc = tg;
            break;
        }
        case 3: {                                                              // BI1
            uint32_t tg = pc + 4 + sext(insn >> 16, 8);
            if (m == 2) { if (A[s] < B4CONSTU[r]) npc = tg; break; }           // BLTUI
            if (m == 3) { if (A[s] >= B4CONSTU[r]) npc = tg; break; }          // BGEUI
            ILLEGAL();                                                         // ENTRY, BF/BT, LOOP*
        }
        }
        break;
    }

    case 7: {  // B (RRI8)
        uint32_t tg = pc + 4 + sext(insn >> 16, 8), as = A[s], at = A[t];
        int take;
        switch (r) {
        case 0:  take = (as & at) == 0; break;                                 // BNONE
        case 1:  take = as == at; break;                                       // BEQ
        case 2:  take = (int32_t)as < (int32_t)at; break;                      // BLT
        case 3:  take = as < at; break;                                        // BLTU
        case 4:  take = (~as & at) == 0; break;                                // BALL
        case 5:  take = !((as >> (at & 31)) & 1); break;                       // BBC
        case 6: case 7: take = !((as >> (((r & 1) << 4) | t)) & 1); break;     // BBCI
        case 8:  take = (as & at) != 0; break;                                 // BANY
        case 9:  take = as != at; break;                                       // BNE
        case 10: take = (int32_t)as >= (int32_t)at; break;                     // BGE
        case 11: take = as >= at; break;                                       // BGEU
        case 12: take = (~as & at) != 0; break;                                // BNALL
        case 13: take = (as >> (at & 31)) & 1; break;                          // BBS
        default: take = (as >> (((r & 1) << 4) | t)) & 1; break;               // BBSI
        }
        if (take) npc = tg;
        break;
    }

    default: ILLEGAL();                                                        // LSCII, MAC16, FP
    }
    C.pc = npc;
}

// Decoded operands are validated against both PC and fetched bytes on every use.
// This covers IRAM writes, flash programming and cache-window remapping without
// write-side invalidation. Direct-mapped collisions only cause a fresh decode.
enum decoded_op {
    D_SLOW, D_L32IN, D_S32IN, D_ADDN, D_ADDIN, D_MOVIN, D_BEQZN, D_BNEZN,
    D_MOVN, D_RETN, D_NOPN,
    D_L8UI, D_L16UI, D_L32I, D_S8I, D_S16I, D_S32I, D_L16SI, D_MOVI, D_ADDI,
    D_ADDMI, D_AND, D_OR, D_XOR, D_ADD, D_ADDX2, D_ADDX4, D_ADDX8,
    D_SUB, D_SUBX2, D_SUBX4, D_SUBX8, D_MULL,
    D_J, D_CALL0, D_BEQZ, D_BNEZ, D_BLTZ, D_BGEZ,
    D_BEQI, D_BNEI, D_BLTI, D_BGEI, D_BLTUI, D_BGEUI,
    D_BNONE, D_BEQ, D_BLT, D_BLTU, D_BALL, D_BBC, D_BBCI, D_BANY,
    D_BNE, D_BGE, D_BGEU, D_BNALL, D_BBS, D_BBSI,
    D_EXTUI, D_SLLI, D_SRAI, D_SRLI, D_SLL, D_SRL, D_SRA, D_SRC,
    D_SSR, D_SSL, D_SSAI, D_SSA8L, D_SSA8B,
    D_MOVEQZ, D_MOVNEZ, D_MOVLTZ, D_MOVGEZ, D_L32R, D_JX, D_CALLX0,
};
typedef struct {
    uint32_t pc, raw, imm;
    uint8_t op, r, s, t;
} decoded_t;
#define DECODE_SLOTS 16384u
static decoded_t decoded[DECODE_SLOTS];

static void decode(decoded_t *d, uint32_t pc, uint32_t insn)
{
    uint32_t op0=insn&15, t=(insn>>4)&15, s=(insn>>8)&15, r=(insn>>12)&15;
    uint32_t op1=(insn>>16)&15, op2=(insn>>20)&15;
    *d=(decoded_t){.pc=pc,.raw=insn|((op0>=8?2u:3u)<<24),.op=D_SLOW,.r=r,.s=s,.t=t};
    if(op0>=8) {
        switch(op0) {
        case 8: d->op=D_L32IN; d->imm=r<<2; break;
        case 9: d->op=D_S32IN; d->imm=r<<2; break;
        case 10: d->op=D_ADDN; break;
        case 11: d->op=D_ADDIN; d->imm=t?t:(uint32_t)-1; break;
        case 12:
            if(!(t&8)) {
                d->op=D_MOVIN; d->imm=((t&7)<<4)|r;
                if((d->imm&0x60)==0x60) d->imm-=128;
            } else { d->op=(t&4)?D_BNEZN:D_BEQZN; d->imm=pc+4+((t&3)<<4)+r; }
            break;
        case 13:
            if(r==0) d->op=D_MOVN;
            else if(r==15 && t==0) d->op=D_RETN;
            else if(r==15 && t==3) d->op=D_NOPN;
            break;
        }
        return;
    }
    switch(op0) {
    case 0:
        if(op1==0) {
            switch(op2) {
            case 0:
                if(r==0) {
                    if(t==8) d->op=D_RETN;
                    else if(t==10) d->op=D_JX;
                    else if(t==12) d->op=D_CALLX0;
                } else if(r==2) d->op=D_NOPN;
                break;
            case 1: d->op=D_AND; break;
            case 2: d->op=D_OR; break;
            case 3: d->op=D_XOR; break;
            case 4:
                switch(r) {
                case 0: d->op=D_SSR; break;
                case 1: d->op=D_SSL; break;
                case 2: d->op=D_SSA8L; break;
                case 3: d->op=D_SSA8B; break;
                case 4: d->op=D_SSAI; d->imm=s|((t&1)<<4); break;
                }
                break;
            case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
                d->op=D_ADD+op2-8; break;
            }
        } else if(op1==1) {
            switch(op2) {
            case 0: case 1: d->op=D_SLLI; d->imm=32-(((op2&1)<<4)|t); break;
            case 2: case 3: d->op=D_SRAI; d->imm=((op2&1)<<4)|s; break;
            case 4: d->op=D_SRLI; d->imm=s; break;
            case 8: d->op=D_SRC; break;
            case 9: d->op=D_SRL; break;
            case 10: d->op=D_SLL; break;
            case 11: d->op=D_SRA; break;
            }
        } else if(op1==2 && op2==8) d->op=D_MULL;
        else if(op1==3 && op2>=8 && op2<=11) d->op=D_MOVEQZ+op2-8;
        else if(op1==4 || op1==5) { d->op=D_EXTUI; d->imm=(1u<<(op2+1))-1; d->s=((op1&1)<<4)|s; }
        break;
    case 1: d->op=D_L32R; d->imm=(0xffff0000u|(insn>>8))<<2; break;
    case 2:
        d->imm=insn>>16;
        switch(r) {
        case 0: d->op=D_L8UI; break;
        case 1: d->op=D_L16UI; d->imm<<=1; break;
        case 2: d->op=D_L32I; d->imm<<=2; break;
        case 4: d->op=D_S8I; break;
        case 5: d->op=D_S16I; d->imm<<=1; break;
        case 6: d->op=D_S32I; d->imm<<=2; break;
        case 7: d->op=D_NOPN; break;
        case 9: d->op=D_L16SI; d->imm<<=1; break;
        case 10: d->op=D_MOVI; d->imm=sext((s<<8)|d->imm,12); break;
        case 11: d->op=D_L32I; d->imm<<=2; break;
        case 12: d->op=D_ADDI; d->imm=sext(d->imm,8); break;
        case 13: d->op=D_ADDI; d->imm=sext(d->imm,8)<<8; break;
        case 15: d->op=D_S32I; d->imm<<=2; break;
        }
        break;
    case 5:
        if((t&3)==0) { d->op=D_CALL0; d->imm=(pc&~3u)+(sext(insn>>6,18)<<2)+4; }
        break;
    case 6: {
        uint32_t n=t&3,m=t>>2;
        if(n==0) { d->op=D_J; d->imm=pc+4+sext(insn>>6,18); }
        else if(n==1) { d->op=D_BEQZ+m; d->imm=pc+4+sext(insn>>12,12); }
        else if(n==2) { d->op=D_BEQI+m; d->imm=pc+4+sext(insn>>16,8); }
        else if(m>=2) { d->op=D_BLTUI+m-2; d->imm=pc+4+sext(insn>>16,8); }
        break;
    }
    case 7: {
        static const uint8_t ops[]={D_BNONE,D_BEQ,D_BLT,D_BLTU,D_BALL,D_BBC,D_BBCI,D_BBCI,D_BANY,D_BNE,D_BGE,D_BGEU,D_BNALL,D_BBS,D_BBSI,D_BBSI};
        d->op=ops[r]; d->imm=pc+4+sext(insn>>16,8);
        if(r==6 || r==7 || r==14 || r==15) d->t=((r&1)<<4)|t;
        break;
    }
    }
}

// Keep an indirect dispatch site for each operation so the host branch predictor
// can learn its successors. GCC tail merging would collapse these NEXT sites.
static __attribute__((optimize("no-crossjumping,no-gcse"))) void cpu_block(unsigned budget, int observe)
{
    uint32_t pc,npc,*A=C.a,r,s,t,imm;
    decoded_t *d;
    static const void *dispatch[]={
        [D_SLOW]=&&op_D_SLOW,
        [D_L32IN]=&&op_D_L32IN,
        [D_S32IN]=&&op_D_S32IN,
        [D_ADDN]=&&op_D_ADDN,
        [D_ADDIN]=&&op_D_ADDIN,
        [D_MOVIN]=&&op_D_MOVIN,
        [D_BEQZN]=&&op_D_BEQZN,
        [D_BNEZN]=&&op_D_BNEZN,
        [D_MOVN]=&&op_D_MOVN,
        [D_RETN]=&&op_D_RETN,
        [D_NOPN]=&&op_D_NOPN,
        [D_L8UI]=&&op_D_L8UI,
        [D_L16UI]=&&op_D_L16UI,
        [D_L32I]=&&op_D_L32I,
        [D_S8I]=&&op_D_S8I,
        [D_S16I]=&&op_D_S16I,
        [D_S32I]=&&op_D_S32I,
        [D_L16SI]=&&op_D_L16SI,
        [D_MOVI]=&&op_D_MOVI,
        [D_ADDI]=&&op_D_ADDI,
        [D_ADDMI]=&&op_D_SLOW,
        [D_AND]=&&op_D_AND,
        [D_OR]=&&op_D_OR,
        [D_XOR]=&&op_D_XOR,
        [D_ADD]=&&op_D_ADD,
        [D_ADDX2]=&&op_D_ADDX2,
        [D_ADDX4]=&&op_D_ADDX4,
        [D_ADDX8]=&&op_D_ADDX8,
        [D_SUB]=&&op_D_SUB,
        [D_SUBX2]=&&op_D_SUBX2,
        [D_SUBX4]=&&op_D_SUBX4,
        [D_SUBX8]=&&op_D_SUBX8,
        [D_MULL]=&&op_D_MULL,
        [D_J]=&&op_D_J,
        [D_CALL0]=&&op_D_CALL0,
        [D_BEQZ]=&&op_D_BEQZ,
        [D_BNEZ]=&&op_D_BNEZ,
        [D_BLTZ]=&&op_D_BLTZ,
        [D_BGEZ]=&&op_D_BGEZ,
        [D_BEQI]=&&op_D_BEQI,
        [D_BNEI]=&&op_D_BNEI,
        [D_BLTI]=&&op_D_BLTI,
        [D_BGEI]=&&op_D_BGEI,
        [D_BLTUI]=&&op_D_BLTUI,
        [D_BGEUI]=&&op_D_BGEUI,
        [D_BNONE]=&&op_D_BNONE,
        [D_BEQ]=&&op_D_BEQ,
        [D_BLT]=&&op_D_BLT,
        [D_BLTU]=&&op_D_BLTU,
        [D_BALL]=&&op_D_BALL,
        [D_BBC]=&&op_D_BBC,
        [D_BBCI]=&&op_D_BBCI,
        [D_BANY]=&&op_D_BANY,
        [D_BNE]=&&op_D_BNE,
        [D_BGE]=&&op_D_BGE,
        [D_BGEU]=&&op_D_BGEU,
        [D_BNALL]=&&op_D_BNALL,
        [D_BBS]=&&op_D_BBS,
        [D_BBSI]=&&op_D_BBSI,
        [D_EXTUI]=&&op_D_EXTUI,
        [D_SLLI]=&&op_D_SLLI,
        [D_SRAI]=&&op_D_SRAI,
        [D_SRLI]=&&op_D_SRLI,
        [D_SLL]=&&op_D_SLL,
        [D_SRL]=&&op_D_SRL,
        [D_SRA]=&&op_D_SRA,
        [D_SRC]=&&op_D_SRC,
        [D_SSR]=&&op_D_SSR,
        [D_SSL]=&&op_D_SSL,
        [D_SSAI]=&&op_D_SSAI,
        [D_SSA8L]=&&op_D_SSA8L,
        [D_SSA8B]=&&op_D_SSA8B,
        [D_MOVEQZ]=&&op_D_MOVEQZ,
        [D_MOVNEZ]=&&op_D_MOVNEZ,
        [D_MOVLTZ]=&&op_D_MOVLTZ,
        [D_MOVGEZ]=&&op_D_MOVGEZ,
        [D_L32R]=&&op_D_L32R,
        [D_JX]=&&op_D_JX,
        [D_CALLX0]=&&op_D_CALLX0,
    };
#define FETCH() do { \
    pc=C.pc; \
    if(observe && (pc==E.sym_setup || pc==E.sym_loop || (E.watch_any && watch_test(pc)))) watch_hit(pc); \
    const uint8_t *p=fetch(pc); \
    if(!p) { cpu_exception(EXC_IFETCH_PROHIB,pc,pc); E.cycles++; E.tick+=E.tpc; return; } \
    uint32_t raw=rd32le(p)&0xffffffu; \
    d=&decoded[((pc>>1)^(pc>>16))&(DECODE_SLOTS-1)]; \
    if(d->pc!=pc || (d->raw&0xffffff)!=raw) decode(d,pc,raw); \
    npc=pc+(d->raw>>24); r=d->r; s=d->s; t=d->t; imm=d->imm; \
} while(0)
#define NEXT() do { C.pc=npc; E.cycles++; E.tick+=E.tpc; if(!--budget || E.irq_req) return; FETCH(); goto *dispatch[d->op]; } while(0)
#define END() do { C.pc=npc; E.cycles++; E.tick+=E.tpc; return; } while(0)
    FETCH();
    goto *dispatch[d->op];
op_D_L32IN: op_D_L32I: { A[t]=ld32(A[s]+imm); } NEXT();
op_D_S32IN: op_D_S32I: { st32(A[s]+imm,A[t]); } if(A[s]+imm-DRAM_BASE<DRAM_SIZE || A[s]+imm-IRAM_BASE<IRAM_SIZE) NEXT(); END();
op_D_ADDN: op_D_ADD: { A[r]=A[s]+A[t]; } NEXT();
op_D_ADDIN: { A[r]=A[s]+imm; } NEXT();
op_D_MOVIN: { A[s]=imm; } NEXT();
op_D_BEQZN: op_D_BEQZ: { if(A[s]==0) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BNEZN: op_D_BNEZ: { if(A[s]!=0) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_MOVN: { A[t]=A[s]; } NEXT();
op_D_RETN: { npc=A[0]; pc_ring[pc_ring_i++&63]=pc; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_NOPN: {  } NEXT();
op_D_L8UI: { A[t]=ld8(A[s]+imm); } NEXT();
op_D_L16UI: { A[t]=ld16(A[s]+imm); } NEXT();
op_D_S8I: { st8(A[s]+imm,A[t]); } if(A[s]+imm-DRAM_BASE<DRAM_SIZE || A[s]+imm-IRAM_BASE<IRAM_SIZE) NEXT(); END();
op_D_S16I: { st16(A[s]+imm,A[t]); } if(A[s]+imm-DRAM_BASE<DRAM_SIZE || A[s]+imm-IRAM_BASE<IRAM_SIZE) NEXT(); END();
op_D_L16SI: { A[t]=sext(ld16(A[s]+imm),16); } NEXT();
op_D_MOVI: { A[t]=imm; } NEXT();
op_D_ADDI: { A[t]=A[s]+imm; } NEXT();
op_D_AND: { A[r]=A[s]&A[t]; } NEXT();
op_D_OR: { A[r]=A[s]|A[t]; } NEXT();
op_D_XOR: { A[r]=A[s]^A[t]; } NEXT();
op_D_ADDX2: { A[r]=(A[s]<<1)+A[t]; } NEXT();
op_D_ADDX4: { A[r]=(A[s]<<2)+A[t]; } NEXT();
op_D_ADDX8: { A[r]=(A[s]<<3)+A[t]; } NEXT();
op_D_SUB: { A[r]=A[s]-A[t]; } NEXT();
op_D_SUBX2: { A[r]=(A[s]<<1)-A[t]; } NEXT();
op_D_SUBX4: { A[r]=(A[s]<<2)-A[t]; } NEXT();
op_D_SUBX8: { A[r]=(A[s]<<3)-A[t]; } NEXT();
op_D_MULL: { A[r]=A[s]*A[t]; } NEXT();
op_D_J: { npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_CALL0: { A[0]=pc+3; npc=imm; pc_ring[pc_ring_i++&63]=pc; if(E.ctrace) ctrace_log(pc,npc); } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BLTZ: { if((int32_t)A[s]<0) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BGEZ: { if((int32_t)A[s]>=0) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BEQI: { if((int32_t)A[s]==B4CONST[r]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BNEI: { if((int32_t)A[s]!=B4CONST[r]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BLTI: { if((int32_t)A[s]<B4CONST[r]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BGEI: { if((int32_t)A[s]>=B4CONST[r]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BLTUI: { if(A[s]<B4CONSTU[r]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BGEUI: { if(A[s]>=B4CONSTU[r]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BNONE: { if(!(A[s]&A[t])) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BEQ: { if(A[s]==A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BLT: { if((int32_t)A[s]<(int32_t)A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BLTU: { if(A[s]<A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BALL: { if(!(~A[s]&A[t])) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BBC: { if(!(A[s]&(1u<<(A[t]&31)))) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BBCI: { if(!(A[s]&(1u<<t))) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BANY: { if(A[s]&A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BNE: { if(A[s]!=A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BGE: { if((int32_t)A[s]>=(int32_t)A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BGEU: { if(A[s]>=A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BNALL: { if(~A[s]&A[t]) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BBS: { if(A[s]&(1u<<(A[t]&31))) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_BBSI: { if(A[s]&(1u<<t)) npc=imm; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_EXTUI: { A[r]=(A[t]>>s)&imm; } NEXT();
op_D_SLLI: { A[r]=imm>=32?0:A[s]<<imm; } NEXT();
op_D_SRAI: { A[r]=(uint32_t)((int32_t)A[t]>>imm); } NEXT();
op_D_SRLI: { A[r]=A[t]>>imm; } NEXT();
op_D_SLL: { A[r]=(uint32_t)((uint64_t)A[s]<<((32-C.sar)&63)); } NEXT();
op_D_SRL: { A[r]=(uint32_t)((uint64_t)A[t]>>(C.sar&63)); } NEXT();
op_D_SRA: { A[r]=(uint32_t)((int64_t)(int32_t)A[t]>>(C.sar&63)); } NEXT();
op_D_SRC: { A[r]=(uint32_t)((((uint64_t)A[s]<<32)|A[t])>>(C.sar&63)); } NEXT();
op_D_SSR: { C.sar=A[s]&31; } NEXT();
op_D_SSL: { C.sar=32-(A[s]&31); } NEXT();
op_D_SSAI: { C.sar=imm; } NEXT();
op_D_SSA8L: { C.sar=(A[s]&3)*8; } NEXT();
op_D_SSA8B: { C.sar=32-(A[s]&3)*8; } NEXT();
op_D_MOVEQZ: { if(!A[t]) A[r]=A[s]; } NEXT();
op_D_MOVNEZ: { if(A[t]) A[r]=A[s]; } NEXT();
op_D_MOVLTZ: { if((int32_t)A[t]<0) A[r]=A[s]; } NEXT();
op_D_MOVGEZ: { if((int32_t)A[t]>=0) A[r]=A[s]; } NEXT();
op_D_L32R: { A[t]=ld32(((C.litbase&1)?(C.litbase&0xfffff000u):((pc+3)&~3u))+imm); } NEXT();
op_D_JX: { npc=A[s]; pc_ring[pc_ring_i++&63]=pc; } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_CALLX0: { npc=A[s]; A[0]=pc+3; pc_ring[pc_ring_i++&63]=pc; if(E.ctrace) ctrace_log(pc,npc); } if(npc-ROM_BASE<ROM_SIZE) END(); NEXT();
op_D_SLOW: step_slow(); E.cycles++; E.tick+=E.tpc; return;
#undef FETCH
#undef NEXT
#undef END
}

// Single-instruction entry for the focused CPU tests.
static inline void step(void)
{
    uint64_t tick=E.tick,cycles=E.cycles;
    cpu_block(1,0);
    E.tick=tick; E.cycles=cycles;
}

int cpu_run(uint32_t stop_pc)
{
    jmp_buf jb;
    jmp_buf *volatile prev = E.fault_jmp;
    E.fault_jmp = &jb;
    setjmp(jb);   // a faulting guest access lands here with the exception already raised
    for (;;) {
        if (E.tick >= E.next_event) soc_events();
        if (E.irq_req) take_irq();
        if (C.waiting) {
            if (E.next_event > E.tick) {
                uint64_t d = E.next_event - E.tick;
                E.tick = E.next_event;
                E.cycles += d / E.tpc;
            }
            continue;
        }
        uint32_t pc = C.pc;
        if (pc - ROM_BASE < ROM_SIZE) {
            if (pc == stop_pc) { E.fault_jmp = prev; return STOP_RET; }
            rom_hle(pc);
            E.cycles += 16;
            E.tick += 16 * E.tpc;
            continue;
        }
        // Execute at most 32 instructions and stop at the exact scheduled event.
        // Overdue recurring events retain the original one-instruction catch-up.
        uint64_t remaining = E.next_event > E.tick ? E.next_event - E.tick : 0;
        unsigned budget = !remaining ? 1 : remaining >= 64 ? 32 : (unsigned)((remaining + E.tpc - 1) / E.tpc);
        if (budget > 32) budget=32;
        int observe = E.sym_setup || E.sym_loop || E.watch_any;
        cpu_block(budget, observe);
    }
}
