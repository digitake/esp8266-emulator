#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t buf[4]; uint32_t bits[2]; uint8_t in[64]; } md5_ctx;   // 88 bytes, ROM layout

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const uint8_t *d, size_t n);
void md5_final(uint8_t out[16], md5_ctx *c);
void sha1_vector(size_t num, const uint8_t *addr[], const size_t *len, uint8_t out[20]);
void hmac_sha1_vec(const uint8_t *key, size_t key_len, size_t num, const uint8_t *addr[], const size_t *len, uint8_t mac[20]);
void hmac_md5_buf(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t mac[16]);
void pbkdf2_sha1_buf(const char *pass, const uint8_t *ssid, size_t ssid_len, int iter, uint8_t *buf, size_t buflen);
void sha1_prf_buf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t data_len, uint8_t *buf, size_t buf_len);
void rc4_skip_buf(const uint8_t *key, size_t keylen, size_t skip, uint8_t *data, size_t data_len);
