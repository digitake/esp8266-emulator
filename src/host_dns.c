#include "host_dns.h"

#include <stdio.h>
#include <string.h>

#define HOST_DNS_MAX_ENTRIES 16
#define ETHERNET_HEADER_SIZE 14
#define IPV4_HEADER_SIZE 20
#define UDP_HEADER_SIZE 8
#define DNS_HEADER_SIZE 12
#define ETHERNET_FRAME_MAX 1518

typedef struct {
    char name[254];
    uint8_t address[4];
} host_dns_entry_t;

static host_dns_entry_t entries[HOST_DNS_MAX_ENTRIES];
static size_t entry_count;

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static uint16_t checksum(const uint8_t *data, size_t len, uint32_t sum)
{
    while (len >= 2) {
        sum += read_u16(data);
        data += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void error_text(char *err, size_t size, const char *message)
{
    if (err && size) snprintf(err, size, "%s", message);
}

static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool hostname_normalize(const char *input, char output[254])
{
    if (!input) return false;
    size_t length = strlen(input);
    if (length && input[length - 1] == '.') length--;
    if (!length || length > 253) return false;

    size_t label_length = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '.') {
            if (!label_length || output[i - 1] == '-') return false;
            label_length = 0;
            output[i] = '.';
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-')) return false;
        if (!label_length && c == '-') return false;
        label_length++;
        if (label_length > 63) return false;
        output[i] = (char)ascii_lower(c);
    }
    if (!label_length || output[length - 1] == '-') return false;
    output[length] = '\0';
    return true;
}

static bool ipv4_parse(const char *input, uint8_t output[4])
{
    if (!input) return false;
    const char *p = input;
    for (unsigned i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return false;
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        output[i] = (uint8_t)value;
        if (i < 3) {
            if (*p++ != '.') return false;
        } else if (*p != '\0') {
            return false;
        }
    }
    return true;
}

bool host_dns_add(const char *name, const char *ipv4, char *err, size_t err_size)
{
    char normalized[254];
    uint8_t address[4];
    if (!hostname_normalize(name, normalized)) {
        error_text(err, err_size, "invalid DNS hostname");
        return false;
    }
    if (!ipv4_parse(ipv4, address)) {
        error_text(err, err_size, "invalid IPv4 address");
        return false;
    }
    for (size_t i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, normalized) == 0) {
            error_text(err, err_size, "duplicate DNS hostname");
            return false;
        }
    }
    if (entry_count == HOST_DNS_MAX_ENTRIES) {
        error_text(err, err_size, "too many DNS overrides");
        return false;
    }
    strcpy(entries[entry_count].name, normalized);
    memcpy(entries[entry_count].address, address, sizeof address);
    entry_count++;
    if (err && err_size) err[0] = '\0';
    return true;
}

void host_dns_clear(void)
{
    memset(entries, 0, sizeof entries);
    entry_count = 0;
}

static bool query_name(const uint8_t *dns, size_t dns_len, char name[254], size_t *question_end)
{
    if (dns_len < DNS_HEADER_SIZE + 5) return false;
    uint16_t flags = read_u16(dns + 2);
    if ((flags & 0x8000u) || (flags & 0x7800u) || read_u16(dns + 4) != 1) return false;

    size_t pos = DNS_HEADER_SIZE;
    size_t output_len = 0;
    size_t label_len = 0;
    for (;;) {
        if (pos >= dns_len) return false;
        uint8_t count = dns[pos++];
        if (count == 0) {
            if (!label_len || output_len == 0) return false;
            break;
        }
        if ((count & 0xc0u) || count > 63 || pos + count > dns_len) return false;
        if (output_len && output_len < 254) name[output_len++] = '.';
        label_len = count;
        for (unsigned i = 0; i < count; i++) {
            unsigned char c = dns[pos++];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-')) return false;
            if ((i == 0 && c == '-') || (i + 1 == (unsigned)count && c == '-')) return false;
            if (output_len >= 253) return false;
            name[output_len++] = (char)ascii_lower(c);
        }
        if (output_len > 253) return false;
        label_len = count;
    }
    (void)label_len;
    name[output_len] = '\0';
    if (pos + 4 > dns_len || read_u16(dns + pos) != 1 || read_u16(dns + pos + 2) != 1)
        return false;
    *question_end = pos + 4;
    return true;
}

static const host_dns_entry_t *lookup(const char *name)
{
    for (size_t i = 0; i < entry_count; i++)
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

size_t host_dns_reply(const uint8_t *ethernet, size_t len, uint8_t *reply, size_t cap)
{
    if (!ethernet || !reply || len < ETHERNET_HEADER_SIZE + IPV4_HEADER_SIZE + UDP_HEADER_SIZE + DNS_HEADER_SIZE ||
        len > ETHERNET_FRAME_MAX) return 0;
    if (read_u16(ethernet + 12) != 0x0800) return 0;

    const uint8_t *ip = ethernet + ETHERNET_HEADER_SIZE;
    if ((ip[0] >> 4) != 4) return 0;
    size_t ip_header_len = (size_t)(ip[0] & 0x0f) * 4;
    if (ip_header_len < IPV4_HEADER_SIZE || ip_header_len > len - ETHERNET_HEADER_SIZE) return 0;
    size_t ip_len = read_u16(ip + 2);
    if (ip_len < ip_header_len + UDP_HEADER_SIZE + DNS_HEADER_SIZE ||
        ip_len > len - ETHERNET_HEADER_SIZE || ip[9] != 17 ||
        checksum(ip, ip_header_len, 0) != 0) return 0;
    if (read_u16(ip + 6) & 0xbfffu) return 0; // No fragments or reserved flag; DF alone is fine.
    static const uint8_t dns_ip[4] = {10, 0, 2, 3};
    if (memcmp(ip + 16, dns_ip, sizeof dns_ip) != 0) return 0;

    const uint8_t *udp = ip + ip_header_len;
    size_t udp_len = read_u16(udp + 4);
    if (read_u16(udp + 2) != 53 || udp_len < UDP_HEADER_SIZE + DNS_HEADER_SIZE ||
        udp_len > ip_len - ip_header_len) return 0;
    uint16_t udp_checksum = read_u16(udp + 6);
    if (udp_checksum) {
        uint32_t udp_sum = 0;
        udp_sum += read_u16(ip + 12);
        udp_sum += read_u16(ip + 14);
        udp_sum += read_u16(ip + 16);
        udp_sum += read_u16(ip + 18);
        udp_sum += 17;
        udp_sum += (uint16_t)udp_len;
        if (checksum(udp, udp_len, udp_sum) != 0) return 0;
    }
    const uint8_t *dns = udp + UDP_HEADER_SIZE;
    size_t dns_len = udp_len - UDP_HEADER_SIZE;
    char name[254];
    size_t question_end;
    if (!query_name(dns, dns_len, name, &question_end)) return 0;
    const host_dns_entry_t *entry = lookup(name);
    if (!entry) return 0;

    size_t response_dns_len = question_end + 16;
    size_t response_udp_len = UDP_HEADER_SIZE + response_dns_len;
    size_t response_ip_len = IPV4_HEADER_SIZE + response_udp_len;
    size_t response_len = ETHERNET_HEADER_SIZE + response_ip_len;
    if (response_len > cap || response_len > ETHERNET_FRAME_MAX) return 0;

    uint8_t frame[ETHERNET_FRAME_MAX];
    memset(frame, 0, response_len);
    memcpy(frame, ethernet + 6, 6);
    memcpy(frame + 6, ethernet, 6);
    write_u16(frame + 12, 0x0800);

    uint8_t *out_ip = frame + ETHERNET_HEADER_SIZE;
    out_ip[0] = 0x45;
    write_u16(out_ip + 2, (uint16_t)response_ip_len);
    write_u16(out_ip + 4, read_u16(ip + 4));
    write_u16(out_ip + 6, 0x4000);
    out_ip[8] = 64;
    out_ip[9] = 17;
    memcpy(out_ip + 12, ip + 16, 4);
    memcpy(out_ip + 16, ip + 12, 4);
    write_u16(out_ip + 10, checksum(out_ip, IPV4_HEADER_SIZE, 0));

    uint8_t *out_udp = out_ip + IPV4_HEADER_SIZE;
    write_u16(out_udp, 53);
    write_u16(out_udp + 2, read_u16(udp));
    write_u16(out_udp + 4, (uint16_t)response_udp_len);
    uint8_t *out_dns = out_udp + UDP_HEADER_SIZE;
    memcpy(out_dns, dns, question_end);
    uint16_t query_flags = read_u16(dns + 2);
    write_u16(out_dns + 2, (uint16_t)(0x8400u | (query_flags & 0x0100u) | 0x0080u));
    write_u16(out_dns + 4, 1);
    write_u16(out_dns + 6, 1);
    write_u16(out_dns + 8, 0);
    write_u16(out_dns + 10, 0);
    size_t answer = question_end;
    out_dns[answer++] = 0xc0;
    out_dns[answer++] = 0x0c;
    write_u16(out_dns + answer, 1); answer += 2;
    write_u16(out_dns + answer, 1); answer += 2;
    out_dns[answer++] = 0;
    out_dns[answer++] = 0;
    out_dns[answer++] = 0;
    out_dns[answer++] = 60;
    write_u16(out_dns + answer, 4); answer += 2;
    memcpy(out_dns + answer, entry->address, 4);

    uint32_t udp_sum = 0;
    udp_sum += read_u16(out_ip + 12);
    udp_sum += read_u16(out_ip + 14);
    udp_sum += read_u16(out_ip + 16);
    udp_sum += read_u16(out_ip + 18);
    udp_sum += 17;
    udp_sum += (uint16_t)response_udp_len;
    write_u16(out_udp + 6, checksum(out_udp, response_udp_len, udp_sum));
    if (read_u16(out_udp + 6) == 0) write_u16(out_udp + 6, 0xffff);

    memcpy(reply, frame, response_len);
    return response_len;
}
