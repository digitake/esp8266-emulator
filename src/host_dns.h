// Optional deterministic A-record answers for guest DNS queries.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool host_dns_add(const char *name, const char *ipv4, char *err, size_t err_size);
void host_dns_clear(void);

// Returns zero when the frame is not a supported query for an overridden name.
// Otherwise writes a complete Ethernet reply and returns its length.
size_t host_dns_reply(const uint8_t *ethernet, size_t len, uint8_t *reply, size_t cap);
