/*
 * ip.c - IPv4: validate input and dispatch it (ICMP here, UDP to udp.c), answer ICMP echo in place (packets
 * come from the receive ring, at an even address), and send: upper layers write their header and data
 * straight into ip_payload() and call ip_output(), so nothing is copied twice.
 */
#include <string.h>

#include "ip.h"
#include "udp.h"
#include "tcp.h"
#include "macip.h"
#include "bytes.h"

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO       8

IPStats ip_stats;
IPEchoReplyHook ip_echo_reply_hook;

static unsigned short ip_ident;
static unsigned short out_buf[DDP_MAX_DATA / 2];        /* word-aligned for the 68000 */

unsigned long ip_sum(unsigned long sum, const void *data, unsigned short len)
{
    const unsigned char *p = data;

    while (len > 1) {
        sum += ((unsigned short)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (unsigned short)p[0] << 8;
    return sum;
}

unsigned short ip_fold(unsigned long sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

unsigned short ip_checksum(const void *data, unsigned short len)
{
    return ip_fold(ip_sum(0, data, len));
}

unsigned char *ip_payload(void)
{
    return (unsigned char *)out_buf + 20;
}

OSErr ip_output(unsigned long dst, unsigned char proto, unsigned short payload_len)
{
    unsigned char *ip = (unsigned char *)out_buf;
    unsigned short total = 20 + payload_len;
    OSErr err;

    if (total > DDP_MAX_DATA)
        return paramErr;
    ip[0] = 0x45;                       /* IPv4, 20-byte header */
    ip[1] = 0;
    put16(ip + 2, total);
    put16(ip + 4, ++ip_ident);
    put16(ip + 6, 0);                   /* no fragmentation flags */
    ip[8] = 64;                         /* TTL */
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, macip_cfg.ip);
    put32(ip + 16, dst);
    put16(ip + 10, ip_checksum(ip, 20));
    err = macip_send(ip, total);
    if (err == noErr)
        ip_stats.tx++;
    return err;
}

static void icmp_input(unsigned char *ip, unsigned short hlen, unsigned short total)
{
    unsigned char *icmp = ip + hlen;
    unsigned short icmp_len = total - hlen;
    unsigned char tmp[4];
    int i;

    if (icmp_len < 8 || ip_checksum(icmp, icmp_len) != 0) {
        ip_stats.bad++;
        return;
    }
    if (icmp[0] == ICMP_ECHO_REPLY) {
        ip_stats.echo_in++;
        if (ip_echo_reply_hook)
            ip_echo_reply_hook(get32(ip + 12), get16(icmp + 4), get16(icmp + 6), icmp + 8, icmp_len - 8);
        return;
    }
    if (icmp[0] != ICMP_ECHO) {
        ip_stats.unhandled++;
        return;
    }
    /* Echo reply in place: swap addresses, fresh TTL, new checksums. */
    for (i = 0; i < 4; i++) {
        tmp[i] = ip[12 + i];
        ip[12 + i] = ip[16 + i];
        ip[16 + i] = tmp[i];
    }
    ip[8] = 64;
    put16(ip + 10, 0);
    put16(ip + 10, ip_checksum(ip, hlen));
    icmp[0] = ICMP_ECHO_REPLY;
    put16(icmp + 2, 0);
    put16(icmp + 2, ip_checksum(icmp, icmp_len));
    if (macip_send(ip, total) == noErr) {
        ip_stats.tx++;
        ip_stats.echo_replies++;
    }
}

unsigned long ip_parse(const char *s)
{
    unsigned long ip = 0;
    int part;

    for (part = 0; part < 4; part++) {
        unsigned int v = 0, digits = 0;

        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (unsigned int)(*s++ - '0');
            digits++;
        }
        if (digits == 0 || v > 255)
            return 0;
        ip = (ip << 8) | v;
        if (part < 3 && *s++ != '.')
            return 0;
    }
    return *s == '\0' ? ip : 0;
}

OSErr ip_send_echo(unsigned long dst, unsigned short id, unsigned short seq, const void *data, unsigned short len)
{
    unsigned char *icmp = ip_payload();

    if (20 + 8 + len > DDP_MAX_DATA)
        return paramErr;
    icmp[0] = ICMP_ECHO;
    icmp[1] = 0;
    put16(icmp + 2, 0);
    put16(icmp + 4, id);
    put16(icmp + 6, seq);
    if (len)
        memcpy(icmp + 8, data, len);
    put16(icmp + 2, ip_checksum(icmp, 8 + len));
    return ip_output(dst, IP_PROTO_ICMP, 8 + len);
}

void ip_input(unsigned char *ip, unsigned short len)
{
    unsigned short hlen, total;
    unsigned long dst;

    if (len < 20 || (ip[0] >> 4) != 4) {
        ip_stats.bad++;
        return;
    }
    hlen = (ip[0] & 0x0F) * 4;
    total = get16(ip + 2);
    if (hlen < 20 || total < hlen || total > len || ip_checksum(ip, hlen) != 0) {
        ip_stats.bad++;
        return;
    }
    dst = get32(ip + 16);
    if (dst != macip_cfg.ip && dst != macip_cfg.broadcast && dst != 0xFFFFFFFFUL) {
        ip_stats.not_ours++;
        return;
    }
    if ((ip[6] & 0x3F) != 0 || ip[7] != 0) {    /* fragment (MF set or offset): not reassembled */
        ip_stats.unhandled++;
        return;
    }
    ip_stats.rx++;
    if (ip[9] == IP_PROTO_ICMP && dst == macip_cfg.ip)
        icmp_input(ip, hlen, total);
    else if (ip[9] == IP_PROTO_UDP)
        udp_input(ip, hlen, total);
    else if (ip[9] == IP_PROTO_TCP && dst == macip_cfg.ip)
        tcp_input(ip, hlen, total);
    else
        ip_stats.unhandled++;
}
