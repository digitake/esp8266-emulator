// Synthetic guest/backend checks for the public network adapter API.
// These test ownership and scheduling; they do not establish firmware support.
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include "emu.h"
#include "net.h"
#include "net_profile.h"
#include "host_net.h"

enum symbol { S_TX, S_RX_ALLOC, S_RX_INPUT, S_FREE, S_UPDATE, S_UPDOWN, S_DHCP_START, S_DHCP_STOP, S_IFUPDOWN, S_LWIP_INIT, S_CONNECT, S_DISCONNECT, S_STATUS, S_IP_GET, S_IP_SET, S_EVENT_CB, S_DHCPC_START, S_DHCPC_STOP, S_DHCPC_STATUS, S_RSSI, S_MAC_GET, S_CONFIG_GET, S_CONFIG_SET, S_CONFIG_CURRENT, S_CHANNEL, S_MODE_SET, S_MODE_CURRENT, S_MODE_GET, S_COUNT };
static const char *const names[S_COUNT] = {
 "glue2esp_linkoutput", "esp2glue_alloc_for_recv", "esp2glue_ethernet_input", "esp2glue_pbuf_freed", "esp2glue_netif_update", "esp2glue_netif_set_up1down0", "esp2glue_dhcp_start", "esp2glue_dhcp_stop", "glue2esp_ifupdown", "esp2glue_lwip_init", "wifi_station_connect", "wifi_station_disconnect", "wifi_station_get_connect_status", "wifi_get_ip_info", "wifi_set_ip_info", "wifi_set_event_handler_cb", "wifi_station_dhcpc_start", "wifi_station_dhcpc_stop", "wifi_station_dhcpc_status", "wifi_station_get_rssi", "wifi_get_macaddr", "wifi_station_get_config", "wifi_station_set_config", "wifi_station_set_config_current", "wifi_get_channel", "wifi_set_opmode", "wifi_set_opmode_current", "wifi_get_opmode"
};
static uint32_t symbols[S_COUNT];
static bool ram(uint32_t a, uint32_t size) { return a >= DRAM_BASE && size <= DRAM_SIZE && a - DRAM_BASE <= DRAM_SIZE - size; }
emu_t E;
cpu_t C;
static uint8_t memory[DRAM_SIZE];
static unsigned sends, frees, inputs, events_seen, updates;
static unsigned backend_resets, mode_reads, dhcp_stops, link_downs, address_clears, last_event;
static uint32_t sdk_mode = 1;
static bool fail_send, fail_alloc;
static uint64_t backend_generation;
static bool expect_halt;
static jmp_buf halt_return;
void hle_read(uint32_t a, void *d, uint32_t nbytes) { assert(ram(a,nbytes)); memcpy(d,memory+a-DRAM_BASE,nbytes); }
void hle_write(uint32_t a,const void *d,uint32_t nbytes) { assert(ram(a,nbytes)); memcpy(memory+a-DRAM_BASE,d,nbytes); }
uint32_t hle_rd32(uint32_t a) { uint32_t x; hle_read(a,&x,4); return x; }
void hle_wr32(uint32_t a,uint32_t x) { hle_write(a,&x,4); }
void ev_log(const char *f,...) {(void)f;}
void emu_halt(const char *f,...) {(void)f;if(expect_halt)longjmp(halt_return,1);abort();}
static bool send_packet(void *ctx,const uint8_t *f,size_t size) {
 (void)ctx; assert(size==60 && f[0]==0xAA); if(fail_send)return false; sends++;return true;
}
static void reset_backend(void *ctx,uint64_t gen) {(void)ctx;backend_generation=gen;backend_resets++;}
net_backend_t host_net_backend(void) { return (net_backend_t){.send=send_packet,.reset=reset_backend}; }
uint32_t guest_call(uint32_t fn,int argc,const uint32_t *a) {
 if(fn==symbols[S_MAC_GET]) {uint8_t mac[6]={2,3,4,5,6,7}; assert(argc==2);hle_write(a[1],mac,6);return 1;}
 if(fn==symbols[S_CONFIG_GET]) {uint8_t config[112]={0};memcpy(config,"test",4);hle_write(a[0],config,112);return 1;}
 if(fn==symbols[S_UPDATE]) {assert(argc==7 && a[6]==1500);updates++;if(!a[1] && !a[4])address_clears++;return 0;}
 if(fn==symbols[S_MODE_GET]) {assert(argc==0);mode_reads++;return sdk_mode;}
 if(fn==symbols[S_DHCP_STOP]) {dhcp_stops++;return 0;}
 if(fn==symbols[S_UPDOWN]) {if(!a[1])link_downs++;return 0;}
 if(fn==symbols[S_FREE]) {assert(a[0]==DRAM_BASE+512);frees++;return 0;}
 if(fn==symbols[S_RX_ALLOC]) {if(!fail_alloc){hle_wr32(a[1],DRAM_BASE+1024);hle_wr32(a[2],DRAM_BASE+1100);}return 0;}
 if(fn==symbols[S_RX_INPUT]) {assert(a[0]==0 && a[1]==DRAM_BASE+1024);inputs++;return 0;}
 if(fn==0x402ffff0) {events_seen++;last_event=hle_rd32(a[0]);return 0;}
 return 0;
}
static uint32_t hook(enum symbol s,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
 C.a[0]=0x402ff000;C.a[2]=a;C.a[3]=b;C.a[4]=c;C.a[5]=d;
 assert(net_hook(symbols[s]));assert(C.pc==C.a[0]);return C.a[2];
}
static void mode_begin(enum symbol s,uint32_t requested,uint32_t return_pc) {
 C.a[0]=return_pc;C.a[2]=requested;
 assert(!net_hook(symbols[s])); // SDK setter body must execute unchanged
}
static void mode_lifecycle(void) {
 net_reset(3);sdk_mode=1;assert(!net_hook(symbols[S_LWIP_INIT]));
 hook(S_EVENT_CB,0x402ffff0,0,0,0);hook(S_CONNECT,0,0,0,0);net_service_guest();
 hook(S_IFUPDOWN,0,0x0f02000a,0x00ffffff,0x0202000a);net_service_guest();
 assert(net_get_stats().connected && net_get_stats().ip==0x0f02000a);
 const uint32_t ret=0x402ef000;uint32_t sp=C.a[1];
 unsigned reads=mode_reads,resets=backend_resets;
 mode_begin(S_MODE_SET,0,ret);sdk_mode=0;
 net_service_guest();assert(mode_reads==reads && net_get_stats().connected);
 // Visiting the same code from a different stack does not finish the setter.
 C.a[1]=sp-16;assert(!net_hook(ret));net_service_guest();assert(mode_reads==reads);C.a[1]=sp;
 // The setter failed and restored its old mode: requested OFF is not evidence.
 sdk_mode=1;C.a[2]=0;assert(!net_hook(ret));net_service_guest();
 assert(mode_reads==reads+1 && net_get_stats().connected && backend_resets==resets);
 uint8_t frame[60];memset(frame,0xaa,sizeof frame);hle_write(DRAM_BASE+2048,frame,60);
 assert(hook(S_TX,0,DRAM_BASE+512,DRAM_BASE+2048,60)==0);assert(net_receive(3,frame,60));
 unsigned old_frees=frees,old_inputs=inputs,stops=dhcp_stops,downs=link_downs,clears=address_clears;
 mode_begin(S_MODE_CURRENT,2,ret+16);sdk_mode=2;C.a[2]=1;assert(!net_hook(ret+16));net_service_guest();
 net_stats_t state=net_get_stats();assert(!state.connected && !state.ip && !state.rx_pending && !state.tx_pending);
 assert(frees==old_frees+1 && inputs==old_inputs && backend_resets==resets+1);
 assert(dhcp_stops==stops+1 && link_downs==downs+1 && address_clears==clears+1 && last_event==1);
 // An explicit connect cannot claim a station connection in AP-only/OFF mode.
 hook(S_CONNECT,0,0,0,0);net_service_guest();assert(!net_get_stats().connected);
 assert(backend_resets==resets+1);
 // A wake-like mode restore still waits for the real setter's completion.
 mode_begin(S_MODE_SET,3,ret+32);sdk_mode=3;hook(S_CONNECT,0,0,0,0);net_service_guest();assert(!net_get_stats().connected);
 assert(!net_hook(ret+32));net_service_guest();assert(net_get_stats().connected && !net_get_stats().ip && last_event==0);
 // Reset invalidates any pending return observer rather than blocking the new boot.
 mode_begin(S_MODE_CURRENT,0,ret+48);net_reset(4);sdk_mode=1;
 assert(!net_hook(symbols[S_LWIP_INIT]));hook(S_CONNECT,0,0,0,0);net_service_guest();assert(net_get_stats().connected);
}
static void write_bytes(const char *path,const void *bytes,size_t size) {
 FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,size,f)==size);assert(!fclose(f));
}
static void image_section(FILE *f,const char *path,uint32_t offset,bool all) {
 fprintf(f,"image %s\n",path);
 for(unsigned i=0;i<(all?S_COUNT:1);i++)fprintf(f,"symbol %s 0x%x\n",names[i],symbols[i]+offset);
 fputs("end\n",f);
}
static void profile_rejected(void) {
 expect_halt=true;
 if(!setjmp(halt_return)){net_profile_app();assert(!"unknown application was accepted");}
 expect_halt=false;
 assert(!net_is_enabled() && !net_profile_match().active);
}
static void profile_lifecycle(void) {
 char dir[]="/tmp/esp-net-profile-XXXXXX";assert(mkdtemp(dir));
 char a[256],b[256],profile[256];snprintf(a,sizeof a,"%s/old image.bin",dir);snprintf(b,sizeof b,"%s/new image.bin",dir);snprintf(profile,sizeof profile,"%s/network.txt",dir);
 uint8_t old[16]={0xe9,1},next[16]={0xe9,2},flash[4096];
 write_bytes(a,old,sizeof old);write_bytes(b,next,sizeof next);
 FILE *f=fopen(profile,"w");assert(f);fputs("esp8266-net-v1 arduino-3.1.2-lwip2\n# provenance is documentary\n\n",f);image_section(f,a,0,true);image_section(f,b,0x10000,true);assert(!fclose(f));
 char error[256];assert(net_profile_load(profile,error,sizeof error));assert(net_profile_requested());
 // The application must match the bytes cached at load, not a changed disk file.
 write_bytes(a,next,sizeof next);
 memset(flash,0xff,sizeof flash);memcpy(flash,old,sizeof old);E.flash=flash;E.flash_size=sizeof flash;E.boot_count=10;
 net_profile_reset(10);assert(!E.entry_hook && !E.guest_service && !net_is_enabled());
 net_profile_app();assert(E.entry_hook==net_hook && E.guest_service==net_service_guest);assert(backend_generation==10);
 assert(net_profile_match().active && net_profile_match().image_index==1 && !net_profile_match().mode_normalized);
 // SDK configuration beyond the BIN prefix does not invalidate the image.
 flash[300]=0x42;net_profile_reset(11);E.boot_count=11;net_profile_app();assert(net_is_enabled());
 // An OTA restart leaves hooks disabled until the new application is selected.
 net_profile_reset(12);E.boot_count=12;assert(!net_is_enabled() && !E.entry_hook);
 memcpy(flash,next,sizeof next);net_profile_app();assert(backend_generation==12);
 assert(!net_hook(symbols[S_CONNECT]));C.a[0]=FLASHMAP_BASE+0xff000;assert(net_hook(symbols[S_CONNECT]+0x10000));
 // Raw Updater preserves the old flash mode. A cold process must also bind
 // that persisted image, with all non-mode bytes checked and no source edits.
 for(unsigned mode=0;mode<4;mode++) {
  net_profile_reset(13);E.boot_count=13;assert(!net_profile_match().active);
  flash[2]=mode;net_profile_app();net_profile_match_t match=net_profile_match();
  assert(match.active && match.image_index==2 && match.image_mode==0 && match.flash_mode==mode);
  assert(match.mode_normalized==(mode!=0));
 }
 net_profile_close();assert(!net_profile_match().active);
 // Restore the original on-disk first image, then simulate a new process.
 write_bytes(a,old,sizeof old);assert(net_profile_load(profile,error,sizeof error));
 net_profile_reset(13);net_profile_app();assert(net_profile_match().mode_normalized);
 // Every other byte remains binding, including header flags and flash size.
 for(unsigned byte=0;byte<sizeof next;byte++) {
  if(byte==2)continue;
  net_profile_reset(13);flash[byte]^=0x80;
  profile_rejected();
  assert(!net_is_enabled() && !net_profile_match().active);flash[byte]^=0x80;
 }
 for(unsigned mode=4;mode<=255;mode++) {
  net_profile_reset(13);flash[2]=mode;
  profile_rejected();
 }
 flash[2]=0;
 net_profile_reset(13);E.boot_count=13;flash[8]^=1;
 expect_halt=true;if(!setjmp(halt_return)){net_profile_app();assert(!"unknown application was accepted");}expect_halt=false;
 assert(!net_is_enabled());net_profile_close();assert(!net_profile_requested());
 // Duplicate image bytes cannot silently pick the first ELF/symbol profile.
 write_bytes(a,next,sizeof next);
 f=fopen(profile,"w");assert(f);fputs("esp8266-net-v1 arduino-3.1.2-lwip2\n",f);image_section(f,a,0,true);image_section(f,b,0x10000,true);assert(!fclose(f));
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"ambiguous"));
 // Even an exact match is unsafe if another ELF profile differs only in mode.
 next[2]=3;write_bytes(b,next,sizeof next);
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"ambiguous"));
 // Reject ambiguous prefixes with different BIN lengths too.
 uint8_t longer[32]={0};memcpy(longer,next,sizeof next);write_bytes(b,longer,sizeof longer);
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"ambiguous"));
 next[2]=4;write_bytes(b,next,sizeof next);
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"flash mode"));
 // Missing required symbols fail before booting.
 f=fopen(profile,"w");assert(f);fputs("esp8266-net-v1 arduino-3.1.2-lwip2\n",f);image_section(f,a,0,false);assert(!fclose(f));
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"missing"));
 f=fopen(profile,"w");assert(f);fprintf(f,"esp8266-net-v1 arduino-3.1.2-lwip2\nimage %s\nsymbol %s 0x40200000\nsymbol %s 0x40200000\nend\n",a,names[0],names[1]);assert(!fclose(f));
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"duplicate"));
 assert(!net_profile_requested() && !net_is_enabled());
 net_profile_close();E.flash=NULL;E.flash_size=0;
 assert(!unlink(a));assert(!unlink(b));assert(!unlink(profile));assert(!rmdir(dir));
}
int main(void) {
 net_clear_symbols();for(unsigned i=0;i<S_COUNT;i++) { symbols[i] = FLASHMAP_BASE+16*i; assert(net_set_symbol(names[i],symbols[i])); }
 assert(!net_set_symbol("unknown",FLASHMAP_BASE));assert(!net_set_symbol(names[0],0x60000000));
 net_backend_t b={.send=send_packet,.reset=reset_backend};char error[128];assert(net_enable(&b,error,sizeof error));
 net_reset(1);assert(backend_generation==1);C.a[1]=DRAM_BASE+DRAM_SIZE-16;C.ps=PS_UM;
 memset(memory,0xCC,sizeof memory);uint8_t stack[256];memcpy(stack,memory+DRAM_SIZE-272,256);
 assert(!net_hook(symbols[S_LWIP_INIT]));hook(S_EVENT_CB,0x402ffff0,0,0,0);hook(S_CONNECT,0,0,0,0);
 net_service_guest();assert(net_get_stats().connected && updates==1 && events_seen==1);
 assert(!memcmp(stack,memory+DRAM_SIZE-272,256));
 assert(hook(S_STATUS,0,0,0,0)==1); // association alone does not mean GOT_IP
 hook(S_IFUPDOWN,0,0x0f02000a,0x00ffffff,0x0202000a);
 assert(hook(S_STATUS,0,0,0,0)==5);
 assert(hook(S_IP_GET,0,DRAM_BASE+400,0,0)==1);
 assert(hle_rd32(DRAM_BASE+400)==0x0f02000a);
 net_service_guest();assert(events_seen==2);
 hook(S_CONNECT,0,0,0,0);hook(S_DHCPC_START,0,0,0,0);net_service_guest();assert(updates==1);
 // A late radio-side netif installation must not discard a real DHCP lease.
 hle_wr32(DRAM_BASE+400,0);hle_wr32(DRAM_BASE+404,0);hle_wr32(DRAM_BASE+408,0);
 hook(S_IP_SET,0,DRAM_BASE+400,0,0);hook(S_UPDATE,0,0,0,0);net_service_guest();
 assert(updates==1 && net_get_stats().ip==0x0f02000a);
 // Public static configuration is still applied after DHCP is stopped.
 hle_wr32(DRAM_BASE+400,0x2a02000a);hle_wr32(DRAM_BASE+404,0x00ffffff);hle_wr32(DRAM_BASE+408,0x0202000a);
 hook(S_DHCPC_STOP,0,0,0,0);hook(S_IP_SET,0,DRAM_BASE+400,0,0);net_service_guest();
 assert(updates==2 && net_get_stats().ip==0x2a02000a);
 hook(S_DHCPC_START,0,0,0,0);net_service_guest();assert(updates==3);
 uint8_t frame[60];memset(frame,0xAA,sizeof frame);hle_write(DRAM_BASE+2048,frame,60);
 for(unsigned i=0;i<NET_QUEUE_MAX;i++)assert(hook(S_TX,0,DRAM_BASE+512,DRAM_BASE+2048,60)==0);
 assert(hook(S_TX,0,DRAM_BASE+512,DRAM_BASE+2048,60)!=0);assert(sends==NET_QUEUE_MAX && frees==0);
 E.call_depth=1;net_service_guest();assert(frees==0);E.call_depth=0;C.ps|=PS_EXCM;net_service_guest();assert(frees==0);C.ps=PS_UM;
 net_service_guest();assert(frees==NET_QUEUE_MAX);
 fail_send=true;assert(hook(S_TX,0,DRAM_BASE+512,DRAM_BASE+2048,60)!=0);net_service_guest();assert(frees==NET_QUEUE_MAX);fail_send=false;
 assert(!net_receive(0,frame,60));assert(net_receive(1,frame,60));net_service_guest();assert(inputs==1);
 fail_alloc=true;assert(net_receive(1,frame,60));net_service_guest();assert(inputs==1);fail_alloc=false;
 for(unsigned i=0;i<NET_QUEUE_MAX;i++)assert(net_receive(1,frame,60));
 assert(!net_receive(1,frame,60));
 hook(S_DISCONNECT,0,0,0,0);assert(hook(S_STATUS,0,0,0,0)==0);
 assert(!net_receive(1,frame,60));net_service_guest();assert(inputs==1 && net_get_stats().rx_pending==0);
 hook(S_CONNECT,0,0,0,0);net_service_guest();assert(net_get_stats().connected);
 assert(hook(S_TX,0,DRAM_BASE+512,DRAM_BASE+2048,60)==0);assert(net_receive(1,frame,60));net_reset(2);net_service_guest();assert(frees==NET_QUEUE_MAX && inputs==1);assert(!net_receive(1,frame,60));
 mode_lifecycle();profile_lifecycle();
 puts("net adapter ownership/scheduler/reset tests passed");
}
