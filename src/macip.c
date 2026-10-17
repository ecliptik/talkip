/*
 * macip.c - MacIP client. Byte layouts and gateway behaviour: docs/research/macip-ddp.md, section 1.
 *
 * ASSIGN is sent as a raw ATP TReq (at-least-once, DDP type 3) from socket 72 rather than through .ATP, whose
 * PSendRequest would use a dynamic socket: macipgw maps our IP to the requesting socket until our first IP
 * datagram. macipgw leases a fresh address on every ASSIGN, so it is sent once and retried only on timeout.
 *
 * Liveness: anything from the gateway counts as hearing from it. macipgw pings an idle client every ~30 s
 * (ip.c answers, which keeps its lease); ClassicStack never sends unsolicited and expires a lease after 5 min
 * without traffic. So after PROBE_IDLE_TICKS without hearing from the gateway we probe it (see probe()),
 * which both gateways answer (and which renews a ClassicStack lease). PROBE_TRIES unanswered probes mean the
 * gateway is gone or has moved (macipgw sits on atalkd's dynamic node, which can change when atalkd restarts):
 * then look it up again and re-ASSIGN, retrying with backoff until it's back. A failed first lookup goes the
 * same way (macip_retry_later): right after boot the node may not have heard a router yet (RTMP every 10 s),
 * so the first NBP lookup can miss a gateway on another network (seen in Snow, 2026-10-15).
 */
#include <string.h>
#include <Events.h>
#include <OSUtils.h>

#include "macip.h"
#include "ip.h"

#define MACIP_ASSIGN_FN     1
#define MACIP_SERVER_FN     3
#define ATP_TREQ_ALO        0x40
#define ATP_TRESP           0x80
#define ATP_FUNC_MASK       0xC0
#define ASSIGN_TRIES        3
#define ASSIGN_WAIT_TICKS   (4 * 60)
#define PROBE_IDLE_TICKS    (120L * 60)
#define PROBE_WAIT_TICKS    (10L * 60)
#define PROBE_TRIES         3
#define RETRY_FIRST_TICKS   (5L * 60)   /* backoff 5, 10, 20, 40 s, then every minute */
#define RETRY_MAX_TICKS     (60L * 60)

MacIPConfig macip_cfg;
MacIPState macip_state = MACIP_DOWN;
MacIPStats macip_stats;
char macip_error[64];
unsigned long macip_last_heard;     /* TickCount when the gateway last sent us anything */

static unsigned short next_tid;
static unsigned long probe_sent;
static short probes_out;            /* unanswered probes; 0 = none outstanding */
static Boolean recovering;
static unsigned long retry_at;
static unsigned long backoff = RETRY_FIRST_TICKS;

static unsigned long get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

static void set_error(const char *s)
{
    strncpy(macip_error, s, sizeof(macip_error) - 1);
    macip_error[sizeof(macip_error) - 1] = '\0';
}

OSErr macip_find_gateway(void)
{
    OSErr err;

    macip_state = MACIP_LOOKUP;
    err = nbp_lookup("=", "IPGATEWAY", "*", &macip_cfg.gateway, macip_cfg.gateway_name);
    if (err != noErr) {
        macip_state = MACIP_FAILED;
        set_error(err == TALKIP_ERR_NOTFOUND ? "No IPGATEWAY in this zone" : "NBP lookup failed");
    }
    return err;
}

static OSErr send_treq(unsigned short tid, long function)
{
    unsigned char req[12];

    req[0] = ATP_TREQ_ALO;
    req[1] = 0x01;                      /* bitmap: one response packet */
    req[2] = tid >> 8;
    req[3] = tid & 0xFF;
    req[4] = 0;                         /* user bytes: version 1 */
    req[5] = 1;
    req[6] = 0;
    req[7] = 0;
    req[8] = 0;                         /* function, int32 big-endian */
    req[9] = 0;
    req[10] = 0;
    req[11] = (unsigned char)function;
    return ddp_write(&macip_cfg.gateway, DDP_TYPE_ATP, req, sizeof(req));
}

static Boolean from_gateway(const DDPRxSlot *s)
{
    return s->src_node == macip_cfg.gateway.node
        && (s->src_net == macip_cfg.gateway.net || macip_cfg.gateway.net == 0);
}

/* Handle one received datagram that isn't the answer we're waiting for. */
static void dispatch(DDPRxSlot *s)
{
    if (from_gateway(s))
        macip_last_heard = TickCount();
    if (s->type == DDP_TYPE_IP)
        ip_input(s->data, s->len);
    else if (s->type == DDP_TYPE_ATP)
        macip_stats.atp_in++;
    else
        macip_stats.other_in++;
}

OSErr macip_assign(void)
{
    unsigned short tid;
    int tries;

    macip_state = MACIP_ASSIGN;
    macip_cfg.ip = 0;
    tid = ++next_tid ^ (unsigned short)TickCount();

    for (tries = 0; tries < ASSIGN_TRIES; tries++) {
        unsigned long deadline;
        OSErr err = send_treq(tid, MACIP_ASSIGN_FN);

        if (err != noErr) {
            macip_state = MACIP_FAILED;
            set_error("writeDDP failed");
            return err;
        }
        deadline = TickCount() + ASSIGN_WAIT_TICKS;
        while (TickCount() < deadline) {
            DDPRxSlot *s = ddp_next();
            const unsigned char *d;
            long function;

            if (s == NULL) {
#ifndef TALKIP_RESIDENT
                SystemTask();
#endif
                continue;
            }
            d = s->data;
            if (s->type != DDP_TYPE_ATP || s->len < 24 || (d[0] & ATP_FUNC_MASK) != ATP_TRESP
                || d[2] != (tid >> 8) || d[3] != (tid & 0xFF)) {
                dispatch(s);
                ddp_release(s);
                continue;
            }
            /* TResp: user bytes 4-7, then function, IP, nameserver, broadcast, pad, [netmask] */
            function = (long)get32(d + 8);
            if (function == -1) {
                set_error(s->len > 44 ? (const char *)d + 44 : "Gateway refused");
                ddp_release(s);
                macip_state = MACIP_FAILED;
                return TALKIP_ERR_GATEWAY;
            }
            macip_cfg.ip = get32(d + 12);
            macip_cfg.nameserver = get32(d + 16);
            macip_cfg.broadcast = get32(d + 20);
            macip_cfg.netmask = s->len >= 32 ? get32(d + 28) : 0;
            ddp_release(s);
            if (macip_cfg.ip == 0) {
                set_error("Gateway has no free address");
                macip_state = MACIP_FAILED;
                return TALKIP_ERR_GATEWAY;
            }
            macip_state = MACIP_UP;
            macip_error[0] = '\0';
            macip_last_heard = TickCount();
            probes_out = 0;
            recovering = false;
            backoff = RETRY_FIRST_TICKS;
            return noErr;
        }
    }
    macip_state = MACIP_FAILED;
    set_error("No answer from the gateway");
    return TALKIP_ERR_TIMEOUT;
}

/*
 * The probe is an ICMP echo to the gateway's own address (its NBP name), not an ATP request: it tests the
 * whole IP path, and macipgw re-learns our AppleTalk address from every IP datagram we send, so a restarted
 * macipgw (which forgot us) can reach us again. ClassicStack answers pings to its gateway address too. If the
 * name isn't an address, fall back to a SERVER request.
 */
#define PROBE_ICMP_ID 0x7A1C

static void probe(void)
{
    unsigned long gw_ip = ip_parse(macip_cfg.gateway_name);

    probe_sent = TickCount();
    probes_out++;
    macip_stats.keepalives++;
    if (gw_ip != 0)
        ip_send_echo(gw_ip, PROBE_ICMP_ID, (unsigned short)macip_stats.keepalives, NULL, 0);
    else
        send_treq(++next_tid, MACIP_SERVER_FN);
}

void macip_poll(void)
{
    DDPRxSlot *s;
    unsigned long now;

    while ((s = ddp_next()) != NULL) {
        dispatch(s);
        ddp_release(s);
    }
    now = TickCount();

    if (macip_state == MACIP_UP) {
        if (probes_out == 0) {
            if (now - macip_last_heard > PROBE_IDLE_TICKS)
                probe();
        } else if (macip_last_heard >= probe_sent) {
            probes_out = 0;                     /* answered */
        } else if (now - probe_sent > PROBE_WAIT_TICKS) {
            if (probes_out < PROBE_TRIES) {
                probe();
            } else {
                macip_state = MACIP_FAILED;
                set_error("Gateway stopped answering");
                recovering = true;
                backoff = RETRY_FIRST_TICKS;
                retry_at = now + 1;             /* next poll: the app sees (and logs) this state first */
            }
        }
    }

    if (recovering && now >= retry_at) {
        /* blocks a few seconds: NBP lookup, then ASSIGN */
        macip_stats.recoveries++;
        if (macip_find_gateway() != noErr || macip_assign() != noErr)
            macip_retry_later();
    }
}

void macip_retry_later(void)
{
    recovering = true;
    retry_at = TickCount() + backoff;
    backoff = backoff * 2 > RETRY_MAX_TICKS ? RETRY_MAX_TICKS : backoff * 2;
}

Boolean macip_recovering(void)
{
    return recovering;
}

OSErr macip_send(const void *packet, unsigned short len)
{
    if (macip_state != MACIP_UP)
        return TALKIP_ERR_DOWN;
    return ddp_write(&macip_cfg.gateway, DDP_TYPE_IP, packet, len);
}
