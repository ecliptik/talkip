#include <string.h>
#include <Events.h>

#include "udp.h"
#include "ip.h"
#include "macip.h"
#include "bytes.h"

UDPStats udp_stats;

static struct {
    unsigned short port;        /* 0 = free */
    UDPHandler handler;
} bindings[UDP_BINDINGS];

static unsigned short next_ephemeral;

OSErr udp_bind(unsigned short port, UDPHandler handler)
{
    int i, free_slot = -1;

    for (i = 0; i < UDP_BINDINGS; i++) {
        if (bindings[i].port == port) {
            bindings[i].handler = handler;
            return noErr;
        }
        if (bindings[i].port == 0 && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return fBsyErr;
    bindings[free_slot].port = port;
    bindings[free_slot].handler = handler;
    return noErr;
}

void udp_unbind(unsigned short port)
{
    int i;

    for (i = 0; i < UDP_BINDINGS; i++)
        if (bindings[i].port == port)
            bindings[i].port = 0;
}

static Boolean bound(unsigned short port)
{
    int i;

    for (i = 0; i < UDP_BINDINGS; i++)
        if (bindings[i].port == port)
            return true;
    return false;
}

unsigned short udp_ephemeral_port(void)
{
    if (next_ephemeral == 0)
        next_ephemeral = 49152 + (unsigned short)(TickCount() & 0x3FFF);
    do {
        next_ephemeral = next_ephemeral >= 65535 ? 49152 : next_ephemeral + 1;
    } while (bound(next_ephemeral));
    return next_ephemeral;
}

/* The pseudo-header: source and destination address, zero, protocol, UDP length. */
static unsigned long pseudo_sum(unsigned long src, unsigned long dst, unsigned short udp_len)
{
    unsigned char ph[12];

    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IP_PROTO_UDP;
    put16(ph + 10, udp_len);
    return ip_sum(0, ph, sizeof(ph));
}

unsigned char *udp_payload(void)
{
    return ip_payload() + 8;
}

OSErr udp_send(unsigned long dst, unsigned short sport, unsigned short dport, const void *data, unsigned short len)
{
    unsigned char *u = ip_payload();
    unsigned short ulen = 8 + len;
    unsigned short ck;
    OSErr err;

    if (len > UDP_MAX_PAYLOAD)
        return paramErr;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, ulen);
    put16(u + 6, 0);
    if (data != u + 8)                  /* callers may build the payload in place (udp_payload) */
        memcpy(u + 8, data, len);
    ck = ip_fold(ip_sum(pseudo_sum(macip_cfg.ip, dst, ulen), u, ulen));
    put16(u + 6, ck ? ck : 0xFFFF);     /* 0 means "no checksum" in UDP */
    err = ip_output(dst, IP_PROTO_UDP, ulen);
    if (err == noErr)
        udp_stats.out++;
    return err;
}

void udp_input(unsigned char *ip, unsigned short hlen, unsigned short total)
{
    unsigned char *u = ip + hlen;
    unsigned short ulen, dport;
    int i;

    if (total - hlen < 8) {
        udp_stats.bad++;
        return;
    }
    ulen = get16(u + 4);
    if (ulen < 8 || ulen > total - hlen) {
        udp_stats.bad++;
        return;
    }
    if (get16(u + 6) != 0 && ip_fold(ip_sum(pseudo_sum(get32(ip + 12), get32(ip + 16), ulen), u, ulen)) != 0) {
        udp_stats.bad++;
        return;
    }
    dport = get16(u + 2);
    for (i = 0; i < UDP_BINDINGS; i++) {
        if (bindings[i].port == dport && bindings[i].handler) {
            udp_stats.in++;
            bindings[i].handler(get32(ip + 12), get16(u), dport, u + 8, ulen - 8);
            return;
        }
    }
    udp_stats.no_port++;
}
