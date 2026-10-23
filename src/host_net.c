// Ethernet transport only. Firmware protocols still execute in the guest.
#include "host_net.h"
#include "host_dns.h"
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_SLIRP
#include <libslirp.h>
#include <poll.h>
#include <errno.h>

typedef struct timer {
    SlirpTimerCb callback;
    void *arg;
    int64_t due_ms;
    struct timer *next;
} timer_t_emu;
static struct {
    Slirp *slirp;
    timer_t_emu *timers;
    uint64_t generation;
    FILE *capture;
    struct pollfd fds[1024];
    unsigned fd_count;
    struct { unsigned host, guest; } forwards[16];
    unsigned forward_count;
} host;

static int64_t clock_ns(void *opaque)
{
    (void)opaque;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static void capture(const void *data, size_t size)
{
    if (!host.capture) return;
    uint64_t us = E.tick / 160;
    uint32_t header[4] = {(uint32_t)(us / 1000000), (uint32_t)(us % 1000000),
                          (uint32_t)size, (uint32_t)size};
    if (fwrite(header, sizeof header, 1, host.capture) != 1 ||
        fwrite(data, 1, size, host.capture) != size)
        emu_halt("cannot write network capture");
}

static ssize_t receive_packet(const void *data, size_t size, void *opaque)
{
    (void)opaque;
    capture(data, size);
    net_receive(host.generation, data, size);
    return (ssize_t)size; // A full guest queue drops frames; TCP may retransmit.
}

static void guest_error(const char *message, void *opaque)
{
    (void)opaque;
    ev_log("network backend: %s", message);
}

static void *new_timer(SlirpTimerCb callback, void *arg, void *opaque)
{
    (void)opaque;
    timer_t_emu *t = calloc(1, sizeof *t);
    if (!t) emu_halt("cannot allocate network timer");
    t->callback = callback; t->arg = arg; t->due_ms = -1;
    t->next = host.timers; host.timers = t;
    return t;
}

static void free_timer(void *timer, void *opaque)
{
    (void)opaque;
    timer_t_emu **link = &host.timers;
    while (*link && *link != timer) link = &(*link)->next;
    if (*link) { timer_t_emu *t = *link; *link = t->next; free(t); }
}

static void change_timer(void *timer, int64_t due_ms, void *opaque)
{
    (void)opaque;
    ((timer_t_emu *)timer)->due_ms = due_ms;
}

static void fd_changed(int fd, void *opaque) { (void)fd; (void)opaque; }
static void notify(void *opaque) { (void)opaque; }

static int add_fd(int fd, int events, void *opaque)
{
    (void)opaque;
    if (host.fd_count == sizeof host.fds / sizeof host.fds[0])
        emu_halt("network backend has too many sockets");
    int index = (int)host.fd_count++;
    short flags = 0;
    if (events & SLIRP_POLL_IN) flags |= POLLIN;
    if (events & SLIRP_POLL_OUT) flags |= POLLOUT;
    if (events & SLIRP_POLL_PRI) flags |= POLLPRI;
    host.fds[index] = (struct pollfd){.fd = fd, .events = flags};
    return index;
}

static int get_events(int index, void *opaque)
{
    (void)opaque;
    if (index < 0 || (unsigned)index >= host.fd_count) return SLIRP_POLL_ERR;
    short flags = host.fds[index].revents;
    int events = 0;
    if (flags & POLLIN) events |= SLIRP_POLL_IN;
    if (flags & POLLOUT) events |= SLIRP_POLL_OUT;
    if (flags & POLLPRI) events |= SLIRP_POLL_PRI;
    if (flags & (POLLERR | POLLNVAL)) events |= SLIRP_POLL_ERR;
    if (flags & POLLHUP) events |= SLIRP_POLL_HUP;
    return events;
}

static const SlirpCb callbacks = {
    .send_packet = receive_packet, .guest_error = guest_error, .clock_get_ns = clock_ns,
    .timer_new = new_timer, .timer_free = free_timer, .timer_mod = change_timer,
    .register_poll_fd = fd_changed, .unregister_poll_fd = fd_changed, .notify = notify,
};

static void reset_backend(void *opaque, uint64_t generation)
{
    (void)opaque;
    if (host.slirp) slirp_cleanup(host.slirp);
    host.slirp = NULL;
    host.generation = generation;
    SlirpConfig config = {.version = 4, .in_enabled = true,
                          .if_mtu = 1500, .if_mru = 1500};
    inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &config.vhost);
    inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
    inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);
    host.slirp = slirp_new(&config, &callbacks, NULL);
    if (!host.slirp) emu_halt("cannot start host network backend");
    struct in_addr local, guest;
    inet_pton(AF_INET, "127.0.0.1", &local);
    inet_pton(AF_INET, "10.0.2.15", &guest);
    for (unsigned i = 0; i < host.forward_count; i++)
        if (slirp_add_hostfwd(host.slirp, 0, local, host.forwards[i].host,
                             guest, host.forwards[i].guest) < 0)
            emu_halt("cannot forward host port %u", host.forwards[i].host);
}

static bool send_packet(void *opaque, const uint8_t *frame, size_t size)
{
    (void)opaque;
    if (!host.slirp || size < 14 || size > NET_FRAME_MAX) return false;
    capture(frame, size);
    uint8_t answer[NET_FRAME_MAX];
    size_t answer_size = host_dns_reply(frame, size, answer, sizeof answer);
    if (answer_size) {
        receive_packet(answer, answer_size, NULL);
        return true;
    }
    slirp_input(host.slirp, frame, (int)size);
    return true;
}

static void poll_backend(void *opaque)
{
    (void)opaque;
    if (!host.slirp) return;
    host.fd_count = 0;
    uint32_t timeout = 0;
    slirp_pollfds_fill(host.slirp, &timeout, add_fd, NULL);
    int result = poll(host.fds, host.fd_count, 0);
    slirp_pollfds_poll(host.slirp, result < 0 && errno != EINTR, get_events, NULL);
    // A callback may remove or add timers, so find the next due timer afresh.
    for (unsigned i = 0; i < 64; i++) {
        int64_t now = clock_ns(NULL) / 1000000;
        timer_t_emu *due = NULL;
        for (timer_t_emu *t = host.timers; t; t = t->next)
            if (t->due_ms >= 0 && t->due_ms <= now) { due = t; break; }
        if (!due) break;
        due->due_ms = -1;
        due->callback(due->arg);
    }
}

bool host_net_open(const char *path, char *error, size_t size)
{
    if (path) {
        host.capture = fopen(path, "wb");
        if (!host.capture) { snprintf(error, size, "cannot open network capture"); return false; }
        const uint32_t header[] = {0xa1b2c3d4u, 0x00040002u, 0, 0, NET_FRAME_MAX, 1};
        if (fwrite(header, sizeof header, 1, host.capture) != 1) {
            fclose(host.capture); host.capture = NULL;
            snprintf(error, size, "cannot write network capture header"); return false;
        }
    }
    if (size) error[0] = 0;
    return true;
}

net_backend_t host_net_backend(void)
{
    return (net_backend_t){.send = send_packet, .poll = poll_backend, .reset = reset_backend};
}

bool host_net_forward(unsigned host_port, unsigned guest_port, char *error, size_t size)
{
    if (!host_port || host_port > 65535 || !guest_port || guest_port > 65535 ||
        host.forward_count == 16) {
        snprintf(error, size, "invalid or too many TCP port forwards"); return false;
    }
    host.forwards[host.forward_count].host = host_port;
    host.forwards[host.forward_count++].guest = guest_port;
    return true;
}

void host_net_close(void)
{
    if (host.slirp) slirp_cleanup(host.slirp);
    while (host.timers) free_timer(host.timers, NULL);
    if (host.capture) fclose(host.capture);
    memset(&host, 0, sizeof host);
    host_dns_clear();
}
#else
bool host_net_open(const char *path, char *error, size_t size)
{
    (void)path;
    snprintf(error, size, "networking needs libslirp: build with make network");
    return false;
}
net_backend_t host_net_backend(void) { return (net_backend_t){0}; }
bool host_net_forward(unsigned host_port, unsigned guest_port, char *error, size_t size)
{
    (void)host_port; (void)guest_port;
    snprintf(error, size, "networking needs libslirp"); return false;
}
void host_net_close(void) {}
#endif
