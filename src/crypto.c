// MD5 and SHA-1 for the ROM routines the SDK calls (hostap-style HMAC/PBKDF2/PRF built on top in rom.c).
// Straightforward implementations of RFC 1321 and FIPS 180-1.
#include <stdint.h>
#include <string.h>
#include "crypto.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

// ---------------------------------------------------------------- MD5
static void md5_block(uint32_t st[4], const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t tmp = d; d = c; c = b;
        b = b + ROL(a + f + K[i] + w[g], R[i]);
        a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

// ctx layout matches the ROM's struct MD5Context { u32 buf[4]; u32 bits[2]; u8 in[64]; }
void md5_init(md5_ctx *c)
{
    c->buf[0] = 0x67452301; c->buf[1] = 0xefcdab89; c->buf[2] = 0x98badcfe; c->buf[3] = 0x10325476;
    c->bits[0] = c->bits[1] = 0;
}

void md5_update(md5_ctx *c, const uint8_t *d, size_t n)
{
    uint32_t have = (c->bits[0] >> 3) & 63;
    uint32_t lo = c->bits[0] + (uint32_t)(n << 3);
    if (lo < c->bits[0]) c->bits[1]++;
    c->bits[0] = lo;
    c->bits[1] += (uint32_t)(n >> 29);
    while (n) {
        size_t k = 64 - have < n ? 64 - have : n;
        memcpy(c->in + have, d, k);
        have += (uint32_t)k; d += k; n -= k;
        if (have == 64) { md5_block(c->buf, c->in); have = 0; }
    }
}

void md5_final(uint8_t out[16], md5_ctx *c)
{
    uint8_t len[8];
    for (int i = 0; i < 4; i++) { len[i] = (uint8_t)(c->bits[0] >> (8 * i)); len[4 + i] = (uint8_t)(c->bits[1] >> (8 * i)); }
    uint8_t pad = 0x80, z = 0;
    md5_update(c, &pad, 1);
    while (((c->bits[0] >> 3) & 63) != 56) md5_update(c, &z, 1);
    md5_update(c, len, 8);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(c->buf[i] >> (8 * j));
}

// ---------------------------------------------------------------- SHA-1
static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_vector(size_t num, const uint8_t *addr[], const size_t *len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t blk[64]; size_t have = 0; uint64_t total = 0;
    for (size_t e = 0; e < num; e++) {
        const uint8_t *d = addr[e]; size_t n = len[e];
        total += n;
        while (n) {
            size_t k = 64 - have < n ? 64 - have : n;
            memcpy(blk + have, d, k); have += k; d += k; n -= k;
            if (have == 64) { sha1_block(h, blk); have = 0; }
        }
    }
    blk[have++] = 0x80;
    if (have > 56) { memset(blk + have, 0, 64 - have); sha1_block(h, blk); have = 0; }
    memset(blk + have, 0, 56 - have);
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++) blk[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_block(h, blk);
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

void hmac_sha1_vec(const uint8_t *key, size_t key_len, size_t num, const uint8_t *addr[], const size_t *len, uint8_t mac[20])
{
    uint8_t k[64], tk[20], pad[64];
    const uint8_t *a[8]; size_t l[8];
    if (num > 6) num = 6;
    if (key_len > 64) { sha1_vector(1, &key, &key_len, tk); key = tk; key_len = 20; }
    memset(k, 0, 64); memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    a[0] = pad; l[0] = 64;
    for (size_t i = 0; i < num; i++) { a[i + 1] = addr[i]; l[i + 1] = len[i]; }
    uint8_t inner[20];
    sha1_vector(num + 1, a, l, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    a[0] = pad; l[0] = 64; a[1] = inner; l[1] = 20;
    sha1_vector(2, a, l, mac);
}

void hmac_md5_buf(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[16])
{
    uint8_t k[64], tk[16], pad[64];
    md5_ctx c;
    if (key_len > 64) { md5_init(&c); md5_update(&c, key, key_len); md5_final(tk, &c); key = tk; key_len = 16; }
    memset(k, 0, 64); memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    md5_init(&c); md5_update(&c, pad, 64); md5_update(&c, data, data_len);
    uint8_t inner[16]; md5_final(inner, &c);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    md5_init(&c); md5_update(&c, pad, 64); md5_update(&c, inner, 16); md5_final(mac, &c);
}

void pbkdf2_sha1_buf(const char *pass, const uint8_t *ssid, size_t ssid_len, int iter, uint8_t *buf, size_t buflen)
{
    size_t plen = strlen(pass);
    uint32_t count = 0;
    while (buflen) {
        count++;
        uint8_t cnt[4] = { (uint8_t)(count >> 24), (uint8_t)(count >> 16), (uint8_t)(count >> 8), (uint8_t)count };
        const uint8_t *a[2] = { ssid, cnt }; size_t l[2] = { ssid_len, 4 };
        uint8_t u[20], t[20];
        hmac_sha1_vec((const uint8_t *)pass, plen, 2, a, l, u);
        memcpy(t, u, 20);
        for (int i = 1; i < iter; i++) {
            const uint8_t *a1[1] = { u }; size_t l1[1] = { 20 };
            hmac_sha1_vec((const uint8_t *)pass, plen, 1, a1, l1, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        size_t k = buflen < 20 ? buflen : 20;
        memcpy(buf, t, k); buf += k; buflen -= k;
    }
}

void sha1_prf_buf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t data_len, uint8_t *buf, size_t buf_len)
{
    uint8_t counter = 0, hash[20];
    size_t label_len = strlen(label) + 1, pos = 0;
    const uint8_t *a[3] = { (const uint8_t *)label, data, &counter };
    size_t l[3] = { label_len, data_len, 1 };
    while (pos < buf_len) {
        size_t plen = buf_len - pos;
        hmac_sha1_vec(key, key_len, 3, a, l, hash);
        memcpy(buf + pos, hash, plen < 20 ? plen : 20);
        pos += plen < 20 ? plen : 20;
        counter++;
    }
}

void rc4_skip_buf(const uint8_t *key, size_t keylen, size_t skip, uint8_t *data, size_t data_len)
{
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + S[i] + key[i % keylen]) & 255;
        uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
    }
    int i = 0, j = 0;
    for (size_t k = 0; k < skip; k++) { i = (i + 1) & 255; j = (j + S[i]) & 255; uint8_t t = S[i]; S[i] = S[j]; S[j] = t; }
    for (size_t k = 0; k < data_len; k++) {
        i = (i + 1) & 255; j = (j + S[i]) & 255;
        uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
        data[k] ^= S[(S[i] + S[j]) & 255];
    }
}
