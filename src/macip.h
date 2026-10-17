/*
 * macip.h - MacIP client: find the gateway (NBP =:IPGATEWAY@*), lease an IP (ATP ASSIGN, built by hand and
 * sent from socket 72 so the gateway maps our IP to it), carry IP datagrams as DDP type 22 to the gateway,
 * keep the lease alive. draft-ietf-appleip-MacIP-02; gateways: netatalk macipgw, ClassicStack.
 */
#ifndef TALKIP_MACIP_H
#define TALKIP_MACIP_H

#include "ddp.h"

#define TALKIP_ERR_TIMEOUT  (-28002)    /* no answer from the gateway */
#define TALKIP_ERR_GATEWAY  (-28003)    /* the gateway refused (see macip_error) */
#define TALKIP_ERR_DOWN     (-28004)    /* no lease */

typedef enum {
    MACIP_DOWN,
    MACIP_LOOKUP,
    MACIP_ASSIGN,
    MACIP_UP,
    MACIP_FAILED
} MacIPState;

typedef struct MacIPConfig {
    unsigned long ip;
    unsigned long nameserver;
    unsigned long broadcast;
    unsigned long netmask;          /* 0 if the gateway didn't send one */
    ATAddr gateway;
    char gateway_name[33];          /* NBP object: the gateway's own IP, dotted */
} MacIPConfig;

typedef struct MacIPStats {
    unsigned long keepalives;       /* SERVER probes sent after hearing nothing from the gateway */
    unsigned long recoveries;       /* look-up-and-re-ASSIGN attempts after the gateway went quiet */
    unsigned long atp_in;           /* ATP datagrams seen after the lease (probe answers) */
    unsigned long other_in;         /* other DDP types: ignored */
} MacIPStats;

extern MacIPConfig macip_cfg;
extern MacIPState macip_state;
extern MacIPStats macip_stats;
extern char macip_error[64];
extern unsigned long macip_last_heard;  /* TickCount of the gateway's last datagram */

OSErr macip_find_gateway(void);     /* blocking, ~2 s; needs ddp_open() first */
OSErr macip_assign(void);           /* blocking, up to ~12 s; leases macip_cfg.ip */
void macip_poll(void);              /* from the event loop: incoming datagrams, probes, recovery (may block) */
Boolean macip_recovering(void);     /* the gateway went quiet and we're looking for it again */
void macip_retry_later(void);       /* after a failed lookup or ASSIGN: macip_poll retries with backoff */
OSErr macip_send(const void *packet, unsigned short len);   /* an IP datagram, header included */

#endif
