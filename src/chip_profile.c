#include "chip_profile.h"

#include <stdio.h>
#include <string.h>

#define KiB 1024u
#define MiB (1024u * KiB)

/* ESP-12E/F modules commonly ship with 4 MiB; all capacities can be
 * overridden for external-flash profiles. The ESP8285 variants have fixed
 * embedded flash capacities. */
static const struct chip_profile profiles[] = {
    { "esp8266",    4u * MiB, 0, 1 },
    { "esp12e",     4u * MiB, 0, 1 },
    { "esp12f",     4u * MiB, 0, 1 },
    { "esp8285",    1u * MiB, 1, 0 },
    { "esp8285h16", 2u * MiB, 1, 0 },
};

const struct chip_profile *chip_profile_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; i++)
        if (strcmp(name, profiles[i].name) == 0) return &profiles[i];
    return NULL;
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size) snprintf(error, error_size, "%s", message);
}

int chip_profile_validate(const struct chip_profile *profile,
                          uint32_t flash_size,
                          uint64_t image_size,
                          char *error,
                          size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!profile || !profile->name || !profile->default_flash_size) {
        set_error(error, error_size, "invalid chip profile");
        return 0;
    }
    if (flash_size != profile->default_flash_size &&
        !profile->flash_size_override_allowed) {
        if (error && error_size)
            snprintf(error, error_size, "%s has fixed %u-byte flash",
                     profile->name, profile->default_flash_size);
        return 0;
    }
    if (!flash_size) {
        set_error(error, error_size, "flash size must be greater than zero");
        return 0;
    }
    if (flash_size < 256u * KiB || flash_size > 16u * MiB ||
        (flash_size & (flash_size - 1u)) != 0) {
        set_error(error, error_size, "flash size must be a power of two from 256 KiB to 16 MiB");
        return 0;
    }
    if (image_size > flash_size) {
        if (error && error_size)
            snprintf(error, error_size, "image is %llu bytes; flash capacity is %u bytes",
                     (unsigned long long)image_size, flash_size);
        return 0;
    }
    return 1;
}

int chip_profile_supports_spi_mode(const struct chip_profile *profile,
                                  enum chip_spi_mode mode)
{
    if (!profile || !profile->name) return 0;
    return mode >= CHIP_SPI_MODE_QIO && mode <= CHIP_SPI_MODE_DOUT;
}
