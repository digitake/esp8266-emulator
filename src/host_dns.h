// Optional deterministic A-record answers for guest DNS queries.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool host_dns_add(const char *name, const char *ipv4, char *err, size_t err_size);
// Answer A queries for every name without an override with this address (--net-local).
bool host_dns_set_fallback(const char *ipv4, char *err, size_t err_size);
void host_dns_clear(void);

// True when a guest frame stays inside the virtual network: ARP, and IPv4 to 10.0.2.0/24 (except the
// backend's resolver at 10.0.2.3), broadcast or multicast. --net-local drops every other frame.
bool host_dns_frame_is_local(const uint8_t *ethernet, size_t len);

// Returns zero when the frame is not a supported query for an overridden name.
// Otherwise writes a complete Ethernet reply and returns its length.
size_t host_dns_reply(const uint8_t *ethernet, size_t len, uint8_t *reply, size_t cap);
