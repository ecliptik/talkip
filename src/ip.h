/*
 * ip.h - IPv4 input for talkip. M1: header checks and ICMP echo replies (which also keep macipgw's lease).
 * TCP and UDP arrive with uIP in later milestones.
 */
#ifndef TALKIP_IP_H
#define TALKIP_IP_H

#include <MacTypes.h>

typedef struct IPStats {
    unsigned long rx;               /* datagrams for us */
    unsigned long tx;
    unsigned long bad;              /* malformed or bad header checksum */
    unsigned long not_ours;         /* addressed elsewhere */
    unsigned long echo_replies;     /* pings answered */
    unsigned long echo_in;          /* echo replies received (our pings and probes) */
    unsigned long unhandled;        /* protocols we don't speak yet */
} IPStats;

/* Called from ip_input (main loop, not interrupt time) for each ICMP echo reply addressed to us. */
typedef void (*IPEchoReplyHook)(unsigned long src, unsigned short id, unsigned short seq,
                                const unsigned char *data, unsigned short len);

extern IPStats ip_stats;
extern IPEchoReplyHook ip_echo_reply_hook;

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17
#define IP_MAX_PAYLOAD  (586 - 20)  /* MacIP MTU (the DDP maximum) less the IP header */

void ip_input(unsigned char *packet, unsigned short len);  /* may reuse packet for a reply */

/* Sending: write the upper-layer header and data at ip_payload() (word-aligned, IP_MAX_PAYLOAD bytes), then
 * ip_output() adds the IP header and sends it through the gateway. One buffer: build and send one at a time. */
unsigned char *ip_payload(void);
OSErr ip_output(unsigned long dst, unsigned char proto, unsigned short payload_len);

/* Ones' complement sums: ip_sum accumulates (for pseudo-headers), ip_fold finishes; ip_checksum = both. */
unsigned long ip_sum(unsigned long sum, const void *data, unsigned short len);
unsigned short ip_fold(unsigned long sum);
/* ICMP echo request; data may be NULL when len is 0. Needs a lease. */
OSErr ip_send_echo(unsigned long dst, unsigned short id, unsigned short seq, const void *data, unsigned short len);
unsigned long ip_parse(const char *dotted);                 /* "a.b.c.d" -> address, 0 if malformed */
unsigned short ip_checksum(const void *data, unsigned short len);  /* ones' complement, network order */

#endif
