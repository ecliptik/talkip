/*
 * tcpshim.c - the MacTCP wrapper API that Flynn, Geomys and Shiori Classic call (NCSA-derived tcp.c:
 * _TCPCreate, _TCPActiveOpen, ... _UDPRelease, ip2long), implemented on talkip instead of the .IPP driver.
 *
 * Semantics follow MacTCP where the apps can see them:
 *   - synchronous calls block until done, pumping the stack (and SystemTask) while they wait;
 *   - asynchronous calls complete at once: ioResult is final when the call returns, and an ioCompletion
 *     routine, if given, is called before returning;
 *   - results use MacTCP's error codes (openFailed, connectionClosing, commandTimeout, ...).
 * TCPSend copies the data, so a caller may reuse its buffers as soon as the call returns.
 * Limits: TCP_MAX_CONNS streams; a TCP receive buffer over 65535 bytes uses the first 65535; one queued
 * datagram per UDP stream (MacTCP queues several in the stream's buffer).
 */
#include <string.h>
#include <Events.h>
#include <OSUtils.h>

#include "MacTCP.h"
#include "talkip.h"
#include "ddp.h"
#include "macip.h"
#include "ip.h"
#include "udp.h"
#include "tcp.h"
#include "fmt.h"

#define OPEN_TIMEOUT_S  30
#define CLOSE_WAIT_S    5
#define LEASE_WAIT_S    20
#define RCV_TIMEOUT_S   30
#define UDP_STREAMS     2

typedef struct ShimStream {
    Boolean used;
    short conn;                         /* talkip TCP handle, -1 before ActiveOpen */
    Ptr rcv_buf;
    unsigned long rcv_len;
    unsigned long remote_ip;
    unsigned short remote_port;
    /* an ActiveOpen in progress (open_pb != NULL): advanced by talkip_idle */
    TCPiopb *open_pb;
    ip_addr *open_lip;
    tcp_port *open_lport;
    TCPIOCompletionUPP open_done;
    unsigned long lease_deadline, open_deadline;
} ShimStream;

typedef struct ShimUDP {
    Boolean used, full;
    unsigned short port;
    Ptr buf;
    unsigned long buf_len;
    unsigned short len;
    unsigned long from_ip;
    unsigned short from_port;
} ShimUDP;

static ShimStream streams[TCP_MAX_CONNS];
static ShimUDP udps[UDP_STREAMS];
static Boolean started;

static void open_step(ShimStream *ss);

/* A stream whose connection has ended (closed, aborted, or in TIME_WAIT) can open again, as with MacTCP */
static void reuse(ShimStream *ss)
{
    short st;

    if (ss->conn >= 0 && !ss->open_pb && ((st = tcp_state(ss->conn)) == TCP_CLOSED || st == TCP_TIME_WAIT)) {
        tcp_release(ss->conn);
        ss->conn = -1;
    }
}

void talkip_idle(void)
{
    int i;

    if (!started)
        return;
    macip_poll();
    tcp_poll();
    for (i = 0; i < TCP_MAX_CONNS; i++)
        if (streams[i].used && streams[i].open_pb)
            open_step(&streams[i]);
}

static void pump(void)
{
    talkip_idle();
#ifndef TALKIP_RESIDENT                 /* in the .ipp driver SystemTask could re-enter the driver */
    SystemTask();
#endif
}

unsigned long talkip_local_ip(void)
{
    return macip_state == MACIP_UP ? macip_cfg.ip : 0;
}

unsigned long talkip_nameserver(void)
{
    return macip_state == MACIP_UP ? macip_cfg.nameserver : 0;
}

const char *talkip_status(void)
{
    static char b[48];

    b[0] = '\0';
    if (macip_state == MACIP_UP) {
        fmt_str(b, "MacIP up ");
        fmt_ip(b, macip_cfg.ip);
    } else if (macip_recovering()) {
        fmt_str(b, "MacIP: looking for the gateway");
    } else {
        fmt_str(b, "MacIP: down");
    }
    return b;
}

/* Wait (pumping) up to s seconds for the lease. */
static Boolean have_lease(unsigned short s)
{
    unsigned long until = TickCount() + (unsigned long)s * 60;

    while (macip_state != MACIP_UP && TickCount() < until)
        pump();
    return macip_state == MACIP_UP;
}

/* Finish a call the MacTCP way: result in ioResult, completion routine for async calls. */
static OSErr done_tcp(TCPiopb *pb, OSErr err, TCPIOCompletionUPP completion, Boolean async)
{
    pb->ioResult = err;
    if (async && completion)
        ((void (*)(TCPiopb *))completion)(pb);
    return err;
}

static OSErr done_udp(UDPiopb *pb, OSErr err, UDPIOCompletionUPP completion, Boolean async)
{
    pb->ioResult = err;
    if (async && completion)
        ((void (*)(UDPiopb *))completion)(pb);
    return err;
}

static ShimStream *stream_of(StreamPtr s)
{
    ShimStream *ss = (ShimStream *)s;

    if (ss < streams || ss >= streams + TCP_MAX_CONNS || !ss->used)
        return NULL;
    return ss;
}

static ShimUDP *udp_of(StreamPtr s)
{
    ShimUDP *u = (ShimUDP *)s;

    if (u < udps || u >= udps + UDP_STREAMS || !u->used)
        return NULL;
    return u;
}

OSErr _TCPInit(void)
{
    OSErr err;

    if (started)
        return noErr;
    err = ddp_open();
    if (err != noErr)
        return err;
    started = true;
    if (macip_find_gateway() != noErr || macip_assign() != noErr)
        macip_retry_later();            /* not fatal: the stack keeps trying while the app runs */
    return noErr;
}

OSErr _TCPCreate(TCPiopb *pb, StreamPtr *stream, Ptr rcvBufPtr, long rcvBufLen, TCPNotifyUPP aNotifyProc,
                 Ptr userDataPtr, TCPIOCompletionUPP ioCompletion, Boolean async)
{
    int i;

    (void)aNotifyProc;                  /* no ASRs: callers poll TCPStatus */
    (void)userDataPtr;
    memset(pb, 0, sizeof(*pb));
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (!streams[i].used) {
            memset(&streams[i], 0, sizeof(streams[i]));
            streams[i].used = true;
            streams[i].conn = -1;
            streams[i].rcv_buf = rcvBufPtr;
            streams[i].rcv_len = rcvBufLen > 65535L ? 65535L : (unsigned long)rcvBufLen;
            *stream = (StreamPtr)&streams[i];
            pb->tcpStream = *stream;
            pb->csParam.create.rcvBuff = rcvBufPtr;
            pb->csParam.create.rcvBuffLen = rcvBufLen;
            return done_tcp(pb, noErr, ioCompletion, async);
        }
    }
    return done_tcp(pb, insufficientResources, ioCompletion, async);
}

/* Finish a pending ActiveOpen: result in its ioResult, completion routine if one was given. */
static void open_finish(ShimStream *ss, OSErr err)
{
    TCPiopb *pb = ss->open_pb;
    TCPIOCompletionUPP done = ss->open_done;

    ss->open_pb = NULL;
    if (err != noErr && ss->conn >= 0) {
        tcp_abort(ss->conn);
        tcp_release(ss->conn);
        ss->conn = -1;
    }
    if (err == noErr) {
        if (ss->open_lip)
            *ss->open_lip = macip_cfg.ip;
        if (ss->open_lport)
            *ss->open_lport = tcp_local_port(ss->conn);
        pb->csParam.open.remoteHost = ss->remote_ip;
        pb->csParam.open.remotePort = ss->remote_port;
        pb->csParam.open.localHost = macip_cfg.ip;
        pb->csParam.open.localPort = tcp_local_port(ss->conn);
    }
    pb->ioResult = err;
    if (done)
        ((void (*)(TCPiopb *))done)(pb);
}

/* Advance a pending ActiveOpen: wait for the lease, send the SYN, wait for the SYN-ACK. */
static void open_step(ShimStream *ss)
{
    if (ss->conn < 0) {
        if (macip_state != MACIP_UP) {
            if (TickCount() > ss->lease_deadline)
                open_finish(ss, openFailed);
            return;
        }
        ss->conn = tcp_open(ss->remote_ip, ss->remote_port, (unsigned char *)ss->rcv_buf,
                            (unsigned short)ss->rcv_len);
        if (ss->conn < 0) {
            ss->conn = -1;
            open_finish(ss, insufficientResources);
            return;
        }
        ss->open_deadline = TickCount() + OPEN_TIMEOUT_S * 60L;
        return;
    }
    switch (tcp_state(ss->conn)) {
    case TCP_SYN_SENT:
        if (TickCount() > ss->open_deadline)
            open_finish(ss, commandTimeout);
        break;
    case TCP_ESTABLISHED:
        open_finish(ss, noErr);
        break;
    default:
        open_finish(ss, openFailed);    /* refused, reset or timed out */
        break;
    }
}

/*
 * ActiveOpen. Synchronous: blocks (pumping) until connected or failed. Asynchronous: returns at once with
 * ioResult = inProgress (1); talkip_idle() advances it, then sets ioResult and calls ioCompletion. Abort or
 * Release ends a pending open with connectionTerminated.
 */
OSErr _TCPActiveOpen(TCPiopb *pb, StreamPtr stream, ip_addr remoteIP, tcp_port remotePort, ip_addr *localIP,
                     tcp_port *localPort, Ptr userData, TCPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimStream *ss = stream_of(stream);

    (void)userData;
    pump();
    if (!ss)
        return done_tcp(pb, invalidStreamPtr, ioCompletion, async);
    reuse(ss);
    if (ss->conn >= 0 || ss->open_pb)
        return done_tcp(pb, duplicateSocket, ioCompletion, async);
    ss->remote_ip = remoteIP;
    ss->remote_port = remotePort;
    ss->open_pb = pb;
    ss->open_lip = localIP;
    ss->open_lport = localPort;
    ss->open_done = async ? ioCompletion : NULL;
    ss->lease_deadline = TickCount() + LEASE_WAIT_S * 60L;
    pb->ioResult = inProgress;
    open_step(ss);
    if (async)
        return noErr;
    while (ss->open_pb)
        pump();
    return pb->ioResult;
}

OSErr _TCPSend(TCPiopb *pb, StreamPtr stream, wdsEntry *wdsPtr, Ptr userData, TCPIOCompletionUPP ioCompletion,
               Boolean async)
{
    ShimStream *ss = stream_of(stream);
    wdsEntry *w;

    (void)userData;
    pump();
    if (!ss || ss->conn < 0)
        return done_tcp(pb, connectionDoesntExist, ioCompletion, async);
    for (w = wdsPtr; w && w->length; w++) {
        const unsigned char *p = (const unsigned char *)w->ptr;
        unsigned short left = w->length;

        while (left) {
            unsigned short n;
            short st = tcp_state(ss->conn);

            if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT)
                return done_tcp(pb, st == TCP_CLOSED ? connectionTerminated : connectionClosing, ioCompletion, async);
            n = tcp_send(ss->conn, p, left);
            p += n;
            left -= n;
            if (left)
                pump();                 /* send buffer full: let acknowledgements in */
        }
    }
    return done_tcp(pb, noErr, ioCompletion, async);
}

OSErr _TCPRcv(TCPiopb *pb, StreamPtr stream, Ptr rcvBufPtr, unsigned short *rcvBufLen, Ptr userData,
              TCPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimStream *ss = stream_of(stream);
    unsigned long until;
    unsigned short n;

    (void)userData;
    pump();
    if (!ss || ss->conn < 0)
        return done_tcp(pb, connectionDoesntExist, ioCompletion, async);
    until = TickCount() + RCV_TIMEOUT_S * 60L;    /* the pb's own timeout field isn't set by callers here */
    while (tcp_unread(ss->conn) == 0 && !tcp_peer_closed(ss->conn) && tcp_state(ss->conn) != TCP_CLOSED
           && TickCount() < until)
        pump();
    n = tcp_recv(ss->conn, rcvBufPtr, *rcvBufLen);
    *rcvBufLen = n;
    pb->csParam.receive.rcvBuff = rcvBufPtr;
    pb->csParam.receive.rcvBuffLen = n;
    if (n)
        return done_tcp(pb, noErr, ioCompletion, async);
    if (tcp_state(ss->conn) == TCP_CLOSED)
        return done_tcp(pb, connectionTerminated, ioCompletion, async);
    if (tcp_peer_closed(ss->conn))
        return done_tcp(pb, connectionClosing, ioCompletion, async);
    return done_tcp(pb, commandTimeout, ioCompletion, async);
}

OSErr _TCPClose(TCPiopb *pb, StreamPtr stream, Ptr userData, TCPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimStream *ss = stream_of(stream);
    unsigned long until;
    short st;

    (void)userData;
    if (!ss || ss->conn < 0)
        return done_tcp(pb, connectionDoesntExist, ioCompletion, async);
    tcp_close(ss->conn);
    /* MacTCP's close completes once our FIN is out and acknowledged; don't wait out TIME_WAIT */
    until = TickCount() + CLOSE_WAIT_S * 60L;
    while (((st = tcp_state(ss->conn)) == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT || st == TCP_FIN_WAIT_1
            || st == TCP_LAST_ACK || st == TCP_CLOSING) && TickCount() < until)
        pump();
    return done_tcp(pb, noErr, ioCompletion, async);
}

OSErr _TCPAbort(TCPiopb *pb, StreamPtr stream, Ptr userData, TCPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimStream *ss = stream_of(stream);

    (void)userData;
    if (ss && ss->open_pb)
        open_finish(ss, connectionTerminated);
    if (!ss || ss->conn < 0)
        return done_tcp(pb, connectionDoesntExist, ioCompletion, async);
    tcp_abort(ss->conn);
    return done_tcp(pb, noErr, ioCompletion, async);
}

static SInt8 mactcp_state(short st)
{
    switch (st) {
    case TCP_LISTEN: return ConnectionStateListening;
    case TCP_SYN_RCVD: return ConnectionStateSYNReceived;
    case TCP_SYN_SENT: return ConnectionStateSYNSent;
    case TCP_ESTABLISHED: return ConnectionStateEstablished;
    case TCP_FIN_WAIT_1: return ConnectionStateFINWait1;
    case TCP_FIN_WAIT_2: return ConnectionStateFINWait2;
    case TCP_CLOSE_WAIT: return ConnectionStateCloseWait;
    case TCP_CLOSING: return ConnectionStateClosing;
    case TCP_LAST_ACK: return ConnectionStateLastAck;
    case TCP_TIME_WAIT: return ConnectionStateTimeWait;
    default: return ConnectionStateClosed;
    }
}

OSErr _TCPStatus(TCPiopb *pb, StreamPtr stream, struct TCPStatusPB *status, Ptr userData,
                 TCPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimStream *ss = stream_of(stream);
    struct TCPStatusPB *s = &pb->csParam.status;
    short st;

    (void)userData;
    pump();
    if (!ss || ss->conn < 0)
        return done_tcp(pb, connectionDoesntExist, ioCompletion, async);
    st = tcp_state(ss->conn);
    memset(s, 0, sizeof(*s));
    s->remoteHost = ss->remote_ip;
    s->remotePort = ss->remote_port;
    s->localHost = macip_cfg.ip;
    s->localPort = tcp_local_port(ss->conn);
    s->connectionState = mactcp_state(st);
    s->sendWindow = tcp_peer_window(ss->conn);
    s->rcvWindow = tcp_our_window(ss->conn);
    s->amtUnackedData = tcp_unsent(ss->conn);
    s->amtUnreadData = tcp_unread(ss->conn);
    s->sendMaxSegSize = TCP_MSS;
    if (status)
        *status = *s;
    if (st == TCP_CLOSED && tcp_unread(ss->conn) == 0)
        return done_tcp(pb, connectionDoesntExist, ioCompletion, async);
    return done_tcp(pb, noErr, ioCompletion, async);
}

OSErr _TCPRelease(TCPiopb *pb, StreamPtr stream, Ptr userData, TCPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimStream *ss = stream_of(stream);

    (void)userData;
    if (!ss)
        return done_tcp(pb, invalidStreamPtr, ioCompletion, async);
    if (ss->open_pb)
        open_finish(ss, connectionTerminated);
    if (ss->conn >= 0)
        tcp_release(ss->conn);
    pb->csParam.create.rcvBuff = ss->rcv_buf;
    pb->csParam.create.rcvBuffLen = ss->rcv_len;
    ss->used = false;
    return done_tcp(pb, noErr, ioCompletion, async);
}

/* UDP: one datagram held per stream, in the stream's own buffer, until UDPBfrReturn. */
static void udp_arrival(unsigned long src, unsigned short sport, unsigned short dport, const unsigned char *data,
                        unsigned short len)
{
    int i;

    for (i = 0; i < UDP_STREAMS; i++) {
        ShimUDP *u = &udps[i];
        if (u->used && u->port == dport && !u->full) {
            if (len > u->buf_len)
                len = (unsigned short)u->buf_len;
            memcpy(u->buf, data, len);
            u->len = len;
            u->from_ip = src;
            u->from_port = sport;
            u->full = true;
            return;
        }
    }
}

OSErr _UDPCreate(UDPiopb *pb, StreamPtr *stream, Ptr rcvBufPtr, long rcvBufLen, UDPNotifyUPP aNotifyProc,
                 Ptr userDataPtr, UDPIOCompletionUPP ioCompletion, Boolean async)
{
    unsigned short port;
    int i;

    (void)aNotifyProc;
    (void)userDataPtr;
    for (i = 0; i < UDP_STREAMS; i++) {
        if (!udps[i].used) {
            ShimUDP *u = &udps[i];
            port = udp_ephemeral_port();    /* callers here never ask for a fixed port */
            if (udp_bind(port, udp_arrival) != noErr)
                return done_udp(pb, insufficientResources, ioCompletion, async);
            memset(u, 0, sizeof(*u));
            u->used = true;
            u->port = port;
            u->buf = rcvBufPtr;
            u->buf_len = rcvBufLen > 65535L ? 65535L : (unsigned long)rcvBufLen;
            *stream = (StreamPtr)u;
            pb->udpStream = *stream;
            pb->csParam.create.localPort = port;
            return done_udp(pb, noErr, ioCompletion, async);
        }
    }
    return done_udp(pb, insufficientResources, ioCompletion, async);
}

OSErr _UDPSend(UDPiopb *pb, StreamPtr stream, wdsEntry *wdsPtr, ip_addr remoteIP, udp_port remotePort,
               Ptr userData, UDPIOCompletionUPP ioCompletion, Boolean async)
{
    unsigned char *buf;
    ShimUDP *u = udp_of(stream);
    unsigned short len = 0;
    wdsEntry *w;

    (void)userData;
    pump();
    if (!u)
        return done_udp(pb, invalidStreamPtr, ioCompletion, async);
    if (!have_lease(LEASE_WAIT_S))
        return done_udp(pb, connectionDoesntExist, ioCompletion, async);
    buf = udp_payload();                /* gathered straight into the IP output buffer: no staging copy */
    for (w = wdsPtr; w && w->length; w++) {
        if (len + w->length > UDP_MAX_PAYLOAD)
            return done_udp(pb, paramErr, ioCompletion, async);
        memcpy(buf + len, w->ptr, w->length);
        len += w->length;
    }
    return done_udp(pb, udp_send(remoteIP, u->port, remotePort, buf, len), ioCompletion, async);
}

OSErr _UDPRcv(UDPiopb *pb, StreamPtr stream, unsigned short timeout, Ptr userData, UDPIOCompletionUPP ioCompletion,
              Boolean async)
{
    ShimUDP *u = udp_of(stream);
    unsigned long until = TickCount() + (unsigned long)(timeout ? timeout : 60) * 60;

    (void)userData;
    if (!u)
        return done_udp(pb, invalidStreamPtr, ioCompletion, async);
    while (!u->full && TickCount() < until)
        pump();
    if (!u->full)
        return done_udp(pb, commandTimeout, ioCompletion, async);
    pb->csParam.receive.rcvBuff = u->buf;
    pb->csParam.receive.rcvBuffLen = u->len;
    pb->csParam.receive.remoteHost = u->from_ip;
    pb->csParam.receive.remotePort = u->from_port;
    return done_udp(pb, noErr, ioCompletion, async);
}

OSErr _UDPBfrReturn(UDPiopb *pb, StreamPtr stream, Ptr rcvBuff, Ptr userData, UDPIOCompletionUPP ioCompletion,
                    Boolean async)
{
    ShimUDP *u = udp_of(stream);

    (void)rcvBuff;
    (void)userData;
    if (!u)
        return done_udp(pb, invalidStreamPtr, ioCompletion, async);
    u->full = false;
    return done_udp(pb, noErr, ioCompletion, async);
}

OSErr _UDPRelease(UDPiopb *pb, StreamPtr stream, Ptr userData, UDPIOCompletionUPP ioCompletion, Boolean async)
{
    ShimUDP *u = udp_of(stream);

    (void)userData;
    if (!u)
        return done_udp(pb, invalidStreamPtr, ioCompletion, async);
    udp_unbind(u->port);
    pb->csParam.create.rcvBuff = u->buf;
    pb->csParam.create.rcvBuffLen = u->buf_len;
    u->used = false;
    return done_udp(pb, noErr, ioCompletion, async);
}

#ifdef TALKIP_RESIDENT
/*
 * Non-blocking steps for the .ipp driver (M8), which keeps MacTCP's asynchronous calls pending and retries them
 * from accRun: each returns inProgress (1) until it can finish, then the MacTCP result, with the pb filled in.
 * Timeouts are the driver's.
 */
OSErr shim_tcp_rcv_try(TCPiopb *pb)
{
    ShimStream *ss = stream_of(pb->tcpStream);
    unsigned short n;

    if (!ss || ss->conn < 0)
        return connectionDoesntExist;
    if (tcp_unread(ss->conn) == 0 && !tcp_peer_closed(ss->conn) && tcp_state(ss->conn) != TCP_CLOSED)
        return inProgress;
    n = tcp_recv(ss->conn, pb->csParam.receive.rcvBuff, pb->csParam.receive.rcvBuffLen);
    pb->csParam.receive.rcvBuffLen = n;
    if (n)
        return noErr;
    return tcp_state(ss->conn) == TCP_CLOSED ? connectionTerminated : connectionClosing;
}

/* Copy as much of the WDS as fits into the send buffer; wi and off say how far the copying has got. */
OSErr shim_tcp_send_try(TCPiopb *pb, unsigned short *wi, unsigned short *off)
{
    ShimStream *ss = stream_of(pb->tcpStream);
    wdsEntry *w = (wdsEntry *)pb->csParam.send.wdsPtr;

    if (!ss || ss->conn < 0)
        return connectionDoesntExist;
    for (w += *wi; w->length; w++, (*wi)++, *off = 0) {
        while (*off < (unsigned short)w->length) {
            short st = tcp_state(ss->conn);
            unsigned short n;

            if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT)
                return st == TCP_CLOSED ? connectionTerminated : connectionClosing;
            n = tcp_send(ss->conn, (const unsigned char *)w->ptr + *off, (unsigned short)w->length - *off);
            if (n == 0)
                return inProgress;      /* send buffer full: wait for acknowledgements */
            *off += n;
        }
    }
    return noErr;
}

/* first: send our FIN. Then: done once it is acknowledged (MacTCP doesn't wait out TIME_WAIT). */
OSErr shim_tcp_close_try(TCPiopb *pb, Boolean first)
{
    ShimStream *ss = stream_of(pb->tcpStream);
    short st;

    if (!ss || ss->conn < 0)
        return connectionDoesntExist;
    if (first)
        tcp_close(ss->conn);
    st = tcp_state(ss->conn);
    if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT || st == TCP_FIN_WAIT_1 || st == TCP_LAST_ACK
        || st == TCP_CLOSING)
        return inProgress;
    return noErr;
}

/* TCPPassiveOpen: listen on csParam.open.localPort for one connection */
OSErr shim_tcp_passive_start(TCPiopb *pb)
{
    ShimStream *ss = stream_of(pb->tcpStream);

    if (!ss)
        return invalidStreamPtr;
    reuse(ss);
    if (ss->conn >= 0 || ss->open_pb)
        return duplicateSocket;
    ss->conn = tcp_listen(pb->csParam.open.localPort, (unsigned char *)ss->rcv_buf, (unsigned short)ss->rcv_len);
    if (ss->conn < 0) {
        ss->conn = -1;
        return insufficientResources;
    }
    return inProgress;
}

OSErr shim_tcp_passive_try(TCPiopb *pb)
{
    ShimStream *ss = stream_of(pb->tcpStream);
    short st;

    if (!ss || ss->conn < 0)
        return connectionDoesntExist;
    st = tcp_state(ss->conn);
    if (st == TCP_LISTEN || st == TCP_SYN_RCVD)
        return inProgress;
    if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT)
        return openFailed;
    tcp_remote(ss->conn, &ss->remote_ip, &ss->remote_port);
    pb->csParam.open.remoteHost = ss->remote_ip;
    pb->csParam.open.remotePort = ss->remote_port;
    pb->csParam.open.localHost = macip_cfg.ip;
    pb->csParam.open.localPort = tcp_local_port(ss->conn);
    return noErr;
}

OSErr shim_udp_read_try(UDPiopb *pb)
{
    ShimUDP *u = udp_of(pb->udpStream);

    if (!u)
        return invalidStreamPtr;
    if (!u->full)
        return inProgress;
    pb->csParam.receive.rcvBuff = u->buf;
    pb->csParam.receive.rcvBuffLen = u->len;
    pb->csParam.receive.remoteHost = u->from_ip;
    pb->csParam.receive.remotePort = u->from_port;
    return noErr;
}

/*
 * An application's memory, [lo, hi), is about to be reused (it quit, or launched another one). Its streams go:
 * those whose receive buffer or pending ActiveOpen is in it. TCP connections are reset, UDP ports unbound. Nothing
 * is completed, since the completion routines are in that memory too.
 */
void shim_forget_app(Ptr lo, Ptr hi)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        ShimStream *ss = &streams[i];
        if (ss->used && ((ss->rcv_buf >= lo && ss->rcv_buf < hi)
                         || ((Ptr)ss->open_pb >= lo && (Ptr)ss->open_pb < hi))) {
            ss->open_pb = NULL;
            if (ss->conn >= 0)
                tcp_release(ss->conn);  /* aborts if still open */
            ss->used = false;
        }
    }
    for (i = 0; i < UDP_STREAMS; i++) {
        ShimUDP *u = &udps[i];
        if (u->used && u->buf >= lo && u->buf < hi) {
            udp_unbind(u->port);
            u->used = false;
        }
    }
}
#endif

unsigned long ip2long(char *ip)
{
    return ip_parse(ip);
}
