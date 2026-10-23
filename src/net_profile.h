// Image-bound symbol profiles for the opt-in Arduino virtual station.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool active, mode_normalized;
    unsigned image_index; // One-based; zero before a successful application bind.
    uint8_t image_mode, flash_mode;
} net_profile_match_t;

bool net_profile_load(const char *path, char *error, size_t error_size);
bool net_profile_requested(void);
net_profile_match_t net_profile_match(void);
void net_profile_reset(uint64_t generation);
void net_profile_app(void);
void net_profile_close(void);
