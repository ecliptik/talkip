/*
 * ipp_probe.c - M8: a plain MacTCP client (no talkip code) to check the .ipp driver: OpenDriver(".ipp"),
 * ipctlGetAddr, UDP (a DNS query for floodgap.com to the gateway's nameserver is not available through MacTCP,
 * so it asks the DNS server in DNS_SERVER directly), and TCP (gopher floodgap.com:70, the M4 test). It prints each step and
 * the application heap's free memory. Then the same fetch with asynchronous calls (PBControlAsync), polling
 * ioResult while calling SystemTask, a TCPStatus while a TCPRcv is pending (the driver must not be busy), and a
 * completion routine counting the receives.
 */
#include <string.h>
#include <stdio.h>
#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Events.h>
#include <Devices.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Gestalt.h>
#include "MacTCP.h"

#ifndef DNS_SERVER
#define DNS_SERVER ip(1, 1, 1, 1)         /* any recursive resolver the Mac can reach */
#endif

static WindowPtr win;
static short line_y = 14;
static short drvr;

static void say(const char *s)
{
    Str255 p;
    size_t n = strlen(s);

    if (n > 250)
        n = 250;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    SetPort(win);
    MoveTo(6, line_y);
    DrawString(p);
    line_y += 13;
}

static OSErr ctl(void *pb, short cs)
{
    ((TCPiopb *)pb)->ioCRefNum = drvr;
    ((TCPiopb *)pb)->csCode = cs;
    return PBControlSync((ParmBlkPtr)pb);
}

static unsigned long ip(int a, int b, int c, int d)
{
    return ((unsigned long)a << 24) | ((unsigned long)b << 16) | ((unsigned long)c << 8) | (unsigned long)d;
}

static void dns(void)
{
    static char buf[2048];
    static unsigned char q[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                                 8, 'f','l','o','o','d','g','a','p', 3, 'c','o','m', 0, 0, 1, 0, 1 };
    UDPiopb pb;
    wdsEntry wds[2];
    StreamPtr s;
    char line[128];
    OSErr err;

    memset(&pb, 0, sizeof(pb));
    pb.csParam.create.rcvBuff = buf;
    pb.csParam.create.rcvBuffLen = sizeof(buf);
    err = ctl(&pb, UDPCreate);
    s = pb.udpStream;
    sprintf(line, "UDPCreate: %d, local port %u", err, pb.csParam.create.localPort);
    say(line);
    if (err)
        return;
    memset(&pb, 0, sizeof(pb));
    pb.udpStream = s;
    pb.csParam.send.remoteHost = DNS_SERVER;
    pb.csParam.send.remotePort = 53;
    wds[0].length = sizeof(q);
    wds[0].ptr = (Ptr)q;
    wds[1].length = 0;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    err = ctl(&pb, UDPWrite);
    sprintf(line, "UDPWrite (DNS A floodgap.com): %d", err);
    say(line);
    memset(&pb, 0, sizeof(pb));
    pb.udpStream = s;
    pb.csParam.receive.timeOut = 5;
    err = ctl(&pb, UDPRead);
    if (err == noErr && pb.csParam.receive.rcvBuffLen >= 16) {
        unsigned char *r = (unsigned char *)pb.csParam.receive.rcvBuff;
        unsigned short n = pb.csParam.receive.rcvBuffLen;
        sprintf(line, "UDPRead: %u bytes, answers %u, last A %u.%u.%u.%u", n, r[7], r[n - 4], r[n - 3], r[n - 2],
                r[n - 1]);
        say(line);
        ctl(&pb, UDPBfrReturn);
    } else {
        sprintf(line, "UDPRead: %d", err);
        say(line);
    }
    memset(&pb, 0, sizeof(pb));
    pb.udpStream = s;
    err = ctl(&pb, UDPRelease);
    sprintf(line, "UDPRelease: %d", err);
    say(line);
}

static void gopher(unsigned long host)
{
    static char rbuf[4096];
    static char data[1024];
    static char sel[] = "\r\n";
    TCPiopb pb;
    wdsEntry wds[2];
    StreamPtr s;
    char line[128];
    long total = 0;
    unsigned long t0;
    OSErr err;

    memset(&pb, 0, sizeof(pb));
    pb.csParam.create.rcvBuff = rbuf;
    pb.csParam.create.rcvBuffLen = sizeof(rbuf);
    err = ctl(&pb, TCPCreate);
    s = pb.tcpStream;
    sprintf(line, "TCPCreate: %d", err);
    say(line);
    if (err)
        return;
    t0 = TickCount();
    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = s;
    pb.csParam.open.remoteHost = host;
    pb.csParam.open.remotePort = 70;
    err = ctl(&pb, TCPActiveOpen);
    sprintf(line, "TCPActiveOpen floodgap:70: %d, local port %u", err, pb.csParam.open.localPort);
    say(line);
    if (err == noErr) {
        memset(&pb, 0, sizeof(pb));
        pb.tcpStream = s;
        wds[0].length = 2;
        wds[0].ptr = sel;
        wds[1].length = 0;
        pb.csParam.send.wdsPtr = (Ptr)wds;
        err = ctl(&pb, TCPSend);
        sprintf(line, "TCPSend: %d", err);
        say(line);
        for (;;) {
            memset(&pb, 0, sizeof(pb));
            pb.tcpStream = s;
            pb.csParam.receive.rcvBuff = data;
            pb.csParam.receive.rcvBuffLen = sizeof(data);
            pb.csParam.receive.commandTimeoutValue = 10;
            err = ctl(&pb, TCPRcv);
            if (err != noErr)
                break;
            total += pb.csParam.receive.rcvBuffLen;
        }
        sprintf(line, "TCPRcv: %ld bytes in %lu ms, ended with %d", total, (TickCount() - t0) * 1000 / 60, err);
        say(line);
        memset(&pb, 0, sizeof(pb));
        pb.tcpStream = s;
        err = ctl(&pb, TCPClose);
        sprintf(line, "TCPClose: %d", err);
        say(line);
    }
    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = s;
    err = ctl(&pb, TCPRelease);
    sprintf(line, "TCPRelease: %d", err);
    say(line);
}

static volatile short completions;

static void rcv_done(TCPiopb *pb)         /* MacTCP calls it C-style: the pb on the stack */
{
    (void)pb;
    completions++;
}

/* Issue an async call and wait for it, calling SystemTask (accRun runs the driver) */
static OSErr actl(TCPiopb *pb, short cs, long *ticks)
{
    unsigned long t0 = TickCount();
    OSErr err;

    pb->ioCRefNum = drvr;
    pb->csCode = cs;
    err = PBControlAsync((ParmBlkPtr)pb);
    if (err != noErr)
        return err;
    while (pb->ioResult > 0)
        SystemTask();
    if (ticks)
        *ticks = (long)(TickCount() - t0);
    return pb->ioResult;
}

static void gopher_async(unsigned long host)
{
    static char rbuf[4096];
    static char data[1024];
    static char sel[] = "\r\n";
    TCPiopb pb, st;
    wdsEntry wds[2];
    StreamPtr s;
    char line[128];
    long total = 0, t;
    short rcvs = 0, during = 0, status_err = 0;
    unsigned long t0;
    OSErr err;

    memset(&pb, 0, sizeof(pb));
    pb.csParam.create.rcvBuff = rbuf;
    pb.csParam.create.rcvBuffLen = sizeof(rbuf);
    err = ctl(&pb, TCPCreate);
    s = pb.tcpStream;
    if (err)
        return;
    t0 = TickCount();
    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = s;
    pb.csParam.open.remoteHost = host;
    pb.csParam.open.remotePort = 70;
    err = actl(&pb, TCPActiveOpen, &t);
    sprintf(line, "async TCPActiveOpen: %d after %ld ticks", err, t);
    say(line);
    if (err == noErr) {
        memset(&pb, 0, sizeof(pb));
        pb.tcpStream = s;
        wds[0].length = 2;
        wds[0].ptr = sel;
        wds[1].length = 0;
        pb.csParam.send.wdsPtr = (Ptr)wds;
        err = actl(&pb, TCPSend, NULL);
        sprintf(line, "async TCPSend: %d", err);
        say(line);
        for (;;) {
            memset(&pb, 0, sizeof(pb));
            pb.tcpStream = s;
            pb.ioCompletion = (TCPIOCompletionUPP)rcv_done;
            pb.csParam.receive.rcvBuff = data;
            pb.csParam.receive.rcvBuffLen = sizeof(data);
            pb.csParam.receive.commandTimeoutValue = 10;
            pb.ioCRefNum = drvr;
            pb.csCode = TCPRcv;
            err = PBControlAsync((ParmBlkPtr)&pb);
            if (err != noErr)
                break;
            if (pb.ioResult > 0) {
                /* pending: the driver must still take another call */
                memset(&st, 0, sizeof(st));
                st.tcpStream = s;
                if (ctl(&st, TCPStatus) == noErr)
                    during++;
                else
                    status_err++;
            }
            while (pb.ioResult > 0)
                SystemTask();
            err = pb.ioResult;
            if (err != noErr)
                break;
            rcvs++;
            total += pb.csParam.receive.rcvBuffLen;
        }
        sprintf(line, "async TCPRcv: %ld bytes in %d receives, %lu ms, ended %d", total, rcvs,
                (TickCount() - t0) * 1000 / 60, err);
        say(line);
        sprintf(line, "  completion routine ran %d times; TCPStatus during a pending receive: %d ok, %d failed",
                completions, during, status_err);
        say(line);
        memset(&pb, 0, sizeof(pb));
        pb.tcpStream = s;
        err = actl(&pb, TCPClose, &t);
        sprintf(line, "async TCPClose: %d after %ld ticks", err, t);
        say(line);
    }
    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = s;
    err = ctl(&pb, TCPRelease);
    sprintf(line, "TCPRelease: %d", err);
    say(line);
}

int main(void)
{
    Rect r;
    GetAddrParamBlock g;
    char line[128];
    OSErr err;
    EventRecord ev;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitCursor();
    SetRect(&r, 10, 40, 502, 330);
    win = NewWindow(NULL, &r, "\pIPP Probe (MacTCP calls only)", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(win);
    TextFont(4);                        /* Monaco */
    TextSize(9);

    sprintf(line, "Free at start: %ld bytes", FreeMem());
    say(line);
    {
        long resp = 0;
        OSErr gerr = Gestalt('mtcp', &resp);
        sprintf(line, "Gestalt mtcp: %d, %08lx", gerr, resp);
        say(line);
    }
    err = OpenDriver("\p.ipp", &drvr);
    sprintf(line, "OpenDriver .ipp: %d, refNum %d", err, drvr);
    say(line);
    if (err == noErr) {
        memset(&g, 0, sizeof(g));
        err = ctl(&g, ipctlGetAddr);
        {
            TCPiopb d;
            memset(&d, 0, sizeof(d));
            sprintf(line, "driver: NewGestalt mtcp returned %d", ctl(&d, 9999));
            say(line);
        }
        sprintf(line, "ipctlGetAddr: %d, %lu.%lu.%lu.%lu mask %08lx", err, g.ourAddress >> 24,
                (g.ourAddress >> 16) & 255, (g.ourAddress >> 8) & 255, g.ourAddress & 255, g.ourNetMask);
        say(line);
        dns();
        gopher(ip(76, 79, 210, 35));
        gopher_async(ip(76, 79, 210, 35));
    }
    sprintf(line, "Free at end: %ld bytes. Click to quit.", FreeMem());
    say(line);
    for (;;)
        if (GetNextEvent(mDownMask | keyDownMask, &ev))
            break;
    return 0;
}
