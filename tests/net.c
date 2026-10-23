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
// Offset first: memory+a would point far outside the array before the subtraction (UB clang's sanitizer reports)
void hle_read(uint32_t a, void *d, uint32_t nbytes) { assert(ram(a,nbytes)); memcpy(d,memory+(a-DRAM_BASE),nbytes); }
void hle_write(uint32_t a,const void *d,uint32_t nbytes) { assert(ram(a,nbytes)); memcpy(memory+(a-DRAM_BASE),d,nbytes); }
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
// Independent hashlib SHA256 answers for E9, tag, mode, then zero padding.
static const struct {unsigned size,tag,mode;const char *sha;} digest_vectors[] = {
 {16,1,0,"2d786cf767facdac91eaf88630d034b6bfe94361c0d8774e13f0f4be8ce122d3"},
 {16,2,0,"944301bb6d6da880941cad56169b6eae953f2ec27d0b0e623426ecb037639136"},
 {16,2,3,"e6b821dc9c1352d8e3becf8a0f29fef7ba5ac4b1bad0e8b11780e6bad02726b4"},
 {16,2,4,"8fd61b16549aedb90b8650bd63ed7152debe841a8388c1a0572ec2b92a191807"},
 {32,2,3,"9f507a34a71f8d38d4441f2d81c406f1c1122771da9434b7dbb2fadaf533bb7e"},
 {55,1,0,"c311189170df9d1e3a1f652242332bbc5367cbf9aa4ce1bdf69c3e52f3a0740f"},
 {56,1,0,"420809096bfafdc527f0f88eae91b985339e35b48f45c0de06c98094f4f4c3a3"},
 {63,1,0,"a344c28c241307f17fb7655e129d905775e518176dab2f709d4b7e730c9b8e9c"},
 {64,1,0,"eaf2377af1cd2f4cb494e54c8a7c55f791132579ecee78f4979b9a84bce84a7e"},
 {65,1,0,"3c04a32a2d20a3c27463c334131513cebdcad95d44c799e5cf91b006eb8f3d18"},
 {119,1,0,"1a849d1edcb53d507f742913cf7ff2561da0347029b01aff2d8e21c47a6f2464"},
 {120,1,0,"72a1693392ed91e067814967061740efffacf28ab2ec3692f0a822301d8dde14"},
 {127,1,0,"4864b72c52838531391fdb818cc3947eb659d836a65643d842b598c871585e08"},
 {128,1,0,"811c80877bfb0f30cc8b3246f2e54ce3d1a1b55cce869d8c4226eea2649a503e"},
 {129,1,0,"175e6ed6ec8d2d6e079f45495a5a3d003b1eaa5bbb9a0f084a847cdd15c1bd49"},
 {8192,1,0,"43fee11ca778a8f0d97dc89addd051c454b718419f48d708f71be3d4cbed1171"}
};
static const char *fixture_digest(const char *path) {
 FILE *f=fopen(path,"rb");assert(f);uint8_t data[8192];size_t size=fread(data,1,sizeof data,f);assert(!ferror(f));fclose(f);
 assert(size>=3 && data[0]==0xe9);for(unsigned i=3;i<size;i++)assert(!data[i]);
 for(unsigned i=0;i<sizeof digest_vectors/sizeof *digest_vectors;i++)
  if(size==digest_vectors[i].size && data[1]==digest_vectors[i].tag && data[2]==digest_vectors[i].mode)return digest_vectors[i].sha;
 assert(!"missing independent SHA256 test vector");return NULL;
}
static void image_section(FILE *f,const char *path,uint32_t offset,bool all) {
 fprintf(f,"image %s\nsha256 %s\n",path,fixture_digest(path));
 for(unsigned i=0;i<(all?S_COUNT:1);i++)fprintf(f,"symbol %s 0x%x\n",names[i],symbols[i]+offset);
 fputs("end\n",f);
}
static void two_image_profile(const char *profile,const char *a,const char *b) {
 FILE *f=fopen(profile,"w");assert(f);fputs("esp8266-net-v2 arduino-3.1.2-lwip2\n",f);
 image_section(f,a,0,true);image_section(f,b,0x10000,true);assert(!fclose(f));
}
static void profile_rejected(void) {
 expect_halt=true;
 if(!setjmp(halt_return)){net_profile_app();assert(!"unknown application was accepted");}
 expect_halt=false;
 assert(!net_is_enabled() && !net_profile_match().active);
}
static void digest_profile(const char *profile,const char *image,const char *lines) {
 FILE *f=fopen(profile,"w");assert(f);
 fprintf(f,"esp8266-net-v2 arduino-3.1.2-lwip2\nimage %s\n%s",image,lines);
 for(unsigned i=0;i<S_COUNT;i++)fprintf(f,"symbol %s 0x%x\n",names[i],symbols[i]);
 fputs("end\n",f);assert(!fclose(f));
}
static void profile_digest_checks(void) {
 char dir[]="/tmp/esp-net-digest-XXXXXX";assert(mkdtemp(dir));
 char image[256],profile[256],error[256],line[160];
 snprintf(image,sizeof image,"%s/image.bin",dir);snprintf(profile,sizeof profile,"%s/profile",dir);
 // Exercise both padding branches, exact blocks and a multi-kilobyte BIN.
 for(unsigned i=0;i<sizeof digest_vectors/sizeof *digest_vectors;i++) {
  if(digest_vectors[i].mode>3)continue;
  uint8_t data[8192]={0xe9};data[1]=digest_vectors[i].tag;data[2]=digest_vectors[i].mode;
  write_bytes(image,data,digest_vectors[i].size);
  snprintf(line,sizeof line,"sha256 %s\n",digest_vectors[i].sha);
  digest_profile(profile,image,line);assert(net_profile_load(profile,error,sizeof error));net_profile_close();
 }
 uint8_t data[16]={0xe9,1};write_bytes(image,data,sizeof data);
 const char *sha=digest_vectors[0].sha;
 snprintf(line,sizeof line,"sha256 %s\n",sha);digest_profile(profile,image,line);
 // Rebuilding or replacing the BIN after generating a profile fails closed,
 // including a source mode-only change that runtime matching would permit.
 data[8]=1;write_bytes(image,data,sizeof data);
 assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"SHA256 mismatch"));
 data[8]=0;data[2]=3;write_bytes(image,data,sizeof data);
 assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"SHA256 mismatch"));
 data[2]=0;write_bytes(image,data,sizeof data);
 digest_profile(profile,image,"");assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"missing image sha256"));
 digest_profile(profile,image,"sha256 1234\n");assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"sha256"));
 memset(line,'g',64);line[64]=0;char malformed[80];snprintf(malformed,sizeof malformed,"sha256 %.64s\n",line);
 digest_profile(profile,image,malformed);assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"sha256"));
 snprintf(line,sizeof line,"sha256 %s\nsha256 %s\n",sha,sha);
 digest_profile(profile,image,line);assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"duplicate"));
 FILE *f=fopen(profile,"w");assert(f);fputs("esp8266-net-v1 arduino-3.1.2-lwip2\n",f);assert(!fclose(f));
 assert(!net_profile_load(profile,error,sizeof error) && strstr(error,"regenerate"));
 assert(!net_profile_requested() && !net_is_enabled());
 net_profile_close();assert(!unlink(image));assert(!unlink(profile));assert(!rmdir(dir));
}
static void profile_lifecycle(void) {
 char dir[]="/tmp/esp-net-profile-XXXXXX";assert(mkdtemp(dir));
 char a[256],b[256],profile[256];snprintf(a,sizeof a,"%s/old image.bin",dir);snprintf(b,sizeof b,"%s/new image.bin",dir);snprintf(profile,sizeof profile,"%s/network.txt",dir);
 uint8_t old[16]={0xe9,1},next[16]={0xe9,2},flash[4096];
 write_bytes(a,old,sizeof old);write_bytes(b,next,sizeof next);
 FILE *f=fopen(profile,"w");assert(f);fputs("esp8266-net-v2 arduino-3.1.2-lwip2\n# provenance is documentary\n\n",f);image_section(f,a,0,true);image_section(f,b,0x10000,true);assert(!fclose(f));
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
 f=fopen(profile,"w");assert(f);fputs("esp8266-net-v2 arduino-3.1.2-lwip2\n",f);image_section(f,a,0,true);image_section(f,b,0x10000,true);assert(!fclose(f));
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"ambiguous"));
 // Even an exact match is unsafe if another ELF profile differs only in mode.
 next[2]=3;write_bytes(b,next,sizeof next);
 two_image_profile(profile,a,b);
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"ambiguous"));
 // Reject ambiguous prefixes with different BIN lengths too.
 uint8_t longer[32]={0};memcpy(longer,next,sizeof next);write_bytes(b,longer,sizeof longer);
 two_image_profile(profile,a,b);
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"ambiguous"));
 next[2]=4;write_bytes(b,next,sizeof next);
 two_image_profile(profile,a,b);
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"flash mode"));
 // Missing required symbols fail before booting.
 f=fopen(profile,"w");assert(f);fputs("esp8266-net-v2 arduino-3.1.2-lwip2\n",f);image_section(f,a,0,false);assert(!fclose(f));
 assert(!net_profile_load(profile,error,sizeof error));assert(strstr(error,"missing"));
 f=fopen(profile,"w");assert(f);fprintf(f,"esp8266-net-v2 arduino-3.1.2-lwip2\nimage %s\nsymbol %s 0x40200000\nsymbol %s 0x40200000\nend\n",a,names[0],names[1]);assert(!fclose(f));
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
 // Stopping DHCP alone preserves the assigned address and existing link. It
 // must not manufacture a reconfiguration/GOT_IP or discard active sockets.
 net_stats_t lease=net_get_stats();unsigned stopped=dhcp_stops,reset_count=backend_resets;
 unsigned down_count=link_downs,update_count=updates,event_count=events_seen;
 hook(S_DHCPC_STOP,0,0,0,0);net_service_guest();net_stats_t kept=net_get_stats();
 assert(!hook(S_DHCPC_STATUS,0,0,0,0));assert(kept.connected && kept.ip==lease.ip && kept.mask==lease.mask && kept.gateway==lease.gateway);
 assert(dhcp_stops==stopped+1 && backend_resets==reset_count && link_downs==down_count && updates==update_count && events_seen==event_count);
 // Public static configuration is still applied after DHCP is stopped.
 hle_wr32(DRAM_BASE+400,0x2a02000a);hle_wr32(DRAM_BASE+404,0x00ffffff);hle_wr32(DRAM_BASE+408,0x0202000a);
 hook(S_IP_SET,0,DRAM_BASE+400,0,0);net_service_guest();
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
 mode_lifecycle();profile_lifecycle();profile_digest_checks();
 puts("net adapter ownership/scheduler/reset tests passed");
}
