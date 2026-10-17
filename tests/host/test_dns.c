/*
 * Host test: the real dns.c, udp.c and ip.c against a real DNS server. Only the MacIP layer is replaced:
 * macip_send takes the finished IP datagram, checks its header and UDP checksums, and sends the UDP payload
 * from a real socket; macip_poll wraps each reply in an IP/UDP header (with a correct checksum) and feeds it
 * to ip_input, as the receive ring would.
 *   make -C tests/host && tests/host/test_dns <your DNS server> floodgap.com 1.1.1.1 nonexistent.invalid
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "dns.h"
#include "udp.h"
#include "ip.h"
#include "macip.h"
#include "bytes.h"

MacIPConfig macip_cfg;
MacIPState macip_state = MACIP_UP;

static int sock;
static unsigned short last_sport;
static int failures;

unsigned long TickCount(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 60 + (unsigned long)(t.tv_nsec / 16666667);
}

void SystemTask(void) {}

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

OSErr macip_send(const void *packet, unsigned short len)
{
    const unsigned char *ip = packet;
    struct sockaddr_in to;
    unsigned short ulen;
    unsigned char ph[12];

    check(len >= 28 && ip[0] == 0x45 && ip[9] == IP_PROTO_UDP, "outgoing is IPv4/UDP");
    check(ip_checksum(ip, 20) == 0, "outgoing IP header checksum");
    ulen = get16(ip + 24);
    memcpy(ph, ip + 12, 8);
    ph[8] = 0;
    ph[9] = 17;
    put16(ph + 10, ulen);
    check(ip_fold(ip_sum(ip_sum(0, ph, 12), ip + 20, ulen)) == 0, "outgoing UDP checksum");
    last_sport = get16(ip + 20);
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(get16(ip + 22));
    to.sin_addr.s_addr = htonl(get32(ip + 16));
    sendto(sock, ip + 28, ulen - 8, 0, (struct sockaddr *)&to, sizeof(to));
    return noErr;
}

void macip_poll(void)
{
    static unsigned char pkt[620];
    unsigned char buf[600], ph[12];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    struct timeval tv = {0, 20000};
    ssize_t n;
    unsigned short ulen;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0)
        return;
    if (n > 586 - 28) {
        printf("  (reply of %zd bytes would not fit the MacIP MTU; truncating)\n", n);
        n = 586 - 28;
    }
    ulen = 8 + (unsigned short)n;
    memset(pkt, 0, 28);
    pkt[0] = 0x45;
    put16(pkt + 2, 20 + ulen);
    pkt[8] = 64;
    pkt[9] = IP_PROTO_UDP;
    put32(pkt + 12, ntohl(from.sin_addr.s_addr));
    put32(pkt + 16, macip_cfg.ip);
    put16(pkt + 10, ip_checksum(pkt, 20));
    put16(pkt + 20, ntohs(from.sin_port));
    put16(pkt + 22, last_sport);
    put16(pkt + 24, ulen);
    memcpy(pkt + 28, buf, n);
    memcpy(ph, pkt + 12, 8);
    ph[8] = 0;
    ph[9] = 17;
    put16(ph + 10, ulen);
    put16(pkt + 26, ip_fold(ip_sum(ip_sum(0, ph, 12), pkt + 20, ulen)));
    ip_input(pkt, 20 + ulen);
}

static void show(const char *name)
{
    DNSResult r;
    unsigned short i;

    if (dns_begin(name) != noErr) {
        printf("%s: dns_begin failed\n", name);
        failures++;
        return;
    }
    while (dns_poll(&r) == DNS_PENDING)
        macip_poll();
    printf("%s: %s (%d), %lu ms, tries %d%s\n", name, dns_error_text(r.err), r.err,
           r.ticks * 1000 / 60, r.tries, r.cached ? ", cached" : "");
    for (i = 0; i < r.naddrs; i++)
        printf("  A %lu.%lu.%lu.%lu ttl %lu\n", r.addr[i] >> 24, (r.addr[i] >> 16) & 255,
               (r.addr[i] >> 8) & 255, r.addr[i] & 255, r.ttl);
    if (r.cname[0])
        printf("  CNAME %s\n", r.cname);
    if (r.ptr[0])
        printf("  PTR %s\n", r.ptr);
}

int main(int argc, char **argv)
{
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <nameserver> <name>...\n", argv[0]);
        return 2;
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    macip_cfg.ip = 0xC0A84802;      /* 192.168.72.2, as a lease would be */
    macip_cfg.nameserver = ip_parse(argv[1]);
    for (i = 2; i < argc; i++)
        show(argv[i]);
    printf("again, from the cache:\n");
    show(argv[2]);
    printf(failures ? "%d FAILURES\n" : "ok\n", failures);
    return failures != 0;
}
