// ISA encodings are independent test inputs; reference: Xtensa ISA and QEMU Xtensa.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/cpu.c"
#include "../src/rom.c"

static void emit(uint32_t addr, uint32_t op, unsigned len) {
    for (unsigned i=0; i<len; ++i) mem_iram[addr-IRAM_BASE+i]=(uint8_t)(op>>(i*8));
}
static void instruction(uint32_t op) {
    C.pc=IRAM_BASE+0x100;
    emit(C.pc,op,3);
    step();
}
static void test_shifts(void) {
    uint32_t inputs[]={0,1,0x80000001,0xffffffff,0x12345678};
    for (unsigned i=0;i<sizeof inputs/sizeof *inputs;i++) for (unsigned sar=0;sar<64;sar++) {
        C.sar=sar; C.a[2]=inputs[i]; C.a[4]=~inputs[i];
        instruction(0xa13200); // sll a3,a2
        assert(C.a[3]==(uint32_t)((uint64_t)inputs[i]<<((32-sar)&63)));
        instruction(0x913020); // srl a3,a2
        assert(C.a[3]==(uint32_t)((uint64_t)inputs[i]>>sar));
        instruction(0xb13020); // sra a3,a2
        assert(C.a[3]==(uint32_t)((int64_t)(int32_t)inputs[i]>>sar));
        instruction(0x813420); // src a3,a4,a2
        assert(C.a[3]==(uint32_t)((((uint64_t)~inputs[i]<<32)|inputs[i])>>sar));
    }
    for(unsigned sar=0;sar<=32;sar+=32) for(unsigned sh=0;sh<32;sh++) {
        C.sar=sar; C.a[2]=0x81234567;
        instruction(0x013200 | ((32-sh)&15)<<4 | (((32-sh)>>4)&1)<<20);
        assert(C.a[3]==(sh ? 0x81234567u<<sh : 0)); // slli encoded 0 is shift 32
        instruction(0x213020 | (sh&15)<<8 | (sh>>4)<<20);
        assert(C.a[3]==(uint32_t)((int32_t)0x81234567>>sh));
        if(sh<16) {
            instruction(0x413020 | sh<<8);
            assert(C.a[3]==0x81234567u>>sh);
        }
    }
    C.a[2]=0x80000000;
    instruction(0x603020); assert(C.a[3]==0x80000000); // neg
    instruction(0x603120); assert(C.a[3]==0x80000000); // abs
}
static void test_immediates(void) {
    for(unsigned bit=0;bit<32;bit++) for(unsigned set=0;set<2;set++) {
        C.a[2]=set ? 1u<<bit : ~(1u<<bit);
        instruction(0x086207 | ((bit&15)<<4) | ((bit>>4)<<12));
        assert(C.pc==IRAM_BASE+0x100+(set?3:12));
        instruction(0x08e207 | ((bit&15)<<4) | ((bit>>4)<<12));
        assert(C.pc==IRAM_BASE+0x100+(set?12:3));
    }
    for(unsigned imm=0;imm<128;imm++) {
        instruction(0x020c | ((imm>>4)<<4) | ((imm&15)<<12));
        assert(C.a[2]==(uint32_t)(imm>=96?(int)imm-128:(int)imm));
    }
    for(unsigned imm=0;imm<64;imm++) for(unsigned ne=0;ne<2;ne++) for(unsigned v=0;v<2;v++) {
        C.a[2]=v;
        instruction(0x028c | (ne<<6) | ((imm>>4)<<4) | ((imm&15)<<12));
        assert(C.pc==IRAM_BASE+0x100+((ne==v)?4+imm:2));
    }
    // Check PC alignment and signed 18-bit displacement near both L32R extremes.
    for(unsigned low=0;low<4;low++) {
        C.pc=IRAM_BASE+0x100+low; C.litbase=0;
        uint32_t addr=(C.pc+3)&~3u;
        hle_wr32(addr-8,0xdeadbeef);
        emit(C.pc,0xfffe21,3); step();
        assert(C.a[2]==0xdeadbeef);
    }
    C.litbase=(DRAM_BASE+0x2000)|1;
    hle_wr32(DRAM_BASE+0x1ffc,0x76543210);
    instruction(0xffff21);
    assert(C.a[2]==0x76543210);
    C.litbase=0;
}
static void test_exceptions(void) {
    for(unsigned user=0;user<2;user++) {
        C.ps=user?PS_UM:0; C.vecbase=IRAM_BASE; C.waiting=1;
        cpu_exception(EXC_SYSCALL,0,IRAM_BASE+0x500);
        assert(C.epc[1]==IRAM_BASE+0x500 && C.exccause==EXC_SYSCALL);
        assert(C.pc==IRAM_BASE+(user?0x50:0x30) && (C.ps&PS_EXCM) && !C.waiting);
        cpu_exception(EXC_LOAD_PROHIB,0xdeadbeef,IRAM_BASE+0x600);
        assert(C.depc==IRAM_BASE+0x600 && C.epc[1]==IRAM_BASE+0x500);
        assert(C.pc==IRAM_BASE+0x70 && C.excvaddr==0xdeadbeef);
    }
    C.ps=PS_UM; C.pc=IRAM_BASE+0x700; C.intenable=1u<<INUM_FRC2; C.intpend=C.intenable; irq_update();
    assert(take_irq() && C.epc[1]==IRAM_BASE+0x700 && C.exccause==EXC_LEVEL1_INT && (C.ps&PS_EXCM));
    assert(!take_irq());
    instruction(0x003000); assert(C.pc==IRAM_BASE+0x700 && C.ps==PS_UM); // rfe
    C.intpend=0; irq_update();
    for(unsigned level=0;level<16;level++) {
        C.ps=PS_UM|level; C.pc=IRAM_BASE+0x723; C.intenable=0;
        irq_raise_edge(INUM_NMI);
        assert(take_irq() && C.epc[3]==IRAM_BASE+0x723 && C.eps[3]==(PS_UM|level));
        assert(C.pc==IRAM_BASE+0x20 && C.ps==(PS_UM|PS_EXCM|3) && !C.intpend);
        instruction(0x003310); assert(C.ps==(PS_UM|level) && C.pc==IRAM_BASE+0x723); // rfi 3
    }
    C.ps=PS_UM|2;
    instruction(0x006f20); assert(C.a[2]==(PS_UM|2) && C.ps==(PS_UM|15)); // rsil a2,15
    instruction(0x007000); assert(C.waiting && C.ps==PS_UM && C.pc==IRAM_BASE+0x103); // waiti 0
    C.intenable=1u<<INUM_FRC2; irq_raise_edge(INUM_FRC2);
    assert(take_irq() && !C.waiting && C.epc[1]==IRAM_BASE+0x103);
    C.intpend=0; C.intenable=0; irq_update();
}
static void test_frames(void) {
    uint32_t frame=DRAM_BASE+0x10000, output=DRAM_BASE+0x200;
    uint32_t original[16];
    for(unsigned i=0;i<16;i++) original[i]=C.a[i]=0x32100000+i;
    original[1]=frame+256; C.a[1]=frame;
    for(unsigned i=2;i<=4;i++) hle_wr32(frame+12+i*4,original[i]);
    C.epc[1]=IRAM_BASE+0x800; C.sar=17; C.ps=PS_UM|PS_EXCM; C.exccause=EXC_LEVEL1_INT;
    uint32_t handler=IRAM_BASE+0x1000;
    emit(handler,0x03e640,3); // rsr a4,ps
    emit(handler+3,0x006242,3); // s32i a4,a2,0
    emit(handler+6,0x03e240,3); // rsr a4,interrupt
    emit(handler+9,0x016242,3); // s32i a4,a2,4
    emit(handler+12,0x00f00d,2); // ret.n
    isr[INUM_FRC2].fn=handler; isr[INUM_FRC2].arg=output;
    C.intenable=1u<<INUM_FRC2; C.intpend=C.intenable; irq_update();
    xtos_l1int();
    assert(hle_rd32(output)==(PS_UM|1));
    assert(!(hle_rd32(output+4)&(1u<<INUM_FRC2))); // edge cleared before handler
    assert(!memcmp(C.a,original,sizeof original) && C.sar==17 && C.ps==PS_UM);
    assert(hle_rd32(frame)==IRAM_BASE+0x800 && hle_rd32(frame+76)==EXC_LEVEL1_INT);
    assert(hle_rd32(frame+16)==original[0] && hle_rd32(frame+72)==original[15]);
    // The C handler edits frame.epc and frame.a15, which must survive the wrapper return.
    C.a[1]=frame; C.ps=PS_UM|PS_EXCM; C.exccause=EXC_ILLEGAL;
    for(unsigned i=2;i<=4;i++) hle_wr32(frame+12+i*4,original[i]);
    emit(handler,0x7ba042,3); // movi a4,123
    emit(handler+3,0x006242,3); // s32i a4,a2,0
    emit(handler+6,0x126242,3); // s32i a4,a2,72
    emit(handler+9,0x00f00d,2);
    c_handler[EXC_ILLEGAL]=handler;
    xtos_c_wrapper();
    assert(C.pc==123 && C.a[15]==123 && C.a[1]==frame+256 && C.ps==PS_UM);
}
static void test_scheduler(void) {
    memset(task,0,sizeof task); idle_called=0;
    run_sp=DRAM_BASE+0x16000; idle_cb=IRAM_BASE+0x1200; idle_arg=0x42;
    task[1].fn=IRAM_BASE+0x1300; task[1].queue=DRAM_BASE+0x8000; task[1].qlen=2;
    task[12].fn=IRAM_BASE+0x1400; task[12].queue=DRAM_BASE+0x8100; task[12].qlen=2;
    C.a[2]=1; C.a[3]=0x11; C.a[4]=0x22; r_ets_post(); assert(C.a[2]==0);
    C.a[2]=12; C.a[3]=0x33; C.a[4]=0x44; r_ets_post(); assert(C.a[2]==0);
    ets_run_loop(); assert(C.pc==task[12].fn && C.a[2]==task[12].queue);
    assert(hle_rd32(C.a[2])==0x33 && hle_rd32(C.a[2]+4)==0x44);
    ets_run_loop(); assert(C.pc==task[1].fn && C.a[2]==task[1].queue);
    ets_run_loop(); assert(C.pc==idle_cb && C.a[2]==idle_arg);
    ets_run_loop(); assert(C.waiting && C.pc==ETS_RUN_LOOP && C.ps==PS_UM);
    C.waiting=0; // resumed after an interrupt that posted a task
    C.a[2]=1; C.a[3]=0x55; C.a[4]=0x66; r_ets_post();
    ets_run_loop(); assert(C.pc==task[1].fn && hle_rd32(C.a[2])==0x55);
    ets_run_loop(); assert(C.pc==idle_cb);
    memset(task,0,sizeof task); idle_cb=0; idle_called=0;
}
static void test_arguments(void) {
    uint32_t stack=DRAM_BASE+0x4000, regs=DRAM_BASE+0x3000;
    for(unsigned i=0;i<16;i++) hle_wr32(stack+4*i,0x100+i);
    argp_t ap={.nregs=5,.stk=stack};
    for(unsigned i=0;i<5;i++) ap.regs[i]=0x201+i;
    assert(arg_next64(&ap)==0x0000020300000202ull); // a4,a5
    assert(arg_next(&ap)==0x204); // a6
    assert(arg_next64(&ap)==0x0000010100000100ull); // cannot split a7 and stack
    assert(arg_next(&ap)==0x102);
    assert(arg_next64(&ap)==0x0000010500000104ull); // 8-byte stack alignment
    for(unsigned i=0;i<6;i++) hle_wr32(regs+4*i,0x200+i);
    ap=(argp_t){.va=1,.va_reg=regs,.va_stk=stack-32,.va_ndx=4};
    assert(arg_next64(&ap)==0x0000020300000202ull);
    assert(arg_next(&ap)==0x204);
    assert(arg_next64(&ap)==0x0000010100000100ull);
    assert(arg_next(&ap)==0x102);
    assert(arg_next64(&ap)==0x0000010500000104ull);
}
static void test_cache_crypto(void) {
    C.a[2]=1; C.a[3]=0; r_cache_read_enable(); assert(E.flash_map_off==0x100000);
    C.a[2]=0; C.a[3]=1; r_cache_read_enable(); assert(E.flash_map_off==0x200000);
    C.a[2]=1; C.a[3]=1; r_cache_read_enable(); assert(E.flash_map_off==0x300000);
    uint8_t out[20], key[20]; memset(key,0x0b,20);
    md5_ctx m; md5_init(&m); md5_update(&m,(const uint8_t *)"a",1); md5_update(&m,(const uint8_t *)"bc",2); md5_final(out,&m);
    assert(!memcmp(out,"\x90\x01\x50\x98\x3c\xd2\x4f\xb0\xd6\x96\x3f\x7d\x28\xe1\x7f\x72",16));
    const uint8_t *a[]={(const uint8_t *)"Hi There"}; size_t l[]={8};
    hmac_sha1_vec(key,20,1,a,l,out);
    assert(!memcmp(out,"\xb6\x17\x31\x86\x55\x05\x72\x64\xe2\x8b\xc0\xb6\xfb\x37\x8c\x8e\xf1\x46\xbe\x00",20));
    hmac_md5_buf(key,16,a[0],8,out);
    assert(!memcmp(out,"\x92\x94\x72\x7a\x36\x38\xbb\x1c\x13\xf4\x8e\xf8\x15\x8b\xfc\x9d",16));
    pbkdf2_sha1_buf("password",(const uint8_t *)"salt",4,2,out,20);
    assert(!memcmp(out,"\xea\x6c\x01\x4d\xc7\x2d\x6f\x8c\xcd\x1e\xd9\x2a\xce\x1d\x41\xf0\xd8\xde\x89\x57",20));
    memcpy(out,"Plaintext",9); rc4_skip_buf((const uint8_t *)"Key",3,0,out,9);
    assert(!memcmp(out,"\xbb\xf3\x16\xe8\xd9\x40\xaf\x0a\xd3",9));
    // FIPS 197 (2001), Figure 7, is an independent oracle for every S-box entry.
    static const char sbox_hex[] =
        "637c777bf26b6fc53001672bfed7ab76" "ca82c97dfa5947f0add4a2af9ca472c0"
        "b7fd9326363ff7cc34a5e5f171d83115" "04c723c31896059a071280e2eb27b275"
        "09832c1a1b6e5aa0523bd6b329e32f84" "53d100ed20fcb15b6acbbe394a4c58cf"
        "d0efaafb434d338545f9027f503c9fa8" "51a3408f929d38f5bcb6da2110fff3d2"
        "cd0c13ec5f974417c4a77e3d645d1973" "60814fdc222a908846eeb814de5e0bdb"
        "e0323a0a4906245cc2d3ac629195e479" "e7c8376d8dd54ea96c56f4ea657aae08"
        "ba78252e1ca6b4c6e8dd741f4bbd8b8a" "703eb5664803f60e613557b986c11d9e"
        "e1f8981169d98e949b1e87e9ce5528df" "8ca1890dbfe6426841992d0fb054bb16";
    static const uint8_t round_constants[] = {1,2,4,8,16,32,64,128,0x1b,0x36};
    build_aes_tables();
    assert(sizeof sbox_hex == 513);
    assert(aes_inverse(0)==0);
    for (unsigned i=0;i<256;i++) {
        unsigned expected;
        assert(sscanf(sbox_hex+2*i,"%2x",&expected)==1);
        assert(aes_sub_byte((uint8_t)i)==expected);
        if (i) assert(gmul((uint8_t)i,aes_inverse((uint8_t)i))==1);
        unsigned doubled=((expected<<1)^((expected&0x80)?0x11b:0))&255;
        uint32_t table=(doubled<<24)|(expected<<16)|(expected<<8)|(doubled^expected);
        assert(hle_rd32(TE0_TABLE+4*i)==table);
    }
    for (unsigned i=0;i<sizeof round_constants;i++)
        assert(hle_rd32(RCONS_TABLE+4*i)==(uint32_t)round_constants[i]<<24);
}
static void test_rom_bounds(void) {
    uint8_t flash[32]; memset(flash,0xa5,sizeof flash);
    E.flash=flash; E.flash_size=sizeof flash;
    mem_dram[0]=0x5a;
    C.a[2]=UINT32_MAX; C.a[3]=DRAM_BASE; C.a[4]=2; r_spi_read();
    assert(C.a[2]==1 && mem_dram[0]==0x5a);
    C.a[2]=UINT32_MAX; C.a[3]=DRAM_BASE; C.a[4]=2; r_spi_write();
    assert(C.a[2]==1 && flash[0]==0xa5);
    C.a[2]=0x100000; r_spi_erase_sector(); assert(C.a[2]==1 && flash[0]==0xa5);
    C.a[2]=0x10000; r_spi_erase_block(); assert(C.a[2]==1 && flash[0]==0xa5);
    memcpy(mem_dram,"%.100f",7);
    argp_t ap={.nregs=5};
    assert(format(ADDR_ETS_UART_PUTC1,DRAM_BASE,&ap)==79);
    flash[0]=0xe9; flash[1]=1;
    uint32_t addr=DRAM_BASE, len=0xfffffff0;
    memcpy(flash+8,&addr,4); memcpy(flash+12,&len,4);
    if (!setjmp(E.reset_jmp)) {
        rom_boot();
        assert(!"overflowing ROM segment must halt");
    }
    E.flash=NULL; E.flash_size=0;
}
int main(void) {
    E.cpu_mhz=80; E.tpc=2; E.next_event=UINT64_MAX;
    rom_init();
    test_shifts(); test_immediates(); test_exceptions(); test_frames(); test_scheduler(); test_arguments(); test_cache_crypto(); test_rom_bounds();
    puts("CPU/ROM: shifts, immediates, exception/NMI entry, RSIL/WAITI, frames, ISR state, varargs, cache maps and crypto vectors PASS");
}
