#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/cpu.c"

static void overdue_event(uint64_t start) {
    memset(&E,0,sizeof E);
    E.tpc=2; E.cpu_mhz=80; E.flash_size=0x200000;
    soc_power_on(); cpu_reset();
    E.slice_end=UINT64_MAX; E.ccomp_tick=UINT64_MAX;
    mem_st32(0x60000600,1); mem_st32(0x60000608,0xc0);
    E.tick=start; E.cycles=start/2;
    C.ps=0; C.intenable=0; C.pc=IRAM_BASE; C.a[0]=ROM_BASE+0xa0;
    for(unsigned i=0;i<40;i++) {mem_iram[2*i]=0x3d;mem_iram[2*i+1]=0xf0;}
    mem_iram[80]=0x0d;mem_iram[81]=0xf0;
    assert(cpu_run(ROM_BASE+0xa0)==STOP_RET);
    assert(E.tick==start+82 && E.cycles==(start+82)/2 && E.next_event==86);
}

static void reset_machine(void) {
    memset(&E,0,sizeof E); E.tpc=2; E.cpu_mhz=80; E.flash_size=0x200000;
    soc_power_on(); cpu_reset(); E.slice_end=E.ccomp_tick=UINT64_MAX;
    soc_schedule(); C.ps=0;
}
static void run_guest(uint32_t pc) {
    C.pc=pc; C.a[0]=ROM_BASE+0xa0;
    assert(cpu_run(ROM_BASE+0xa0)==STOP_RET);
}
static void test_block_effects(void) {
    // Warm the cache, then change its next instruction from inside the same block.
    for(unsigned size=1;size<=4;size*=2) {
        reset_machine();
        unsigned r=size==1?4:size==2?5:6;
        uint32_t raw=2u|(2u<<4)|(3u<<8)|(r<<12);
        memcpy(mem_iram,&raw,3);
        mem_iram[3]=0x0c; mem_iram[4]=0x52; mem_iram[5]=0x0d; mem_iram[6]=0xf0;
        E.sym_loop=IRAM_BASE+3;
        run_guest(IRAM_BASE+3); assert(C.a[2]==5);
        C.a[2]=size==1?0x62:size==2?0x620c:0xf00d620c;
        C.a[3]=IRAM_BASE+(size==1?4:3);
        run_guest(IRAM_BASE);
        assert(C.a[2]==6 && E.loop_calls==2 && E.cycles==5 && E.tick==10);
    }
    reset_machine();
    mem_iram[0]=0x22; mem_iram[1]=0x23; mem_iram[2]=0;
    C.a[3]=0xdead0000; C.pc=IRAM_BASE;
    assert(cpu_run(ROM_BASE+0x30)==STOP_RET);
    assert(C.exccause==EXC_LOAD_PROHIB && C.epc[1]==IRAM_BASE);
    assert(E.cycles==0 && E.tick==0); // The original interpreter aborts this instruction.
    reset_machine(); C.pc=0xdead0000;
    assert(cpu_run(ROM_BASE+0x30)==STOP_RET && C.exccause==EXC_IFETCH_PROHIB);
    assert(E.cycles==1 && E.tick==2); // Fetch faults retain the original one-cycle cost.
}

int main(void) {
    E.cpu_mhz=80; E.tpc=2; E.next_event=UINT64_MAX;
    uint32_t seed=1, tested=0;
    for(unsigned i=0;i<250000;i++) {
        seed=seed*1664525+1013904223;
        uint32_t raw=seed&0xffffff;
        // Keep literal addresses in DRAM while still varying the destination and displacement.
        if((raw&15)==1) raw|=0xff0000;
        decoded_t d; decode(&d,IRAM_BASE+0x4000,raw);
        if(d.op==D_SLOW) continue;
        memset(&C,0,sizeof C);
        for(unsigned a=0;a<16;a++) C.a[a]=seed^(a*0x9e3779b9u);
        if(d.s<16) C.a[d.s]=DRAM_BASE+0x1000;
        C.sar=(seed>>24)&63; C.ps=PS_UM; C.litbase=(DRAM_BASE+0x10000)|1;
        C.pc=IRAM_BASE+0x4000;
        memcpy(mem_iram+0x4000,&raw,4);
        cpu_t before=C;
        step_slow();
        cpu_t expected=C;
        C=before;
        step();
        if(memcmp(&C,&expected,sizeof C)) {
            fprintf(stderr,"cached decode differs at raw=%06x op=%u pc=%08x/%08x\n",raw,d.op,C.pc,expected.pc);
            return 1;
        }
        tested++;
    }
    // Cached code must observe self-modifying IRAM immediately.
    C.litbase=0; C.pc=IRAM_BASE;
    mem_iram[0]=0x0c; mem_iram[1]=0x52; step(); assert(C.a[2]==5);
    C.pc=IRAM_BASE; mem_iram[1]=0x62; step(); assert(C.a[2]==6);
    E.flash=calloc(1,0x100040); E.flash_size=0x100040; E.flash_map_off=0;
    assert(E.flash);
    E.flash[0]=0x0c; E.flash[1]=0x72;
    E.flash[0x100000]=0x0c; E.flash[0x100001]=0x82;
    cpu_fetch_invalidate(); C.pc=FLASHMAP_BASE; step(); assert(C.a[2]==7);
    E.flash[1]=0x92; C.pc=FLASHMAP_BASE; step(); assert(C.a[2]==9);
    E.flash_map_off=0x100000; cpu_fetch_invalidate(); C.pc=FLASHMAP_BASE; step(); assert(C.a[2]==8);
    free(E.flash); E.flash=NULL;
    overdue_event(100); overdue_event(4); test_block_effects();
    printf("Predecode: %u differential instructions, IRAM/flash writes, cache remap overdue/equal-time events, observer and fault timing PASS\n",tested);
}
