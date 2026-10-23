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
    bool digest_checked;
} profile_image_t;
static profile_image_t images[PROFILE_IMAGES];
static unsigned image_count;
static bool requested, activated;
static net_profile_match_t selected;

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(uint32_t h[8], const uint8_t p[64])
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
               (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (ror(x,7) ^ ror(x,18) ^ (x >> 3)) + w[i-7] +
               (ror(y,17) ^ ror(y,19) ^ (y >> 10));
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = v + (ror(e,6) ^ ror(e,11) ^ ror(e,25)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a,2) ^ ror(a,13) ^ ror(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        v=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=v;
}

// Hash the already bounded/cached source BIN, before any running-image mode
// equivalence. No second file read can race verification against binding.
static void image_sha256(const profile_image_t *image, char hex[65])
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t full = image->size & ~(size_t)63;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, image->bytes + i);
    uint8_t tail[128] = {0};
    size_t rem = image->size - full, padded = rem < 56 ? 64 : 128;
    memcpy(tail, image->bytes + full, rem);
    tail[rem] = 0x80;
    uint64_t bits = (uint64_t)image->size * 8;
    for (unsigned i = 0; i < 8; i++) tail[padded - 1 - i] = (uint8_t)(bits >> (8*i));
    sha256_block(h, tail);
    if (padded == 128) sha256_block(h, tail + 64);
    for (unsigned i = 0; i < 8; i++) snprintf(hex + 8*i, 9, "%08x", h[i]);
}

// Arduino 3.1.2 Updater::_writeBuffer may replace only header byte 2 on raw
// U_FLASH updates, preserving the running QIO/QOUT/DIO/DOUT mode (0..3).
// Compare the complete cached BIN otherwise. This also recognizes a persisted
// updated image on a fresh process, where no previous boot mode is available.
static bool image_matches(const uint8_t *a, const uint8_t *b, size_t size)
{
    return size >= 3 && a[2] <= 3 && b[2] <= 3 &&
           !memcmp(a, b, 2) && !memcmp(a + 3, b + 3, size - 3);
}

void net_profile_close(void)
{
    E.entry_hook = NULL;
    E.guest_service = NULL;
    net_disable();
    for (unsigned i = 0; i < PROFILE_IMAGES; i++) free(images[i].bytes);
    memset(images, 0, sizeof images);
    image_count = 0;
    requested = activated = false;
    selected = (net_profile_match_t){0};
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
    if (image->bytes[2] > 3) {
        snprintf(error, size, "profile image has an unsupported flash mode (expected 0..3)");
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
            if (strcmp(line, "esp8266-net-v2 arduino-3.1.2-lwip2")) {
                snprintf(error, error_size, "network profile requires v2 BIN digest binding; regenerate with scripts/network_profile.py"); ok = false; break;
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
        } else if (!strncmp(line, "sha256 ", 7)) {
            if (!current || current->digest_checked || strlen(line + 7) != 64) {
                snprintf(error, error_size, "invalid, duplicate or misplaced image sha256 at line %u", line_number); ok = false; break;
            }
            char expected[65], actual[65];
            for (unsigned i = 0; i < 64; i++) {
                unsigned char c = (unsigned char)line[7+i];
                if (!isxdigit(c)) { ok = false; break; }
                expected[i] = (char)tolower(c);
            }
            expected[64] = 0;
            if (!ok) { snprintf(error, error_size, "invalid image sha256 at line %u", line_number); break; }
            image_sha256(current, actual);
            if (strcmp(expected, actual)) {
                snprintf(error, error_size, "network profile BIN SHA256 mismatch; regenerate with scripts/network_profile.py for the matching BIN/ELF"); ok = false; break;
            }
            current->digest_checked = true;
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
            if (!current->digest_checked) {
                snprintf(error, error_size, "missing image sha256; regenerate with scripts/network_profile.py"); ok = false; break;
            }
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
    // Persistent flash may be larger than either BIN. Reject profiles whose
    // prefixes overlap even after the one permitted flash-mode substitution.
    for (unsigned i = 0; i < image_count; i++) for (unsigned j = 0; j < i; j++) {
        size_t common = images[i].size < images[j].size ? images[i].size : images[j].size;
        if (image_matches(images[i].bytes, images[j].bytes, common)) {
            snprintf(error, error_size, "ambiguous network image prefixes after flash-mode normalization");
            net_profile_close(); return false;
        }
    }
    net_clear_symbols();
    requested = true;
    return true;
}

bool net_profile_requested(void) { return requested; }
net_profile_match_t net_profile_match(void) { return selected; }

void net_profile_reset(uint64_t generation)
{
    E.entry_hook = NULL;
    E.guest_service = NULL;
    net_disable();
    net_reset(generation);
    activated = false;
    selected = (net_profile_match_t){0};
}

void net_profile_app(void)
{
    if (!requested || activated) return;
    const profile_image_t *match = NULL;
    unsigned index = 0;
    for (unsigned i = 0; i < image_count; i++) {
        if (images[i].size <= E.flash_size && image_matches(images[i].bytes, E.flash, images[i].size)) {
            if (match) emu_halt("network profile matches multiple application images");
            match = &images[i]; index = i;
        }
    }
    if (!match) emu_halt("network profile has no matching application (only valid flash-mode byte 2 may differ); supply its BIN/ELF profile");
    char error[256];
    if (!bind(match, error, sizeof error)) emu_halt("network profile: %s", error);
    net_backend_t transport = host_net_backend();
    if (!net_enable(&transport, error, sizeof error)) emu_halt("network profile: %s", error);
    net_reset(E.boot_count);
    E.entry_hook = net_hook;
    E.guest_service = net_service_guest;
    activated = true;
    selected = (net_profile_match_t){.active = true, .image_index = index + 1,
        .mode_normalized = match->bytes[2] != E.flash[2],
        .image_mode = match->bytes[2], .flash_mode = E.flash[2]};
    if (selected.mode_normalized)
        ev_log("network: virtual station Arduino 3.1.2/lwIP2, image profile %u, flash-mode byte 2 normalized from %u to %u; all other BIN bytes exact",
               index + 1, selected.image_mode, selected.flash_mode);
    else
        ev_log("network: virtual station Arduino 3.1.2/lwIP2, exact image profile %u", index + 1);
}
