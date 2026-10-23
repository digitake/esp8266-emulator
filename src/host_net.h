// Host transport for the optional virtual station.
#pragma once
#include "net.h"

bool host_net_open(const char *capture_path, char *error, size_t error_size);
net_backend_t host_net_backend(void);
bool host_net_forward(unsigned host_port, unsigned guest_port, char *error, size_t error_size);
void host_net_close(void);
