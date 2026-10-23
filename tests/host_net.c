// Exercise the actual libslirp transport with Ethernet ARP, without firmware.
#include "emu.h"
#include "host_net.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

emu_t E;
static uint64_t received_generation;
static unsigned received;
static uint8_t reply[NET_FRAME_MAX];
static size_t reply_size;

bool net_receive(uint64_t generation, const uint8_t *frame, size_t size)
{
    assert(size <= sizeof reply);
    received_generation = generation;
    memcpy(reply, frame, size);
    reply_size = size;
    received++;
    return true;
}
void ev_log(const char *fmt, ...) { (void)fmt; }
void emu_halt(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    abort();
}

int main(void)
{
    char error[128];
    assert(host_net_open(NULL, error, sizeof error));
    assert(!host_net_forward(0, 80, error, sizeof error));
    assert(!host_net_forward(8000, 65536, error, sizeof error));
    net_backend_t backend = host_net_backend();
    backend.reset(backend.ctx, 7);
    const uint8_t arp[] = {
        255,255,255,255,255,255, 2,0,0,0,0,1, 8,6,
        0,1, 8,0, 6,4, 0,1,
        2,0,0,0,0,1, 10,0,2,15,
        0,0,0,0,0,0, 10,0,2,2,
    };
    assert(backend.send(backend.ctx, arp, sizeof arp));
    backend.poll(backend.ctx);
    assert(received && received_generation == 7 && reply_size >= 42);
    assert(reply[12] == 8 && reply[13] == 6 && reply[21] == 2);
    assert(!memcmp(reply + 28, (uint8_t[]){10,0,2,2}, 4));
    assert(!memcmp(reply + 38, (uint8_t[]){10,0,2,15}, 4));
    unsigned previous = received;
    backend.reset(backend.ctx, 8);
    assert(backend.send(backend.ctx, arp, sizeof arp));
    backend.poll(backend.ctx);
    assert(received > previous && received_generation == 8);
    assert(!backend.send(backend.ctx, arp, 8));
    host_net_close();
    puts("host network ARP/reset tests passed");
    return 0;
}
