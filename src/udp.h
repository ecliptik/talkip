/*
 * udp.h - UDP for talkip: a few bound ports, each with a handler called from the main loop (never at
 * interrupt time), and sending through ip_output. Checksums are sent and, when present, verified.
 */
#ifndef TALKIP_UDP_H
#define TALKIP_UDP_H

#include <MacTypes.h>

#define UDP_MAX_PAYLOAD (586 - 20 - 8)
#define UDP_BINDINGS    4

typedef void (*UDPHandler)(unsigned long src, unsigned short sport, unsigned short dport,
                           const unsigned char *data, unsigned short len);

typedef struct UDPStats {
    unsigned long in;
    unsigned long out;
    unsigned long no_port;      /* nothing bound to the destination port */
    unsigned long bad;          /* short, or a wrong checksum */
} UDPStats;

extern UDPStats udp_stats;

OSErr udp_bind(unsigned short port, UDPHandler handler);   /* -1 (fBsyErr) if all bindings are used */
void udp_unbind(unsigned short port);
unsigned short udp_ephemeral_port(void);                   /* a free port in 49152-65535 */
/* Where udp_send expects its payload if the caller builds it in place (no copy); valid until the next send */
unsigned char *udp_payload(void);
OSErr udp_send(unsigned long dst, unsigned short sport, unsigned short dport, const void *data, unsigned short len);
void udp_input(unsigned char *ip, unsigned short hlen, unsigned short total);   /* from ip_input */

#endif
