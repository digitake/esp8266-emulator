#include "chip_profile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define KiB 1024u
#define MiB (1024u * KiB)

static void test_profiles(void)
{
    const struct chip_profile *p;

    p = chip_profile_find("esp8266");
    assert(p && p->default_flash_size == 4u * MiB);
    assert(!p->embedded_flash && p->flash_size_override_allowed);
    p = chip_profile_find("esp12e");
    assert(p && p->default_flash_size == 4u * MiB);
    assert(!p->embedded_flash && p->flash_size_override_allowed);
    p = chip_profile_find("esp12f");
    assert(p && p->default_flash_size == 4u * MiB);
    assert(!p->embedded_flash && p->flash_size_override_allowed);
    p = chip_profile_find("esp8285");
    assert(p && p->default_flash_size == 1u * MiB);
    assert(p->embedded_flash && !p->flash_size_override_allowed);
    p = chip_profile_find("esp8285h16");
    assert(p && p->default_flash_size == 2u * MiB);
    assert(p->embedded_flash && !p->flash_size_override_allowed);

    assert(!chip_profile_find("ESP8266"));
    assert(!chip_profile_find("esp12"));
    assert(!chip_profile_find(NULL));
}

static void test_capacity_validation(void)
{
    char error[160];
    const struct chip_profile *external = chip_profile_find("esp8266");
    const struct chip_profile *n08 = chip_profile_find("esp8285");
    const struct chip_profile *h16 = chip_profile_find("esp8285h16");

    assert(chip_profile_validate(external, 4u * MiB, 4u * MiB,
                                 error, sizeof error));
    assert(error[0] == '\0');
    assert(chip_profile_validate(external, 8u * MiB, 5u * MiB,
                                 error, sizeof error));
    assert(!chip_profile_validate(external, 4u * MiB, 4u * MiB + 1u,
                                  error, sizeof error));
    assert(strstr(error, "image") != NULL);
    assert(chip_profile_validate(n08, 1u * MiB, 1u * MiB,
                                 error, sizeof error));
    assert(!chip_profile_validate(n08, 2u * MiB, 1u * MiB,
                                  error, sizeof error));
    assert(strstr(error, "fixed") != NULL);
    assert(chip_profile_validate(h16, 2u * MiB, 2u * MiB,
                                 error, sizeof error));
    assert(!chip_profile_validate(h16, 2u * MiB, 2u * MiB + 1u,
                                  error, sizeof error));
    assert(!chip_profile_validate(external, 0, 0, error, sizeof error));
    assert(strstr(error, "greater than zero") != NULL);
    assert(!chip_profile_validate(external, 3u * MiB, 1u,
                                  error, sizeof error));
    assert(strstr(error, "power of two") != NULL);
    assert(!chip_profile_validate(NULL, 1u * MiB, 1,
                                  error, sizeof error));
}

static void test_spi_modes(void)
{
    const struct chip_profile *p = chip_profile_find("esp12e");
    for (int mode = CHIP_SPI_MODE_QIO; mode <= CHIP_SPI_MODE_DOUT; mode++)
        assert(chip_profile_supports_spi_mode(p, (enum chip_spi_mode)mode));
    assert(!chip_profile_supports_spi_mode(p, CHIP_SPI_MODE_INVALID));
    assert(!chip_profile_supports_spi_mode(NULL, CHIP_SPI_MODE_QIO));
}

int main(void)
{
    test_profiles();
    test_capacity_validation();
    test_spi_modes();
    puts("chip profile tests passed");
    return 0;
}
