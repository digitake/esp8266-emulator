#ifndef CHIP_PROFILE_H
#define CHIP_PROFILE_H

#include <stddef.h>
#include <stdint.h>

/* A named emulator configuration for chip identity and flash capacity. */
struct chip_profile {
    const char *name;
    uint32_t default_flash_size;
    int embedded_flash;
    int flash_size_override_allowed;
};

enum chip_spi_mode {
    CHIP_SPI_MODE_QIO = 0,
    CHIP_SPI_MODE_QOUT,
    CHIP_SPI_MODE_DIO,
    CHIP_SPI_MODE_DOUT,
    CHIP_SPI_MODE_INVALID
};

#define CHIP_SPI_MODE_BIT(mode) (1u << (unsigned)(mode))
#define CHIP_SPI_MODE_ALL \
    (CHIP_SPI_MODE_BIT(CHIP_SPI_MODE_QIO) | \
     CHIP_SPI_MODE_BIT(CHIP_SPI_MODE_QOUT) | \
     CHIP_SPI_MODE_BIT(CHIP_SPI_MODE_DIO) | \
     CHIP_SPI_MODE_BIT(CHIP_SPI_MODE_DOUT))

/* Returns NULL for an unknown profile name. Names are lowercase and exact. */
const struct chip_profile *chip_profile_find(const char *name);

/* Returns 1 when the image fits the profile, otherwise 0 and writes a reason. */
int chip_profile_validate(const struct chip_profile *profile,
                          uint32_t flash_size,
                          uint64_t image_size,
                          char *error,
                          size_t error_size);

/* True for the four ESP8266 boot-header modes represented by the emulator. */
int chip_profile_supports_spi_mode(const struct chip_profile *profile,
                                  enum chip_spi_mode mode);

#endif
