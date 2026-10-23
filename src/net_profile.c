#include "net_profile.h"
#include "net.h"
#include "host_net.h"
#include "emu.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROFILE_IMAGES 8
#define PROFILE_SYMBOLS 64
#define PROFILE_LINE 4096
#define PROFILE_IMAGE_MAX (16u * 1024u * 1024u)

typedef struct { char name[64]; uint32_t address; } binding_t;
typedef struct {
    uint8_t *bytes;
    size_t size;
    binding_t symbols[PROFILE_SYMBOLS];
    unsigned count;
} profile_image_t;
static profile_image_t images[PROFILE_IMAGES];
static unsigned image_count;
static bool requested, activated;

void net_profile_close(void)
{
    E.entry_hook = NULL;
    E.guest_service = NULL;
    net_disable();
    for (unsigned i = 0; i < PROFILE_IMAGES; i++) free(images[i].bytes);
    memset(images, 0, sizeof images);
    image_count = 0;
    requested = activated = false;
    net_clear_symbols();
}

static bool image_read(profile_image_t *image, const char *path, char *error, size_t size)
{
    if (path[0] != '/') {
        snprintf(error, size, "image path must be absolute");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(error, size, "cannot read profile image: %s", strerror(errno)); return false; }
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 16 || st.st_size > PROFILE_IMAGE_MAX) {
        snprintf(error, size, "profile image must be a regular 16-byte to 16-MiB file");
        fclose(f);
        return false;
    }
    image->size = (size_t)st.st_size;
    image->bytes = malloc(image->size);
    if (!image->bytes) { snprintf(error, size, "cannot allocate profile image"); fclose(f); return false; }
    bool ok = fread(image->bytes, 1, image->size, f) == image->size && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!ok || image->bytes[0] != 0xe9) {
        snprintf(error, size, "profile image must be a readable E9 image for flash address zero");
        return false;
    }
    return true;
}

static bool bind(const profile_image_t *image, char *error, size_t size)
{
    net_clear_symbols();
    for (unsigned i = 0; i < image->count; i++)
        if (!net_set_symbol(image->symbols[i].name, image->symbols[i].address)) {
            snprintf(error, size, "unknown/invalid network symbol %s", image->symbols[i].name);
            return false;
        }
    return true;
}

static bool validate_send(void *ctx, const uint8_t *frame, size_t size)
{ (void)ctx; (void)frame; (void)size; return false; }

bool net_profile_load(const char *path, char *error, size_t error_size)
{
    // Keep diagnostics valid even for callers that do not request an error string.
    char fallback[256];
    if (!error || !error_size) { error = fallback; error_size = sizeof fallback; }
    error[0] = 0;
    net_profile_close();
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(error, error_size, "cannot open network profile: %s", strerror(errno)); return false; }
    char line[PROFILE_LINE + 2];
    unsigned line_number = 0;
    profile_image_t *current = NULL;
    bool ok = true;
    while (fgets(line, sizeof line, f)) {
        line_number++;
        size_t len = strlen(line);
        if (len > PROFILE_LINE || (len && line[len - 1] != '\n' && !feof(f))) {
            snprintf(error, error_size, "network profile line %u is too long", line_number); ok = false; break;
        }
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (line_number == 1) {
            if (strcmp(line, "esp8266-net-v1 arduino-3.1.2-lwip2")) {
                snprintf(error, error_size, "unsupported network profile header"); ok = false; break;
            }
            continue;
        }
        const char *content = line;
        while (*content && isspace((unsigned char)*content)) content++;
        if (!*content || *content == '#') continue;
        if (!strncmp(line, "image ", 6)) {
            if (current || image_count == PROFILE_IMAGES) {
                snprintf(error, error_size, "nested or too many profile images"); ok = false; break;
            }
            current = &images[image_count++];
            if (!image_read(current, line + 6, error, error_size)) { ok = false; break; }
        } else if (!strncmp(line, "symbol ", 7)) {
            if (!current || current->count == PROFILE_SYMBOLS) {
                snprintf(error, error_size, "symbol outside image or too many symbols"); ok = false; break;
            }
            char name[64], address[32], extra;
            if (sscanf(line + 7, "%63s %31s %c", name, address, &extra) != 2 ||
                strncmp(address, "0x", 2) || !isxdigit((unsigned char)address[2])) {
                snprintf(error, error_size, "invalid profile symbol at line %u", line_number); ok = false; break;
            }
            errno = 0; char *end;
            unsigned long value = strtoul(address + 2, &end, 16);
            if (*end || errno || value > UINT32_MAX) {
                snprintf(error, error_size, "invalid symbol address at line %u", line_number); ok = false; break;
            }
            for (unsigned i = 0; i < current->count; i++)
                if (!strcmp(current->symbols[i].name, name) || current->symbols[i].address == value) {
                    snprintf(error, error_size, "duplicate symbol name/address at line %u", line_number); ok = false; break;
                }
            if (!ok) break;
            binding_t *symbol = &current->symbols[current->count++];
            snprintf(symbol->name, sizeof symbol->name, "%s", name);
            symbol->address = (uint32_t)value;
        } else if (!strcmp(line, "end")) {
            if (!current) { snprintf(error, error_size, "end outside profile image"); ok = false; break; }
            if (!bind(current, error, error_size)) { ok = false; break; }
            net_backend_t validation = {.send = validate_send};
            if (!net_enable(&validation, error, error_size)) { ok = false; break; }
            net_disable();
            current = NULL;
        } else {
            snprintf(error, error_size, "unknown profile directive at line %u", line_number); ok = false; break;
        }
    }
    if (ok && (ferror(f) || !line_number || !image_count || current)) {
        snprintf(error, error_size, "incomplete network profile"); ok = false;
    }
    fclose(f);
    if (!ok) { net_profile_close(); return false; }
    // Prefix-identical profiles are ambiguous because persistent flash may be
    // larger than either BIN. Reject them before executing guest instructions.
    for (unsigned i = 0; i < image_count; i++) for (unsigned j = 0; j < i; j++) {
        size_t common = images[i].size < images[j].size ? images[i].size : images[j].size;
        if (!memcmp(images[i].bytes, images[j].bytes, common)) {
            snprintf(error, error_size, "ambiguous prefix-identical network images");
            net_profile_close(); return false;
        }
    }
    net_clear_symbols();
    requested = true;
    return true;
}

bool net_profile_requested(void) { return requested; }

void net_profile_reset(uint64_t generation)
{
    E.entry_hook = NULL;
    E.guest_service = NULL;
    net_disable();
    net_reset(generation);
    activated = false;
}

void net_profile_app(void)
{
    if (!requested || activated) return;
    const profile_image_t *match = NULL;
    unsigned index = 0;
    for (unsigned i = 0; i < image_count; i++) {
        if (images[i].size <= E.flash_size && !memcmp(images[i].bytes, E.flash, images[i].size)) {
            if (match) emu_halt("network profile matches multiple application images");
            match = &images[i]; index = i;
        }
    }
    if (!match) emu_halt("network profile has no exact match for running application; supply its BIN/ELF profile");
    char error[256];
    if (!bind(match, error, sizeof error)) emu_halt("network profile: %s", error);
    net_backend_t transport = host_net_backend();
    if (!net_enable(&transport, error, sizeof error)) emu_halt("network profile: %s", error);
    net_reset(E.boot_count);
    E.entry_hook = net_hook;
    E.guest_service = net_service_guest;
    activated = true;
    ev_log("network: virtual station Arduino 3.1.2/lwIP2, exact image profile %u", index + 1);
}
