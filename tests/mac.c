#include "emu.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static const uint32_t CTL=0x3ff20dc4, MASK=0x3ff20c18, STATUS=0x3ff20c20, CLEAR=0x3ff20c24, RESULT=0x3ff20c84;
static void reset_model(void) {
 E.tick=0; E.slice_end=UINT64_MAX; E.ccomp_tick=UINT64_MAX; E.flash_size=0x200000; E.tpc=2;
 soc_power_on(); E.slice_end=UINT64_MAX; E.ccomp_tick=UINT64_MAX; soc_schedule();
}
int main(void) {
 reset_model();
 mem_st32(CTL,0xc0000000); assert(E.next_event==48000);
 E.tick=47999; soc_events(); assert(mem_ld32(STATUS)==0);
 E.tick=48000; soc_events(); assert(mem_ld32(STATUS)==0xc0000); assert((E.irq_lines&1)==0);
 assert((mem_ld32(CTL)&0xc0000000)==0);
 mem_st32(MASK,0xc0000); assert(E.irq_lines&1);
 mem_st32(CLEAR,0x40000); assert(mem_ld32(STATUS)==0x80000); assert(E.irq_lines&1);
 mem_st32(CLEAR,0x80000); assert(mem_ld32(STATUS)==0); assert((E.irq_lines&1)==0);
 /* Every hardware queue index is encoded in the sole result register. A
    disabled interrupt mask must not prevent completion or acknowledgment. */
 for (unsigned q=0; q<8; q++) {
  reset_model();
  mem_st32(CTL-24*q,0xc0000000); E.tick=48000; soc_events();
  assert(mem_ld32(RESULT)==q<<12); assert(mem_ld32(STATUS)==0xc0000);
  assert((E.irq_lines&1)==0);
  mem_st32(CLEAR,0); mem_st32(CLEAR,1); assert(mem_ld32(STATUS)==0xc0000);
  mem_st32(CLEAR,0xc0000); assert(mem_ld32(STATUS)==0); assert((E.irq_lines&1)==0);
 }
 /* Mask updates recompute a level IRQ; W1C retains status bits that were not
    written. A pending masked done bit can later reassert the interrupt. */
 reset_model(); mem_st32(MASK,0x40000); mem_st32(CTL,0xc0000000);
 E.tick=48000; soc_events(); assert(E.irq_lines&1);
 mem_st32(CLEAR,0x40000); assert(mem_ld32(STATUS)==0x80000); assert((E.irq_lines&1)==0);
 mem_st32(MASK,0x80000); assert(E.irq_lines&1);
 mem_st32(MASK,0); assert((E.irq_lines&1)==0); assert(mem_ld32(STATUS)==0x80000);
 mem_st32(CLEAR,0x80000); mem_st32(MASK,0xc0000); assert((E.irq_lines&1)==0);
 reset_model();
 mem_st32(CTL,0xc0000000); mem_st32(CTL-24,0xc0000000);
 E.tick=48000; soc_events(); assert(mem_ld32(RESULT)==0); assert(E.next_event==UINT64_MAX);
 mem_st32(CLEAR,0xc0000); assert(E.next_event==48000); soc_events(); assert(mem_ld32(RESULT)==0x1000);
 reset_model();
 mem_st32(CTL,0xc0000000); mem_st32(CTL,0); E.tick=48000; soc_events(); assert(mem_ld32(STATUS)==0);
 for (unsigned ack=1; ack<8; ack++) {
  reset_model();
  mem_st32(CTL,0xc0000000|(ack<<24)); E.tick=48000; soc_events(); assert(mem_ld32(STATUS)==0);
 }
 reset_model();
 mem_st32(CTL,0xc0000000); E.tick=2000; mem_st32(CTL,0xc0000000); assert(E.next_event==48000);
 soc_reset(false); E.tick=48000; soc_events(); assert(mem_ld32(STATUS)==0);
 /* The SDK's next-beacon calculation requires a running 64-bit TSF clock. */
 reset_model();
 assert(mem_ld32(0x3ff21048)==0 && mem_ld32(0x3ff2104c)==0);
 E.tick=160000; assert(mem_ld32(0x3ff21048)==1000);
 E.tick=0xffffffffull*160;
 assert(mem_ld32(0x3ff21048)==0xffffffff && mem_ld32(0x3ff2104c)==0);
 E.tick=0x100000001ull*160;
 assert(mem_ld32(0x3ff21048)==1 && mem_ld32(0x3ff2104c)==1);
 cpu_set_freq(160);
 E.tick+=160; assert(mem_ld32(0x3ff21048)==2);
 puts("MAC TX register tests passed");
}
