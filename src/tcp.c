/*
 * tcp.c - TCP (RFC 793/9293 subset) for talkip; see tcp.h for the shape and the reasons.
 *
 * Send side: sbuf holds bytes from snd_base (the sequence number of sbuf[0]) onward: the unacknowledged
 * segment, if any, then data not yet sent. Only one segment is ever in flight (snd_nxt == snd_una before the
 * next is sent), which also suits ClassicStack's gateway: it doesn't check sequence numbers, so overlapping
 * retransmits would corrupt the stream there.
 * Receive side: in-order data goes straight into the caller's ring; anything else is dropped and acked.
 */
#include <stdint.h>
#include <string.h>
#include <Events.h>

#include "tcp.h"
#include "ip.h"
#include "macip.h"
#include "bytes.h"

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

/* Sequence numbers are 32-bit modular (uint32_t, not unsigned long, which is 64-bit on host tests). */
typedef uint32_t seq_t;
#define SEQ_LT(a, b)    ((int32_t)((seq_t)(a) - (seq_t)(b)) < 0)
#define SEQ_LEQ(a, b)   ((int32_t)((seq_t)(a) - (seq_t)(b)) <= 0)
#define SEQ_GT(a, b)    ((int32_t)((seq_t)(a) - (seq_t)(b)) > 0)
#define SEQ_GEQ(a, b)   ((int32_t)((seq_t)(a) - (seq_t)(b)) >= 0)

#define RTO_INITIAL     (3 * 60)
#define RTO_MIN         60
#define RTO_MAX         (60L * 60)
#define TIME_WAIT_TICKS (10 * 60)

typedef struct TCB {
    unsigned char state;
    OSErr err;
    unsigned long rip;
    unsigned short rport, lport;
    seq_t iss, snd_una, snd_nxt, snd_base, rcv_nxt;
    unsigned short snd_wnd, mss;
    unsigned char sbuf[TCP_SNDBUF];
    unsigned short slen;                /* bytes in sbuf (unacked + unsent) */
    Boolean fin_wanted, fin_sent, fin_acked, fin_rcvd, ack_now;
    unsigned char *rbuf;
    unsigned short rsize, rhead, rcount;
    unsigned short last_wnd;            /* window we last advertised */
    unsigned long timer_at;             /* retransmit (or TIME_WAIT) deadline; 0 = none */
    unsigned long rto, srtt, rttvar;    /* ticks; srtt and rttvar scaled by 8 and 4 */
    seq_t rtt_seq;                      /* the segment being timed */
    unsigned long rtt_start;            /* when (0 = none) */
    short retries;
} TCB;

TkTCPStats tcp_stats;

static TCB tcbs[TCP_MAX_CONNS];
static unsigned short next_port;

const char *tcp_state_name(short s)
{
    static const char *names[] = { "closed", "connecting", "open", "closing", "closing", "peer closed",
                                   "closing", "closing", "time-wait", "listening", "connecting" };
    return (s >= 0 && s <= TCP_SYN_RCVD) ? names[s] : "?";
}

static TCB *get(short c)
{
    return (c >= 0 && c < TCP_MAX_CONNS && tcbs[c].rbuf) ? &tcbs[c] : NULL;
}

static unsigned short rcv_window(TCB *t)
{
    unsigned short room = t->rsize - t->rcount;

    return room > TCP_MAX_WINDOW ? TCP_MAX_WINDOW : room;
}

static unsigned long pseudo_sum(unsigned long src, unsigned long dst, unsigned short len)
{
    unsigned char ph[12];

    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IP_PROTO_TCP;
    put16(ph + 10, len);
    return ip_sum(0, ph, sizeof(ph));
}

/* Build and send one segment. */
static void emit(unsigned long rip, unsigned short lport, unsigned short rport, seq_t seq,
                 seq_t ack, unsigned char flags, unsigned short wnd, const unsigned char *data,
                 unsigned short len, Boolean mss_option)
{
    unsigned char *h = ip_payload();
    unsigned short hlen = mss_option ? 24 : 20;

    put16(h, lport);
    put16(h + 2, rport);
    put32(h + 4, seq);
    put32(h + 8, ack);
    h[12] = (unsigned char)((hlen / 4) << 4);
    h[13] = flags;
    put16(h + 14, wnd);
    put16(h + 16, 0);
    put16(h + 18, 0);
    if (mss_option) {
        h[20] = 2;                      /* MSS */
        h[21] = 4;
        put16(h + 22, TCP_MSS);
    }
    if (len)
        memcpy(h + hlen, data, len);
    put16(h + 16, ip_fold(ip_sum(pseudo_sum(macip_cfg.ip, rip, hlen + len), h, hlen + len)));
    if (ip_output(rip, IP_PROTO_TCP, hlen + len) == noErr)
        tcp_stats.segs_out++;
}

static void send_ack(TCB *t)
{
    t->last_wnd = rcv_window(t);
    emit(t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, ACK, t->last_wnd, NULL, 0, false);
    t->ack_now = false;
}

static void arm_timer(TCB *t)
{
    t->timer_at = TickCount() + t->rto;
    if (t->timer_at == 0)
        t->timer_at = 1;
}

static void set_closed(TCB *t, OSErr err)
{
    t->state = TCP_CLOSED;
    t->err = err;
    t->timer_at = 0;
}

static void send_syn(TCB *t)
{
    t->last_wnd = rcv_window(t);
    if (t->state == TCP_SYN_RCVD)       /* passive open: SYN-ACK */
        emit(t->rip, t->lport, t->rport, t->iss, t->rcv_nxt, SYN | ACK, t->last_wnd, NULL, 0, true);
    else
        emit(t->rip, t->lport, t->rport, t->iss, 0, SYN, t->last_wnd, NULL, 0, true);
}

/* Send whatever is due: the next data segment, our FIN, or a bare ACK. One segment in flight at most. */
static void output(TCB *t)
{
    Boolean sent = false;
    seq_t in_flight = t->snd_nxt - t->snd_una;

    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT && t->state != TCP_FIN_WAIT_1
        && t->state != TCP_LAST_ACK && t->state != TCP_CLOSING) {
        if (t->ack_now && t->state != TCP_SYN_SENT && t->state != TCP_SYN_RCVD && t->state != TCP_LISTEN
            && t->state != TCP_CLOSED)
            send_ack(t);
        return;
    }
    if (in_flight == 0 && t->slen > 0 && t->snd_wnd > 0 && !t->fin_sent) {
        unsigned short n = t->slen;

        if (n > t->mss)
            n = t->mss;
        if (n > t->snd_wnd)
            n = t->snd_wnd;
        t->last_wnd = rcv_window(t);
        emit(t->rip, t->lport, t->rport, t->snd_base, t->rcv_nxt, ACK | PSH, t->last_wnd, t->sbuf, n, false);
        t->snd_nxt = t->snd_base + n;
        t->rtt_seq = t->snd_nxt;
        t->rtt_start = TickCount();
        arm_timer(t);
        sent = true;
    } else if (in_flight == 0 && t->slen == 0 && t->fin_wanted && !t->fin_sent) {
        t->last_wnd = rcv_window(t);
        emit(t->rip, t->lport, t->rport, t->snd_base, t->rcv_nxt, FIN | ACK, t->last_wnd, NULL, 0, false);
        t->snd_nxt = t->snd_base + 1;
        t->fin_sent = true;
        if (t->state == TCP_ESTABLISHED)
            t->state = TCP_FIN_WAIT_1;
        else if (t->state == TCP_CLOSE_WAIT)
            t->state = TCP_LAST_ACK;
        arm_timer(t);
        sent = true;
    } else if (in_flight == 0 && t->slen > 0 && t->snd_wnd == 0 && t->timer_at == 0) {
        arm_timer(t);                   /* zero window: the timer sends a 1-byte probe */
    }
    if (sent)
        t->ack_now = false;
    else if (t->ack_now)
        send_ack(t);
}

static void retransmit(TCB *t)
{
    tcp_stats.retransmits++;
    t->rtt_start = 0;                   /* Karn: don't time a retransmitted segment */
    if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) {
        send_syn(t);
    } else if (t->snd_nxt != t->snd_una || (t->slen && t->snd_wnd == 0)) {
        if (t->fin_sent && t->slen == 0) {
            emit(t->rip, t->lport, t->rport, t->snd_base, t->rcv_nxt, FIN | ACK, rcv_window(t), NULL, 0, false);
        } else {
            unsigned short n = t->slen;
            if (t->snd_nxt != t->snd_una && n > t->snd_nxt - t->snd_base)
                n = (unsigned short)(t->snd_nxt - t->snd_base);
            if (n > t->mss)
                n = t->mss;
            if (t->snd_wnd == 0 && n > 1)
                n = 1;                  /* window probe */
            emit(t->rip, t->lport, t->rport, t->snd_base, t->rcv_nxt, ACK | PSH, rcv_window(t), t->sbuf, n, false);
            if (SEQ_GT(t->snd_base + n, t->snd_nxt))
                t->snd_nxt = t->snd_base + n;
        }
    } else {
        t->timer_at = 0;
        return;
    }
    t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
    arm_timer(t);
}

void tcp_poll(void)
{
    unsigned long now = TickCount();
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        TCB *t = &tcbs[i];

        if (!t->rbuf || t->state == TCP_CLOSED)
            continue;
        if (t->state == TCP_TIME_WAIT) {
            if (t->timer_at && (long)(now - t->timer_at) >= 0)
                set_closed(t, noErr);
            continue;
        }
        if (t->timer_at && (long)(now - t->timer_at) >= 0) {
            if (++t->retries > TCP_MAX_RETRIES) {
                emit(t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, RST | ACK, 0, NULL, 0, false);
                set_closed(t, TCP_ERR_TIMEOUT);
                continue;
            }
            retransmit(t);
        }
        /* a window update after the app made room, so the server doesn't stall on a full buffer */
        if ((t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2)
            && rcv_window(t) >= t->last_wnd + t->mss)
            t->ack_now = true;
        output(t);
    }
}

short tcp_open(unsigned long ip, unsigned short port, unsigned char *rcvbuf, unsigned short rcvlen)
{
    short c;
    TCB *t = NULL;

    if (macip_state != MACIP_UP)
        return TCP_ERR_CLOSED;
    for (c = 0; c < TCP_MAX_CONNS; c++) {
        if (tcbs[c].rbuf == NULL) {
            t = &tcbs[c];
            break;
        }
    }
    if (t == NULL || rcvbuf == NULL || rcvlen == 0)
        return TCP_ERR_NOCONN;
    memset(t, 0, sizeof(*t));
    if (next_port == 0)
        next_port = 49152 + (unsigned short)(TickCount() & 0x3FFF);
    next_port = next_port >= 65535 ? 49152 : next_port + 1;
    t->lport = next_port;
    t->rip = ip;
    t->rport = port;
    t->rbuf = rcvbuf;
    t->rsize = rcvlen;
    t->mss = TCP_MSS;
    t->iss = (seq_t)(TickCount() * 2654435761UL);
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->snd_base = t->iss + 1;
    t->rto = RTO_INITIAL;
    t->state = TCP_SYN_SENT;
    send_syn(t);
    t->rtt_start = TickCount();
    t->rtt_seq = t->iss + 1;
    arm_timer(t);
    return c;
}

short tcp_listen(unsigned short port, unsigned char *rcvbuf, unsigned short rcvlen)
{
    short c;
    TCB *t = NULL;

    for (c = 0; c < TCP_MAX_CONNS; c++) {
        if (tcbs[c].rbuf == NULL) {
            t = &tcbs[c];
            break;
        }
    }
    if (t == NULL || rcvbuf == NULL || rcvlen == 0 || port == 0)
        return TCP_ERR_NOCONN;
    memset(t, 0, sizeof(*t));
    t->lport = port;
    t->rbuf = rcvbuf;
    t->rsize = rcvlen;
    t->mss = TCP_MSS;
    t->rto = RTO_INITIAL;
    t->state = TCP_LISTEN;
    return c;
}

void tcp_remote(short c, unsigned long *ip, unsigned short *port)
{
    TCB *t = get(c);

    *ip = t ? t->rip : 0;
    *port = t ? t->rport : 0;
}

short tcp_state(short c)
{
    TCB *t = get(c);

    return t ? t->state : TCP_CLOSED;
}

OSErr tcp_error(short c)
{
    TCB *t = get(c);

    return t ? t->err : TCP_ERR_NOCONN;
}

unsigned short tcp_send_space(short c)
{
    TCB *t = get(c);

    if (!t || t->fin_wanted || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT
                                && t->state != TCP_SYN_SENT && t->state != TCP_SYN_RCVD))
        return 0;
    return TCP_SNDBUF - t->slen;
}

unsigned short tcp_send(short c, const void *data, unsigned short len)
{
    TCB *t = get(c);
    unsigned short room = tcp_send_space(c);

    if (!t || room == 0)
        return 0;
    if (len > room)
        len = room;
    memcpy(t->sbuf + t->slen, data, len);
    t->slen += len;
    output(t);
    return len;
}

unsigned short tcp_unsent(short c)
{
    TCB *t = get(c);

    return t ? t->slen : 0;
}

unsigned short tcp_unread(short c)
{
    TCB *t = get(c);

    return t ? t->rcount : 0;
}

unsigned short tcp_recv(short c, void *buf, unsigned short max)
{
    TCB *t = get(c);
    unsigned char *out = buf;
    unsigned short n, first;

    if (!t || t->rcount == 0)
        return 0;
    n = max < t->rcount ? max : t->rcount;
    first = t->rsize - t->rhead;
    if (first > n)
        first = n;
    memcpy(out, t->rbuf + t->rhead, first);
    memcpy(out + first, t->rbuf, n - first);
    t->rhead = (unsigned short)((t->rhead + n) % t->rsize);
    t->rcount -= n;
    return n;
}

Boolean tcp_peer_closed(short c)
{
    TCB *t = get(c);

    return t ? t->fin_rcvd : true;
}

void tcp_close(short c)
{
    TCB *t = get(c);

    if (!t)
        return;
    if (t->state == TCP_SYN_SENT || t->state == TCP_LISTEN || t->state == TCP_SYN_RCVD) {
        set_closed(t, noErr);
        return;
    }
    t->fin_wanted = true;
    output(t);
}

void tcp_abort(short c)
{
    TCB *t = get(c);

    if (!t || t->state == TCP_CLOSED)
        return;
    if (t->state != TCP_TIME_WAIT && t->state != TCP_LISTEN)
        emit(t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, RST | ACK, 0, NULL, 0, false);
    set_closed(t, TCP_ERR_CLOSED);
}

void tcp_release(short c)
{
    TCB *t = get(c);

    if (!t)
        return;
    if (t->state != TCP_CLOSED && t->state != TCP_TIME_WAIT)
        tcp_abort(c);
    t->rbuf = NULL;                     /* a TIME_WAIT connection just goes: its port won't be reused soon */
    t->state = TCP_CLOSED;
}

/* A segment for no connection: answer with RST (RFC 9293 3.10.7.1), never to a RST. */
static void reset_reply(unsigned long src, unsigned short sport, unsigned short dport, seq_t seq,
                        seq_t ack, unsigned char flags, unsigned short len)
{
    if (flags & RST)
        return;
    tcp_stats.resets_sent++;
    if (flags & ACK)
        emit(src, dport, sport, ack, 0, RST, 0, NULL, 0, false);
    else
        emit(src, dport, sport, 0, seq + len + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0),
             RST | ACK, 0, NULL, 0, false);
}

static void rtt_sample(TCB *t, unsigned long m)
{
    if (m == 0)
        m = 1;
    if (t->srtt == 0) {
        t->srtt = m << 3;
        t->rttvar = m << 1;
    } else {
        long err = (long)m - (long)(t->srtt >> 3);
        t->srtt += err;
        if (err < 0)
            err = -err;
        t->rttvar += err - (long)(t->rttvar >> 2);
    }
    t->rto = (t->srtt >> 3) + t->rttvar;
    if (t->rto < RTO_MIN)
        t->rto = RTO_MIN;
    if (t->rto > RTO_MAX)
        t->rto = RTO_MAX;
}

static void mss_option(TCB *t, const unsigned char *opt, unsigned short olen)
{
    while (olen > 0) {
        if (opt[0] == 0)
            break;
        if (opt[0] == 1) {
            opt++;
            olen--;
            continue;
        }
        if (olen < 2 || opt[1] < 2 || opt[1] > olen)
            break;
        if (opt[0] == 2 && opt[1] == 4) {
            unsigned short m = get16(opt + 2);
            if (m >= 64 && m < t->mss)
                t->mss = m;
        }
        olen -= opt[1];
        opt += opt[1];
    }
}

void tcp_input(unsigned char *ip, unsigned short hlen, unsigned short total)
{
    unsigned char *h = ip + hlen;
    unsigned short len = total - hlen, thl, dlen, sport, dport, wnd;
    unsigned long src = get32(ip + 12);
    seq_t seq, ack;
    unsigned char flags;
    unsigned char *data;
    TCB *t = NULL;
    int i;

    if (len < 20 || ip_fold(ip_sum(pseudo_sum(src, get32(ip + 16), len), h, len)) != 0) {
        tcp_stats.bad++;
        return;
    }
    thl = (h[12] >> 4) * 4;
    if (thl < 20 || thl > len) {
        tcp_stats.bad++;
        return;
    }
    tcp_stats.segs_in++;
    sport = get16(h);
    dport = get16(h + 2);
    seq = get32(h + 4);
    ack = get32(h + 8);
    flags = h[13];
    wnd = get16(h + 14);
    data = h + thl;
    dlen = len - thl;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        TCB *c = &tcbs[i];
        if (c->rbuf && c->state != TCP_CLOSED && c->lport == dport && c->rport == sport && c->rip == src) {
            t = c;
            break;
        }
    }
    if (t == NULL && (flags & (SYN | ACK | RST)) == SYN) {
        /* A SYN for a listening port: take the connection (passive open) */
        for (i = 0; i < TCP_MAX_CONNS; i++) {
            TCB *c = &tcbs[i];
            if (c->rbuf && c->state == TCP_LISTEN && c->lport == dport) {
                t = c;
                break;
            }
        }
        if (t && macip_state == MACIP_UP) {
            t->rip = src;
            t->rport = sport;
            mss_option(t, h + 20, thl - 20);
            t->rcv_nxt = seq + 1;
            t->iss = (seq_t)(TickCount() * 2654435761UL);
            t->snd_una = t->iss;
            t->snd_nxt = t->iss + 1;
            t->snd_base = t->iss + 1;
            t->snd_wnd = wnd;
            t->state = TCP_SYN_RCVD;
            send_syn(t);
            t->rtt_start = TickCount();
            t->rtt_seq = t->iss + 1;
            arm_timer(t);
            return;
        }
        t = NULL;
    }
    if (t == NULL) {
        reset_reply(src, sport, dport, seq, ack, flags, dlen);
        return;
    }

    if (t->state == TCP_SYN_RCVD) {
        if (flags & RST) {              /* the peer gave up: listen again */
            t->state = TCP_LISTEN;
            t->rip = 0;
            t->rport = 0;
            t->timer_at = 0;
            return;
        }
        if (flags & SYN) {              /* our SYN-ACK was lost */
            send_syn(t);
            return;
        }
        if (!(flags & ACK) || ack != t->iss + 1)
            return;
        t->snd_una = ack;
        t->snd_wnd = wnd;
        t->state = TCP_ESTABLISHED;
        t->timer_at = 0;
        t->retries = 0;
        if (t->rtt_start) {
            rtt_sample(t, TickCount() - t->rtt_start);
            t->rtt_start = 0;
        }
        /* carry on: the ACK may bring data */
    }

    if (t->state == TCP_SYN_SENT) {
        if ((flags & ACK) && ack != t->iss + 1) {
            reset_reply(src, sport, dport, seq, ack, flags, dlen);
            return;
        }
        if (flags & RST) {
            if (flags & ACK)
                set_closed(t, TCP_ERR_REFUSED);
            return;
        }
        if ((flags & SYN) && (flags & ACK)) {
            mss_option(t, h + 20, thl - 20);
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_nxt = ack;
            t->snd_base = ack;
            t->snd_wnd = wnd;
            t->state = TCP_ESTABLISHED;
            t->timer_at = 0;
            t->retries = 0;
            if (t->rtt_start) {         /* the SYN wasn't retransmitted: a valid RTT sample (Karn) */
                rtt_sample(t, TickCount() - t->rtt_start);
                t->rtt_start = 0;
            }
            t->ack_now = true;
            output(t);
        }
        return;                         /* a bare SYN (simultaneous open) isn't supported */
    }

    /* Acceptability (RFC 9293 3.10.7.4, simplified for a small window): RST must be in the window. */
    if (flags & RST) {
        if (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + (rcv_window(t) ? rcv_window(t) : 1)))
            set_closed(t, t->state == TCP_TIME_WAIT ? noErr : TCP_ERR_RESET);
        return;
    }
    if (flags & SYN) {                  /* a retransmitted SYN-ACK: our ACK was lost */
        t->ack_now = true;
        output(t);
        return;
    }
    if (!(flags & ACK))
        return;

    /* ACK */
    if (SEQ_GT(ack, t->snd_una) && SEQ_LEQ(ack, t->snd_nxt)) {
        seq_t acked_data = ack - t->snd_base;

        if (acked_data > t->slen)
            acked_data = t->slen;       /* the rest acknowledges our FIN */
        if (acked_data) {
            memmove(t->sbuf, t->sbuf + acked_data, t->slen - acked_data);
            t->slen -= (unsigned short)acked_data;
            t->snd_base += acked_data;
        }
        if (t->fin_sent && ack == t->snd_nxt)
            t->fin_acked = true;
        if (t->rtt_start && SEQ_GEQ(ack, t->rtt_seq) && t->retries == 0) {
            rtt_sample(t, TickCount() - t->rtt_start);
            t->rtt_start = 0;
        }
        t->snd_una = ack;
        t->retries = 0;
        t->timer_at = 0;
        if (t->snd_una != t->snd_nxt)
            arm_timer(t);
    }
    if (SEQ_LEQ(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_nxt))
        t->snd_wnd = wnd;

    if (t->fin_acked) {
        if (t->state == TCP_FIN_WAIT_1)
            t->state = TCP_FIN_WAIT_2;
        else if (t->state == TCP_CLOSING) {
            t->state = TCP_TIME_WAIT;
            t->timer_at = TickCount() + TIME_WAIT_TICKS;
        } else if (t->state == TCP_LAST_ACK) {
            set_closed(t, noErr);
            return;
        }
    }

    /* An empty segment that isn't at rcv_nxt (an old one, or a zero-window probe) gets an ACK, as RFC 9293
     * says for unacceptable segments: otherwise a lost window update deadlocks a full receive buffer. */
    if (dlen == 0 && !(flags & FIN) && seq != t->rcv_nxt)
        t->ack_now = true;

    /* Data, in order only */
    if (dlen && (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2)) {
        if (SEQ_LEQ(seq, t->rcv_nxt) && SEQ_GT(seq + dlen, t->rcv_nxt)) {
            unsigned short skip = (unsigned short)(t->rcv_nxt - seq);
            unsigned short n = dlen - skip, room = t->rsize - t->rcount, k;
            unsigned short pos = (unsigned short)((t->rhead + t->rcount) % t->rsize);

            if (n > room)
                n = room;
            for (k = 0; k < n; k++) {
                t->rbuf[pos] = data[skip + k];
                if (++pos == t->rsize)
                    pos = 0;
            }
            t->rcount += n;
            t->rcv_nxt += n;
            if (skip + n < dlen)
                flags &= ~FIN;          /* the FIN comes after bytes we couldn't take */
        } else {
            flags &= ~FIN;              /* out of order or old: drop, ack what we have */
        }
        t->ack_now = true;
    }

    /* FIN, once everything before it has been taken */
    if ((flags & FIN) && seq + dlen == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++;
        t->fin_rcvd = true;
        t->ack_now = true;
        if (t->state == TCP_ESTABLISHED) {
            t->state = TCP_CLOSE_WAIT;
        } else if (t->state == TCP_FIN_WAIT_1) {
            t->state = t->fin_acked ? TCP_TIME_WAIT : TCP_CLOSING;
        } else if (t->state == TCP_FIN_WAIT_2) {
            t->state = TCP_TIME_WAIT;
        }
        if (t->state == TCP_TIME_WAIT)
            t->timer_at = TickCount() + TIME_WAIT_TICKS;
    } else if ((flags & FIN) && t->fin_rcvd) {
        t->ack_now = true;              /* a retransmitted FIN: our ACK was lost */
    }

    output(t);
}

unsigned short tcp_local_port(short c)
{
    TCB *t = get(c);

    return t ? t->lport : 0;
}

unsigned short tcp_peer_window(short c)
{
    TCB *t = get(c);

    return t ? t->snd_wnd : 0;
}

unsigned short tcp_our_window(short c)
{
    TCB *t = get(c);

    return t ? rcv_window(t) : 0;
}
