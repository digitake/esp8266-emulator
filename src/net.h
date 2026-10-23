// Opt-in Arduino lwIP2 virtual station. All calls run on the emulator thread.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_FRAME_MAX 1518u
#define NET_QUEUE_MAX 32u

typedef struct {
    // send must copy/consume before returning; never call guest code. False
    // means no ownership was accepted. poll may call net_receive().
    bool (*send)(void *ctx, const uint8_t *frame, size_t size);
    void (*poll)(void *ctx);
    void (*reset)(void *ctx, uint64_t generation);
    void *ctx;
} net_backend_t;

typedef struct {
    uint64_t tx_packets, rx_packets, tx_rejected, rx_dropped;
    uint64_t tx_released, resets;
    uint32_t ip, mask, gateway; // guest/lwIP byte order
    unsigned tx_pending, rx_pending;
    bool connected, initialized;
} net_stats_t;

// Call clear/bind/enable for a verified, image-specific symbol profile.
// Rebind after an OTA changes code addresses; a symbol name alone does not
// establish firmware compatibility. Unknown names are rejected.
void net_clear_symbols(void);
bool net_set_symbol(const char *name, uint32_t address);
bool net_enable(const net_backend_t *backend, char *error, size_t error_size);
void net_disable(void);
bool net_is_enabled(void);

// Reset on EVERY boot, including software reset. Drops old guest references
// without callbacks: the old heap no longer exists after the guest restarts.
void net_reset(uint64_t generation);

// CPU entry observer/interceptor. True means C.pc was advanced by this hook.
bool net_hook(uint32_t pc);
// Host I/O only, safe even when guest CPU execution is nested.
void net_poll_host(void);
// At the SDK scheduler boundary only, outside interrupts and nested calls.
void net_service_guest(void);
// Host backend ingress. Generation must identify the boot that accepted I/O.
bool net_receive(uint64_t generation, const uint8_t *frame, size_t size);
net_stats_t net_get_stats(void);
