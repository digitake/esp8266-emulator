// Host transport for the optional virtual station.
#pragma once
#include "net.h"

bool host_net_open(const char *capture_path, char *error, size_t error_size);
net_backend_t host_net_backend(void);
bool host_net_forward(unsigned host_port, unsigned guest_port, char *error, size_t error_size);
// Keep the guest on the virtual network: drop frames that host_dns_frame_is_local() rejects.
void host_net_set_local_only(void);
void host_net_close(void);
