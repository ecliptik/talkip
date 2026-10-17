/*
 * ddp.h - AppleTalk DDP for talkip: .MPP, our static socket 72, a receive ring filled at interrupt time.
 *
 * The socket listener (asm, in ddp.c) is called by .MPP when a datagram for socket 72 arrives. It copies the
 * datagram into the next free ring slot with ReadRest and marks it full; the main loop takes slots in order
 * with ddp_next() and gives each back with ddp_release() before taking the next. A full ring drops the
 * datagram (counted). Inside Macintosh: Networking ch. 7; Apple TalkTool DDPListener.a.
 * Notes: docs/research/macip-ddp.md.
 */
#ifndef TALKIP_DDP_H
#define TALKIP_DDP_H

#include <MacTypes.h>

#define DDP_MAX_DATA    586     /* largest DDP payload, and so the MacIP MTU */
#define DDP_TYPE_ATP    3
#define DDP_TYPE_IP     22      /* MacIP: an IP datagram, no extra framing */
#define TALKIP_SOCKET   72      /* MacIP clients and gateways both use socket 72 */
#define TALKIP_SOCKET_ALT 73    /* when the TalkIP Driver (.ipp) holds 72: a Lite app gets its own socket and lease */
#define TALKIP_DRIVER_SIG "TkIP" /* after the .ipp driver header's name (offset 24), to tell it from MacTCP */
extern unsigned char ddp_socket;        /* the socket in use: TALKIP_SOCKET or TALKIP_SOCKET_ALT */
#define DDP_RX_SLOTS    8       /* a power of two: the listener masks the index with DDP_RX_SLOTS-1. 8 since
                                 * M7: 4 overflowed while an app redrew (4.8 KB of RAM) */

#define TALKIP_ERR_NOTFOUND (-28001)    /* NBP lookup found nothing */

typedef struct ATAddr {
    unsigned short net;         /* 0 = this network */
    unsigned char node;
    unsigned char socket;
} ATAddr;

/* The asm listener in ddp.c writes these fields by offset: keep the two in step. */
typedef struct DDPRxSlot {
    volatile unsigned char full;    /*  0: nonzero = filled by the listener, not yet released */
    unsigned char type;             /*  1: DDP protocol type */
    unsigned short len;             /*  2: bytes in data */
    unsigned short src_net;         /*  4: for a short header, this network (.MPP's own number) */
    unsigned short dst_net;         /*  6: likewise */
    unsigned char src_node;         /*  8 */
    unsigned char src_socket;       /*  9 */
    unsigned char dst_node;         /* 10: our node, or 0xFF for a broadcast */
    unsigned char pad;              /* 11 */
    unsigned char data[DDP_MAX_DATA]; /* 12 */
} DDPRxSlot;                        /* 598 bytes */

typedef struct DDPStats {
    unsigned long rx;               /* datagrams handed to the main loop */
    unsigned long tx;
    unsigned long tx_err;
} DDPStats;

extern DDPStats ddp_stats;
extern volatile unsigned long ddp_rx_dropped;   /* ring full or oversize (counted by the listener) */
extern volatile unsigned long ddp_rx_calls;     /* listener invocations */
extern volatile unsigned long ddp_rx_errors;    /* ReadRest reported an error (bad frame) */
extern unsigned char ddp_my_node;               /* learned from received datagrams; 0 = not yet */
extern unsigned short ddp_my_net;

Boolean talkip_driver_present(void);
OSErr ddp_open(void);                   /* .MPP + socket 72 */
void ddp_close(void);                   /* closes socket 72: must run before the app quits */
OSErr ddp_write(const ATAddr *dst, unsigned char type, const void *data, unsigned short len);
DDPRxSlot *ddp_next(void);              /* oldest filled slot, or NULL */
void ddp_release(DDPRxSlot *slot);
short ddp_driver_version(void);         /* AppleTalk driver version (SysEnvirons) */

/* NBP lookup of object:type@zone (C strings; "=" and "*" wildcards as usual). Returns noErr with *addr and the
 * first match's object name (up to 32 chars + NUL) in object_out, or TALKIP_ERR_NOTFOUND. Blocks up to ~2 s. */
OSErr nbp_lookup(const char *object, const char *type, const char *zone, ATAddr *addr, char *object_out);

#endif
