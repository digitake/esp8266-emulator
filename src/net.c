// Arduino core 3.1.2 lwIP2 glue adapter. This models a virtual station at the
// SDK API boundary; it does not model RF association or modify guest code.
#include "emu.h"
#include "net.h"
#include <stdio.h>
#include <string.h>

enum symbol {
    S_TX, S_RX_ALLOC, S_RX_INPUT, S_FREE, S_UPDATE, S_UPDOWN, S_DHCP_START,
    S_DHCP_STOP, S_IFUPDOWN, S_LWIP_INIT, S_CONNECT, S_DISCONNECT, S_STATUS,
    S_IP_GET, S_IP_SET, S_EVENT_CB, S_DHCPC_START, S_DHCPC_STOP, S_DHCPC_STATUS,
    S_RSSI, S_MAC_GET, S_CONFIG_GET, S_CONFIG_SET, S_CONFIG_CURRENT, S_CHANNEL,
    S_COUNT
};
static const char *const names[S_COUNT] = {
    "glue2esp_linkoutput", "esp2glue_alloc_for_recv", "esp2glue_ethernet_input",
    "esp2glue_pbuf_freed", "esp2glue_netif_update", "esp2glue_netif_set_up1down0",
    "esp2glue_dhcp_start", "esp2glue_dhcp_stop", "glue2esp_ifupdown",
    "esp2glue_lwip_init", "wifi_station_connect", "wifi_station_disconnect",
    "wifi_station_get_connect_status", "wifi_get_ip_info", "wifi_set_ip_info",
    "wifi_set_event_handler_cb", "wifi_station_dhcpc_start", "wifi_station_dhcpc_stop",
    "wifi_station_dhcpc_status", "wifi_station_get_rssi", "wifi_get_macaddr",
    "wifi_station_get_config", "wifi_station_set_config", "wifi_station_set_config_current",
    "wifi_get_channel"
};
static uint32_t symbols[S_COUNT];
// Entry-point filter: avoid a linear symbol scan for ordinary instructions.
static uint8_t entry_bits[8192];
static net_backend_t backend;
static bool enabled;
typedef struct { uint16_t size; uint8_t data[NET_FRAME_MAX]; } frame_t;
static struct {
    uint64_t generation;
    bool busy, updating, lwip_seen, connected, initialized, start, stop, dhcp, dhcp_running;
    uint32_t event_cb, ip, mask, gw, static_ip, static_mask, static_gw;
    uint8_t ssid[32], mac[6], bssid[6];
    uint32_t releases[NET_QUEUE_MAX];
    unsigned release_head, release_count, rx_head, rx_count;
    frame_t rx[NET_QUEUE_MAX];
    uint8_t events[16][44];
    unsigned event_head, event_count;
    net_stats_t stats;
} n;

static unsigned entry_key(uint32_t pc) { return (pc ^ (pc >> 16)) & 65535u; }
static bool ram(uint32_t addr, uint32_t len) {
    return addr >= DRAM_BASE && len <= DRAM_SIZE && addr - DRAM_BASE <= DRAM_SIZE - len;
}
static bool return_value(uint32_t value) { C.a[2] = value; C.pc = C.a[0]; return true; }
static uint32_t call(enum symbol s, int argc, const uint32_t *argv) {
    return guest_call(symbols[s], argc, argv);
}

void net_clear_symbols(void) {
    enabled = false;
    memset(symbols, 0, sizeof symbols);
    memset(entry_bits, 0, sizeof entry_bits);
}
bool net_set_symbol(const char *name, uint32_t address) {
    if (enabled || !address) return false;
    for (unsigned i = 0; i < S_COUNT; i++) if (!strcmp(name, names[i])) {
        // Functions must reside in executable guest memory, not MMIO/ROM data.
        if (!((address >= IRAM_BASE && address < IRAM_BASE + IRAM_SIZE) ||
              (address >= FLASHMAP_BASE && address < FLASHMAP_BASE + FLASHMAP_SIZE))) return false;
        symbols[i] = address;
        unsigned bit = entry_key(address);
        entry_bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
        return true;
    }
    return false;
}
bool net_enable(const net_backend_t *b, char *error, size_t size) {
    if (!b || !b->send) {
        if (size) snprintf(error, size, "network backend has no send callback");
        return false;
    }
    // Optional observers/getters may be discarded by linker GC. Every entry
    // used to drive the guest and every connection/status boundary is required.
    for (unsigned i = 0; i < S_COUNT; i++) {
        bool required = i <= S_STATUS || i == S_IP_GET || i == S_EVENT_CB || i == S_MAC_GET;
        if (required && !symbols[i]) {
            if (size) snprintf(error, size, "network profile missing %s", names[i]);
            return false;
        }
    }
    backend = *b;
    enabled = true;
    if (size) error[0] = 0;
    return true;
}
void net_disable(void) { enabled = false; }
bool net_is_enabled(void) { return enabled; }
void net_reset(uint64_t generation) {
    uint64_t resets = n.stats.resets + (n.generation != generation || !n.stats.resets);
    memset(&n, 0, sizeof n);
    n.generation = generation;
    n.dhcp = true;
    n.stats.resets = resets;
    // Locally administered virtual AP identity; never advertised as physical RF.
    const uint8_t ap[6] = { 0x02, 0x45, 0x4d, 0x55, 0x00, 0x01 };
    memcpy(n.bssid, ap, sizeof ap);
    if (enabled && backend.reset) backend.reset(backend.ctx, generation);
}
static void event(unsigned kind) {
    if (!n.event_cb) return;
    if (n.event_count == 16) { ev_log("network event queue full"); return; }
    uint8_t *e = n.events[(n.event_head + n.event_count++) % 16];
    memset(e, 0, 44);
    e[0] = (uint8_t)kind;
    if (kind == 3) {
        memcpy(e + 4, &n.ip, 4); memcpy(e + 8, &n.mask, 4); memcpy(e + 12, &n.gw, 4);
    } else {
        memcpy(e + 4, n.ssid, 32);
        unsigned len = 0; while (len < 32 && n.ssid[len]) len++;
        e[36] = (uint8_t)len;
        memcpy(e + 37, n.bssid, 6);
        e[43] = kind == 1 ? 8 : 1; // association leave / virtual channel 1
    }
}
static void set_ip(uint32_t ip, uint32_t mask, uint32_t gw) {
    bool changed = ip && (ip != n.ip || mask != n.mask || gw != n.gw);
    n.ip = ip; n.mask = mask; n.gw = gw;
    if (changed && n.connected) event(3);
}
bool net_hook(uint32_t pc) {
    if (!enabled) return false;
    unsigned bit = entry_key(pc);
    if (!(entry_bits[bit / 8] & (1u << (bit % 8)))) return false;
    int s; for (s = 0; s < S_COUNT; s++) if (symbols[s] && symbols[s] == pc) break;
    uint32_t a = C.a[2], b = C.a[3];
    switch (s) {
    case S_LWIP_INIT: n.lwip_seen = true; return false;
    case S_CONFIG_SET: case S_CONFIG_CURRENT:
        if (ram(a, 32)) hle_read(a, n.ssid, 32);
        return false; // retain SDK storage/persistence semantics
    case S_TX: {
        uint32_t data = C.a[4], len = C.a[5];
        if (a != 0 || !n.connected || len < 14 || len > NET_FRAME_MAX ||
            !ram(data, len) || !ram(b, 4) || n.release_count == NET_QUEUE_MAX) {
            n.stats.tx_rejected++;
            return return_value((uint32_t)-12); // GLUE_ERR_IF; caller retains ref
        }
        uint8_t frame[NET_FRAME_MAX];
        hle_read(data, frame, len);
        if (!backend.send(backend.ctx, frame, len)) {
            n.stats.tx_rejected++;
            return return_value((uint32_t)-1); // caller releases ref on error
        }
        n.releases[(n.release_head + n.release_count++) % NET_QUEUE_MAX] = b;
        n.stats.tx_packets++;
        return return_value(0);
    }
    case S_IFUPDOWN:
        if (a != 0) return false;
        set_ip(b, C.a[4], C.a[5]);
        return return_value(0);
    case S_UPDATE:
        // The closed SDK can finish installing its radio-side peer after our
        // virtual station already received a lease. Its stale zero address
        // must not reset the active lwIP interface. Public wifi_set_ip_info
        // requests are cached below and applied by the deferred owner instead.
        if (a == 0 && n.initialized && !n.updating) return return_value(0);
        return false;
    case S_CONNECT:
        if (!n.connected) n.start = true;
        return return_value(1);
    case S_DISCONNECT:
        n.start = false; n.stop = true;
        if (n.connected) event(1);
        n.connected = false;
        set_ip(0, 0, 0);
        return return_value(1);
    case S_STATUS: return return_value(n.connected ? (n.ip ? 5 : 1) : (n.start ? 1 : 0));
    case S_IP_GET:
        if (a != 0) return false;
        if (!ram(b, 12)) return return_value(0);
        hle_wr32(b, n.ip); hle_wr32(b + 4, n.mask); hle_wr32(b + 8, n.gw);
        return return_value(1);
    case S_IP_SET:
        if (a != 0) return false;
        if (!ram(b, 12)) return return_value(0);
        n.static_ip = hle_rd32(b); n.static_mask = hle_rd32(b + 4); n.static_gw = hle_rd32(b + 8);
        if (n.connected && !n.dhcp) n.start = true;
        return return_value(1);
    case S_EVENT_CB: n.event_cb = a; return return_value(0);
    case S_DHCPC_START:
        if (n.connected && (!n.dhcp || !n.dhcp_running)) n.start = true;
        n.dhcp = true;
        return return_value(1);
    case S_DHCPC_STOP:
        if (n.connected && n.dhcp) n.start = true;
        n.dhcp = false;
        return return_value(1);
    case S_DHCPC_STATUS: return return_value(n.dhcp ? 1 : 0);
    case S_RSSI: return return_value(n.connected ? (uint32_t)-42 : 31);
    case S_CHANNEL: if (n.connected) return return_value(1); return false;
    default: return false;
    }
}
bool net_receive(uint64_t generation, const uint8_t *frame, size_t size) {
    if (!enabled || generation != n.generation || !n.connected || !frame ||
        size < 14 || size > NET_FRAME_MAX || n.rx_count == NET_QUEUE_MAX) {
        n.stats.rx_dropped++;
        return false;
    }
    frame_t *f = &n.rx[(n.rx_head + n.rx_count++) % NET_QUEUE_MAX];
    f->size = (uint16_t)size;
    memcpy(f->data, frame, size);
    return true;
}
void net_poll_host(void) { if (enabled && backend.poll) backend.poll(backend.ctx); }

void net_service_guest(void) {
    if (!enabled || n.busy || !n.lwip_seen || E.call_depth ||
        (C.ps & (PS_INTLEVEL | PS_EXCM))) return;
    // Temporarily reserve scratch ABOVE the nested guest's stack. Never use an
    // arbitrary address in the guest heap. guest_call restores argument regs.
    uint32_t sp = C.a[1];
    if (sp < 256 || !ram(sp - 256, 256)) return;
    uint8_t saved[256]; hle_read(sp - 256, saved, sizeof saved);
    C.a[1] = sp - 256;
    uint32_t scratch = C.a[1] + 32;
    n.busy = true;
    bool waiting = C.waiting; C.waiting = false;
    uint32_t zero = 0;
    if (n.stop) {
        n.stop = false;
        if (n.dhcp_running) call(S_DHCP_STOP, 1, &zero);
        n.dhcp_running = false;
        uint32_t down[2] = {0, 0}; call(S_UPDOWN, 2, down);
        n.rx_head = n.rx_count = 0;
    }
    if (n.start) {
        n.start = false;
        uint32_t mac_args[2] = {0, scratch};
        if (!call(S_MAC_GET, 2, mac_args)) {
            ev_log("network: guest station MAC unavailable");
        } else {
            hle_read(scratch, n.mac, 6);
            if (symbols[S_CONFIG_GET]) {
                uint32_t arg = scratch + 16;
                if (call(S_CONFIG_GET, 1, &arg)) hle_read(arg, n.ssid, 32);
            }
            bool first = !n.connected;
            n.connected = true;
            if (first) event(0);
            if (n.dhcp_running) call(S_DHCP_STOP, 1, &zero);
            n.dhcp_running = false;
            hle_write(scratch, n.mac, 6);
            uint32_t args[7] = {0, n.dhcp ? 0 : n.static_ip, n.dhcp ? 0 : n.static_mask,
                               n.dhcp ? 0 : n.static_gw, 6, scratch, 1500};
            n.updating = true;
            call(S_UPDATE, 7, args);
            n.updating = false;
            n.initialized = true;
            if (n.dhcp) {
                uint32_t rc = call(S_DHCP_START, 1, &zero);
                n.dhcp_running = rc == 0;
                if (rc) ev_log("network: guest DHCP start failed (%d)", (int32_t)rc);
            } else set_ip(n.static_ip, n.static_mask, n.static_gw);
        }
    }
    // Drain a bounded snapshot. A callback may produce more work; defer it to
    // the next scheduler visit rather than recursively draining forever.
    unsigned releases = n.release_count;
    while (releases--) {
        uint32_t ref = n.releases[n.release_head];
        n.release_head = (n.release_head + 1) % NET_QUEUE_MAX; n.release_count--;
        call(S_FREE, 1, &ref); n.stats.tx_released++;
    }
    unsigned receives = n.rx_count < 8 ? n.rx_count : 8;
    while (receives-- && n.connected && n.rx_count) {
        frame_t frame = n.rx[n.rx_head];
        n.rx_head = (n.rx_head + 1) % NET_QUEUE_MAX; n.rx_count--;
        hle_wr32(scratch, 0); hle_wr32(scratch + 4, 0);
        uint32_t args[3] = {frame.size, scratch, scratch + 4};
        call(S_RX_ALLOC, 3, args);
        uint32_t ref = hle_rd32(scratch), data = hle_rd32(scratch + 4);
        if (!ref) { n.stats.rx_dropped++; continue; }
        if (!ram(ref, 4) || !ram(data, frame.size)) {
            // A corrupt guest allocation is an emulator/ABI error, not a packet
            // to write outside RAM. Stop before following an unchecked pointer.
            emu_halt("network: invalid guest receive allocation");
        }
        hle_write(data, frame.data, frame.size);
        uint32_t input[2] = {0, ref};
        call(S_RX_INPUT, 2, input); // ownership transfers, even when packet is dropped
        n.stats.rx_packets++;
    }
    unsigned events = n.event_count;
    while (events-- && n.event_count) {
        hle_write(scratch, n.events[n.event_head], 44);
        n.event_head = (n.event_head + 1) % 16; n.event_count--;
        if (n.event_cb) guest_call(n.event_cb, 1, &scratch);
    }
    C.waiting = waiting;
    hle_write(sp - 256, saved, sizeof saved);
    C.a[1] = sp;
    n.busy = false;
}
net_stats_t net_get_stats(void) {
    net_stats_t s = n.stats;
    s.ip = n.ip; s.mask = n.mask; s.gateway = n.gw;
    s.tx_pending = n.release_count; s.rx_pending = n.rx_count;
    s.connected = n.connected; s.initialized = n.initialized;
    return s;
}
