// Image-bound symbol profiles for the opt-in Arduino virtual station.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool net_profile_load(const char *path, char *error, size_t error_size);
bool net_profile_requested(void);
void net_profile_reset(uint64_t generation);
void net_profile_app(void);
void net_profile_close(void);
