/*
 * tcp.h - TCP for talkip: client connections (active open), polled, non-blocking.
 *
 * Shaped for a 512 KB Mac on LocalTalk and for the MacTCP-style shim on top (tcpshim, M4):
 *   - one segment in flight each way: we send the next segment only when the last is acknowledged, and
 *     advertise at most TCP_MAX_WINDOW, so a sender can't overrun talkip's 8-slot DDP receive ring;
 *   - the receive buffer is the caller's (like MacTCP's TCPCreate), the send buffer is TCP_SNDBUF inside;
 *   - every call returns at once; tcp_poll() (via ak_net_poll) runs the timers.
 * Retransmission: RTO from a smoothed RTT (Jacobson/Karn), 1 s minimum, 3 s initial, doubling, give up
 * after TCP_MAX_RETRIES. Passive open takes one connection per listen (MacTCP's TCPPassiveOpen); no urgent data,
 * no out-of-order queue (in-order only, acked).
 */
#ifndef TCP_TALKIP_H
#define TCP_TALKIP_H

#include <MacTypes.h>

#define TCP_MAX_CONNS   2
#define TCP_MSS         546         /* 586-byte MacIP MTU - 40 */
#ifdef TALKIP_RESIDENT
#define TCP_SNDBUF      TCP_MSS     /* the .ipp driver: one segment, the most ever in flight (saves 2 x 478 bytes) */
#else
#define TCP_SNDBUF      1024
#endif
#define TCP_MAX_WINDOW  1092        /* two segments */
#define TCP_MAX_RETRIES 7

#define TCP_ERR_NOCONN      (-28020)    /* no free connection slot / bad handle */
#define TCP_ERR_REFUSED     (-28021)    /* the server reset the connection attempt */
#define TCP_ERR_RESET       (-28022)    /* the connection was reset */
#define TCP_ERR_TIMEOUT     (-28023)    /* no acknowledgement after all retries */
#define TCP_ERR_CLOSED      (-28024)    /* the connection is closed */

enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
    TCP_LISTEN,                         /* passive open: waiting for a SYN */
    TCP_SYN_RCVD
};

typedef struct TkTCPStats {
    unsigned long segs_in, segs_out, retransmits, bad, resets_sent;
} TkTCPStats;

extern TkTCPStats tcp_stats;

/* Start a connection; rcvbuf (rcvlen bytes) holds received data until tcp_recv. Returns a handle >= 0. */
short tcp_open(unsigned long ip, unsigned short port, unsigned char *rcvbuf, unsigned short rcvlen);
short tcp_state(short c);
/* Wait for one connection on port (TCP_LISTEN, then TCP_SYN_RCVD, then TCP_ESTABLISHED). Returns a handle >= 0. */
short tcp_listen(unsigned short port, unsigned char *rcvbuf, unsigned short rcvlen);
void tcp_remote(short c, unsigned long *ip, unsigned short *port);
OSErr tcp_error(short c);                       /* why it closed (noErr for an orderly close) */
unsigned short tcp_send(short c, const void *data, unsigned short len);  /* bytes accepted (copied) */
unsigned short tcp_send_space(short c);
unsigned short tcp_unsent(short c);             /* bytes not yet acknowledged */
unsigned short tcp_recv(short c, void *buf, unsigned short max);         /* bytes copied out */
unsigned short tcp_unread(short c);
Boolean tcp_peer_closed(short c);               /* the server sent FIN (more data may still be unread) */
void tcp_close(short c);                        /* FIN once everything sent is acknowledged */
void tcp_abort(short c);                        /* RST, closed at once */
void tcp_release(short c);                      /* free the handle (aborts if still open) */
void tcp_poll(void);                            /* timers: retransmission, TIME_WAIT */
void tcp_input(unsigned char *ip, unsigned short hlen, unsigned short total);   /* from ip_input */
const char *tcp_state_name(short state);
unsigned short tcp_local_port(short c);
unsigned short tcp_peer_window(short c);         /* what the server lets us send */
unsigned short tcp_our_window(short c);          /* what we advertise */

#endif
