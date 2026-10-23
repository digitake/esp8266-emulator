#include "host_dns.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static uint16_t checksum(const uint8_t *data, size_t len, uint32_t sum)
{
    while (len >= 2) {
        sum += get16(data);
        data += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t make_query(uint8_t *frame, const char *name, uint16_t type)
{
    memset(frame, 0, 1518);
    for (unsigned i = 0; i < 6; i++) {
        frame[i] = (uint8_t)(0x10 + i);
        frame[6 + i] = (uint8_t)(0x20 + i);
    }
    put16(frame + 12, 0x0800);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, (uint8_t[]){10, 0, 2, 15}, 4);
    memcpy(ip + 16, (uint8_t[]){10, 0, 2, 3}, 4);

    uint8_t *udp = ip + 20;
    put16(udp, 41000);
    put16(udp + 2, 53);
    uint8_t *dns = udp + 8;
    put16(dns, 0x1234);
    put16(dns + 2, 0x0100); // RD
    put16(dns + 4, 1);
    size_t p = 12;
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t n = dot ? (size_t)(dot - label) : strlen(label);
        assert(n > 0 && n <= 63 && p + n + 1 < 512);
        dns[p++] = (uint8_t)n;
        memcpy(dns + p, label, n);
        p += n;
        if (!dot) break;
        label = dot + 1;
    }
    dns[p++] = 0;
    put16(dns + p, type); p += 2;
    put16(dns + p, 1); p += 2;
    size_t udp_len = 8 + p;
    size_t ip_len = 20 + udp_len;
    put16(udp + 4, (uint16_t)udp_len);
    put16(ip + 2, (uint16_t)ip_len);
    put16(ip + 10, checksum(ip, 20, 0));
    return 14 + ip_len;
}

static uint32_t pseudo_sum(const uint8_t *ip, size_t udp_len)
{
    return get16(ip + 12) + get16(ip + 14) + get16(ip + 16) + get16(ip + 18) + 17 + (uint16_t)udp_len;
}

static void fix_ip_checksum(uint8_t *frame)
{
    uint8_t *ip = frame + 14;
    size_t header_len = (size_t)(ip[0] & 0x0f) * 4;
    put16(ip + 10, 0);
    put16(ip + 10, checksum(ip, header_len, 0));
}

static void fix_udp_checksum(uint8_t *frame)
{
    uint8_t *ip = frame + 14;
    size_t header_len = (size_t)(ip[0] & 0x0f) * 4;
    uint8_t *udp = ip + header_len;
    size_t udp_len = get16(udp + 4);
    put16(udp + 6, 0);
    uint16_t value = checksum(udp, udp_len, pseudo_sum(ip, udp_len));
    put16(udp + 6, value ? value : 0xffff);
}

static void test_mapping_and_checksums(void)
{
    char err[80];
    uint8_t query[1518], reply[1518];
    size_t query_len = make_query(query, "Broker.Example.Test", 1);
    assert(host_dns_add("broker.example.test.", "10.0.2.2", err, sizeof err));
    assert(err[0] == '\0');
    size_t reply_len = host_dns_reply(query, query_len, reply, sizeof reply);
    assert(reply_len == query_len + 16);
    assert(!memcmp(reply, query + 6, 6));
    assert(!memcmp(reply + 6, query, 6));

    const uint8_t *ip = reply + 14;
    assert(ip[0] == 0x45 && ip[9] == 17 && get16(ip + 2) == reply_len - 14);
    assert(checksum(ip, 20, 0) == 0);
    assert(!memcmp(ip + 12, (uint8_t[]){10, 0, 2, 3}, 4));
    assert(!memcmp(ip + 16, (uint8_t[]){10, 0, 2, 15}, 4));

    const uint8_t *udp = ip + 20;
    assert(get16(udp) == 53 && get16(udp + 2) == 41000);
    assert(get16(udp + 4) == reply_len - 14 - 20);
    assert(checksum(udp, get16(udp + 4), pseudo_sum(ip, get16(udp + 4))) == 0);
    const uint8_t *dns = udp + 8;
    assert(get16(dns) == 0x1234);
    assert(get16(dns + 2) == 0x8580); // QR, AA, RD, RA
    assert(get16(dns + 4) == 1 && get16(dns + 6) == 1);
    size_t answer = 12;
    while (dns[answer]) answer += 1 + dns[answer];
    answer += 1 + 4; // root terminator and QTYPE/QCLASS
    assert(dns[answer] == 0xc0 && dns[answer + 1] == 0x0c);
    answer += 2;
    assert(get16(dns + answer) == 1 && get16(dns + answer + 2) == 1);
    assert(get16(dns + answer + 8) == 4);
    assert(!memcmp(dns + answer + 10, (uint8_t[]){10, 0, 2, 2}, 4));
    assert(host_dns_reply(query, query_len, reply, reply_len - 1) == 0);
}

static void test_rejected_mappings(void)
{
    char err[80];
    host_dns_clear();
    assert(!host_dns_add("", "10.0.2.2", err, sizeof err));
    assert(!host_dns_add("a..test", "10.0.2.2", err, sizeof err));
    assert(!host_dns_add("-bad.test", "10.0.2.2", err, sizeof err));
    assert(!host_dns_add("bad-.test", "10.0.2.2", err, sizeof err));
    assert(!host_dns_add("bad name.test", "10.0.2.2", err, sizeof err));
    assert(!host_dns_add("x.test", "256.0.0.1", err, sizeof err));
    assert(!host_dns_add("x.test", "1.2.3", err, sizeof err));
    assert(host_dns_add("x.test", "1.2.3.4", err, sizeof err));
    assert(!host_dns_add("X.TEST.", "1.2.3.4", err, sizeof err)); // normalized duplicate
    for (unsigned i = 0; i < 15; i++) {
        char name[32];
        snprintf(name, sizeof name, "host%u.test", i);
        assert(host_dns_add(name, "1.2.3.4", err, sizeof err));
    }
    assert(!host_dns_add("seventeenth.test", "1.2.3.4", err, sizeof err));
    host_dns_clear();
}

static void test_passthrough_and_bounds(void)
{
    uint8_t query[1518], reply[1518], oversized[1519];
    size_t len = make_query(query, "broker.example.test", 1);
    char err[80];
    host_dns_clear();
    assert(host_dns_add("broker.example.test", "10.0.2.2", err, sizeof err));

    for (size_t n = 0; n < len; n++)
        assert(host_dns_reply(query, n, reply, sizeof reply) == 0);
    memset(oversized, 0, sizeof oversized);
    assert(host_dns_reply(oversized, sizeof oversized, reply, sizeof reply) == 0);
    assert(host_dns_reply(NULL, len, reply, sizeof reply) == 0);
    assert(host_dns_reply(query, len, NULL, sizeof reply) == 0);

    uint8_t saved = query[12];
    query[12] = 0x86; // non-IPv4 ethertype
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    query[12] = saved;

    query[14 + 16 + 3] = 4; // another DNS destination
    fix_ip_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    query[14 + 16 + 3] = 3;
    fix_ip_checksum(query);
    put16(query + 14 + 6, 0x2000); // first fragment
    fix_ip_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 6, 0x0001); // nonzero fragment offset
    fix_ip_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 6, 0x8000); // reserved fragment flag
    fix_ip_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 6, 0x4000); // DF is allowed
    fix_ip_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) > 0);
    put16(query + 14 + 6, 0);
    fix_ip_checksum(query);

    query[14 + 8] ^= 1; // corrupt IPv4 header checksum (TTL changed)
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    query[14 + 8] ^= 1;
    fix_ip_checksum(query);

    fix_udp_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) > 0);
    query[14 + 20 + 6] ^= 1; // nonzero but invalid UDP checksum
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    fix_udp_checksum(query);
    assert(host_dns_reply(query, len, reply, sizeof reply) > 0);

    assert(host_dns_reply(query, len - 1, reply, sizeof reply) == 0); // truncated DNS question
    put16(query + 14 + 20 + 8 + 2, 0x8100); // response bit set
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 20 + 8 + 2, 0x0900); // unsupported opcode
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 20 + 8 + 2, 0x0100);
    put16(query + 14 + 20 + 8 + 4, 2); // unsupported question count
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 20 + 8 + 4, 1);
    put16(query + 14 + 20 + 8 + 12 + 1 + 6 + 1 + 7 + 1 + 4 + 1, 28); // AAAA question
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);

    len = make_query(query, "unmatched.example.test", 1);
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    len = make_query(query, "broker.example.test", 1);
    put16(query + 14 + 2, 20); // invalid IPv4 total length
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    put16(query + 14 + 2, (uint16_t)(len - 14));
    put16(query + 14 + 20 + 4, 7); // UDP shorter than header
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    host_dns_clear();
}

static void test_local_fallback(void)
{
    char err[128];
    uint8_t query[1518], reply[1518];
    assert(!host_dns_set_fallback("10.0.2", err, sizeof err));
    assert(host_dns_add("broker.example.test", "10.0.2.9", err, sizeof err));
    assert(host_dns_set_fallback("10.0.2.2", err, sizeof err));
    size_t len = make_query(query, "broker.example.test", 1); // an override still wins
    size_t reply_len = host_dns_reply(query, len, reply, sizeof reply);
    assert(reply_len && !memcmp(reply + reply_len - 4, (uint8_t[]){10, 0, 2, 9}, 4));
    len = make_query(query, "anything.example.com", 1);       // every other name leads to the host
    reply_len = host_dns_reply(query, len, reply, sizeof reply);
    assert(reply_len && !memcmp(reply + reply_len - 4, (uint8_t[]){10, 0, 2, 2}, 4));
    len = make_query(query, "anything.example.com", 28);      // AAAA: not answered...
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0);
    assert(!host_dns_frame_is_local(query, len));              // ...and not let through to the resolver
    host_dns_clear();
    len = make_query(query, "anything.example.com", 1);
    assert(host_dns_reply(query, len, reply, sizeof reply) == 0); // clear removes the fallback
}

static void test_local_frames(void)
{
    uint8_t frame[1518];
    size_t len = make_query(frame, "x.test", 1);
    uint8_t *dst = frame + 14 + 16;
    static const uint8_t allowed[][4] = {{10, 0, 2, 2}, {10, 0, 2, 15}, {255, 255, 255, 255}, {224, 0, 0, 251}};
    static const uint8_t refused[][4] = {{10, 0, 2, 3}, {8, 8, 8, 8}, {10, 0, 3, 2}, {192, 168, 1, 1}, {127, 0, 0, 1}};
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        memcpy(dst, allowed[i], 4);
        assert(host_dns_frame_is_local(frame, len));
    }
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        memcpy(dst, refused[i], 4);
        assert(!host_dns_frame_is_local(frame, len));
    }
    put16(frame + 12, 0x0806);                 // ARP
    assert(host_dns_frame_is_local(frame, 42));
    put16(frame + 12, 0x86dd);                 // IPv6 could route around the IPv4 checks
    assert(!host_dns_frame_is_local(frame, len));
    put16(frame + 12, 0x0800);
    assert(!host_dns_frame_is_local(frame, 20)); // truncated IPv4
    assert(!host_dns_frame_is_local(NULL, len));
}

int main(void)
{
    host_dns_clear();
    test_mapping_and_checksums();
    test_rejected_mappings();
    test_passthrough_and_bounds();
    test_local_fallback();
    test_local_frames();
    puts("host DNS override tests passed");
    return 0;
}
