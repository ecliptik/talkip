/*
 * ipp.c - M8 prototype: talkip as a MacTCP-compatible ".ipp" driver, so unmodified MacTCP applications can
 * use MacIP over LocalTalk on a 512Ke.
 *
 * An INIT ("TalkIP Driver" in the System Folder) runs once at boot: it relocates its code in place in the
 * system heap (globals go there too), the INIT detaches its own resource so it stays, and it puts a device
 * control entry for ".ipp" in a free unit-table slot. The driver header and its glue are in this code, so the
 * code is the driver. Applications then OpenDriver(".ipp") and make MacTCP's Control calls (TCPCreate ...
 * UDPRelease, ipctlGetAddr) as they would with MacTCP.
 *
 * Scope (prototype, 2026-10-16):
 *   - Calls that wait (ipctlGetAddr, TCPActiveOpen, TCPSend, TCPRcv, TCPClose, UDPRead) are steps that either
 *     finish or say "not yet". A synchronous call loops on its step, running the stack. An asynchronous one is
 *     taken off the Device Manager's queue (so other calls can come in, as with MacTCP), kept pending with
 *     ioResult = inProgress, and retried on every accRun (every 6 ticks while the application calls SystemTask or
 *     WaitNextEvent) and every call; when it finishes the driver sets ioResult and calls ioCompletion (A0 = pb).
 *   - The other calls finish at once. TCPPassiveOpen takes one connection, as MacTCP's does. No ASRs
 *     (notifyProc), no NoCopyRcv, no ICMP echo.
 *   - No DNR: MacTCP's resolver lives in the MacTCP control panel, not the driver. Geomys and Flynn resolve
 *     names themselves over UDP.
 */
#include <string.h>
#include <Devices.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Resources.h>
#include <Gestalt.h>
#include <stdint.h>
#include "Retro68Runtime.h"

#include "MacTCP.h"
#include "talkip.h"
#include "macip.h"
#include "tcp.h"

/* tcpshim.c: the MacTCP wrapper API, and the non-blocking steps the driver needs (TALKIP_RESIDENT) */
OSErr _TCPInit(void);
OSErr _TCPCreate(TCPiopb *, StreamPtr *, Ptr, long, TCPNotifyUPP, Ptr, TCPIOCompletionUPP, Boolean);
OSErr _TCPActiveOpen(TCPiopb *, StreamPtr, ip_addr, tcp_port, ip_addr *, tcp_port *, Ptr, TCPIOCompletionUPP,
                     Boolean);
OSErr _TCPAbort(TCPiopb *, StreamPtr, Ptr, TCPIOCompletionUPP, Boolean);
OSErr _TCPStatus(TCPiopb *, StreamPtr, struct TCPStatusPB *, Ptr, TCPIOCompletionUPP, Boolean);
OSErr _TCPRelease(TCPiopb *, StreamPtr, Ptr, TCPIOCompletionUPP, Boolean);
OSErr _UDPCreate(UDPiopb *, StreamPtr *, Ptr, long, UDPNotifyUPP, Ptr, UDPIOCompletionUPP, Boolean);
OSErr _UDPSend(UDPiopb *, StreamPtr, wdsEntry *, ip_addr, udp_port, Ptr, UDPIOCompletionUPP, Boolean);
OSErr _UDPBfrReturn(UDPiopb *, StreamPtr, Ptr, Ptr, UDPIOCompletionUPP, Boolean);
OSErr _UDPRelease(UDPiopb *, StreamPtr, Ptr, UDPIOCompletionUPP, Boolean);
OSErr shim_tcp_rcv_try(TCPiopb *);
OSErr shim_tcp_send_try(TCPiopb *, unsigned short *, unsigned short *);
OSErr shim_tcp_close_try(TCPiopb *, Boolean);
OSErr shim_udp_read_try(UDPiopb *);
OSErr shim_tcp_passive_start(TCPiopb *);
OSErr shim_tcp_passive_try(TCPiopb *);
void shim_forget_app(Ptr, Ptr);
extern char _stext[], _edata[], _sbss[], _ebss[];  /* the linker's section bounds */

#define IPP_FLAGS       0x6C00      /* dNeedLock | dNeedTime | dStatEnable | dCtlEnable */
#define IPP_DELAY       6           /* accRun every 6 ticks */
#define HDR_LEN         28          /* the parameter block header, up to and including csCode */
#define LEASE_WAIT_S    20
#define MACIP_MTU       586
#define CLOSE_WAIT_S    5
#define MAX_PENDING     8
#define ASYNC_TRAP      0x0400      /* ioTrap bit 10: asynchronous */
#define DRVR_ACTIVE     0x0080      /* dCtlFlags: the Device Manager is waiting for IODone */

/*
 * The driver: header (offsets from ipp_header), then glue. The Device Manager calls a routine with A0 = the
 * parameter block, A1 = the DCE. Open and Close return with RTS. Prime, Control and Status return with RTS
 * for an immediate call (noQueueBit, bit 9 of ioTrap, as SystemTask's accRun) and through JIODone otherwise,
 * with A0/A1 as they came in and the result in D0.
 */
__asm__(
    "   .text\n"
    "   .even\n"
    "   .globl  ipp_header\n"
    "ipp_header:\n"
    "   .word   0x6C00\n"                   /* IPP_FLAGS */
    "   .word   6\n"                        /* IPP_DELAY */
    "   .word   0\n"                        /* event mask */
    "   .word   0\n"                        /* menu */
    "   .word   ipp_g_open-ipp_header\n"
    "   .word   ipp_g_prime-ipp_header\n"
    "   .word   ipp_g_ctl-ipp_header\n"
    "   .word   ipp_g_stat-ipp_header\n"
    "   .word   ipp_g_close-ipp_header\n"
    "   .byte   4\n"
    "   .ascii  \".ipp\"\n"
    "   .even\n"
    "   .ascii  \"TkIP\"\n"                 /* TALKIP_DRIVER_SIG at offset 24: talkip apps look for it */
    "ipp_g_open:\n"
    "   movem.l %d1-%d7/%a0-%a6,-(%sp)\n"
    "   move.l  %a1,-(%sp)\n"
    "   move.l  %a0,-(%sp)\n"
    "   jsr     ipp_open\n"
    "   addq.l  #8,%sp\n"
    "   movem.l (%sp)+,%d1-%d7/%a0-%a6\n"
    "   rts\n"
    "ipp_g_close:\n"
    "   moveq   #-24,%d0\n"                 /* closErr: stay open, like MacTCP */
    "   rts\n"
    "ipp_g_prime:\n"
    "   moveq   #-17,%d0\n"                 /* controlErr: no reads or writes */
    "   bra.s   ipp_g_done\n"
    "ipp_g_stat:\n"
    "   moveq   #-18,%d0\n"                 /* statusErr */
    "   bra.s   ipp_g_done\n"
    "ipp_g_ctl:\n"
    "   movem.l %d1-%d7/%a0-%a6,-(%sp)\n"
    "   move.l  %a1,-(%sp)\n"
    "   move.l  %a0,-(%sp)\n"
    "   jsr     ipp_control\n"
    "   addq.l  #8,%sp\n"
    "   movem.l (%sp)+,%d1-%d7/%a0-%a6\n"
    "   cmp.l   #1,%d0\n"                   /* inProgress: kept pending, already off the queue */
    "   bne.s   ipp_g_done\n"
    "   moveq   #0,%d0\n"
    "   rts\n"
    "ipp_g_done:\n"
    "   move.w  6(%a0),%d1\n"               /* ioTrap */
    "   btst    #9,%d1\n"                   /* noQueueBit: immediate */
    "   bne.s   1f\n"
    "   move.l  0x8FC,-(%sp)\n"             /* JIODone */
    "1: rts\n"
    /*
     * void ipp_complete(ParmBlkPtr pb): call pb->ioCompletion, if any, the way MacTCP does: C-style, with the pb
     * on the stack (MacTCP.h's completion procs are stack-based). A0 = pb as well, as the Device Manager passes it.
     */
    "   .globl  ipp_complete\n"
    "ipp_complete:\n"
    "   movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "   move.l  48(%sp),%a0\n"
    "   move.l  12(%a0),%d0\n"
    "   beq.s   2f\n"
    "   move.l  %d0,%a1\n"
    "   move.l  %a0,-(%sp)\n"
    "   jsr     (%a1)\n"
    "   addq.l  #4,%sp\n"
    "2: movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "   rts\n"
    /*
     * ipp_tramp: put in a pb's ioCompletion for an asynchronous call that finishes at once, so the Device
     * Manager's IODone (which calls completion routines with A0 = pb, nothing on the stack) calls this, and this
     * calls the application's routine MacTCP's way.
     */
    "   .globl  ipp_tramp\n"
    "ipp_tramp:\n"
    "   movem.l %d0-%d2/%a0-%a1,-(%sp)\n"
    "   move.l  %a0,-(%sp)\n"
    "   jsr     ipp_tramp_c\n"
    "   addq.l  #4,%sp\n"
    "   movem.l (%sp)+,%d0-%d2/%a0-%a1\n"
    "   rts\n"
);
void ipp_complete(ParmBlkPtr pb);
void ipp_tramp(void);
extern char ipp_header[];

static Boolean opened;
static OSErr gestalt_err = 1;          /* NewGestalt's result at boot, for the M8 probe (csCode 9999) */

long ipp_open(ParmBlkPtr pb, DCtlPtr dce)
{
    (void)pb;
    (void)dce;
    if (!opened) {
        opened = true;
        _TCPInit();                     /* opens socket 72, looks for the gateway; keeps trying if it's not there */
    }
    return noErr;
}

/* A call that may have to wait: its pb, csCode, deadline (0 = none) and, for TCPSend, how far it has got */
typedef struct Pending {
    ParmBlkPtr pb;                      /* NULL: free */
    short op;
    unsigned long deadline;
    unsigned short wi, off;
} Pending;

static Pending pending[MAX_PENDING];
static Boolean polling;

/* Completion routines swapped for ipp_tramp while IODone finishes a call: the pb and the application's routine */
static struct { ParmBlkPtr pb; ProcPtr done; } swapped[MAX_PENDING];

void ipp_tramp_c(ParmBlkPtr pb)
{
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        if (swapped[i].pb == pb) {
            pb->ioParam.ioCompletion = (void *)swapped[i].done;
            swapped[i].pb = NULL;
            ipp_complete(pb);
            return;
        }
    }
}

static Boolean waits(short cs)
{
    return cs == ipctlGetAddr || cs == TCPActiveOpen || cs == TCPPassiveOpen || cs == TCPSend || cs == TCPRcv
           || cs == TCPClose || cs == UDPRead;
}

static unsigned long deadline_of(ParmBlkPtr pb, short cs)
{
    unsigned short s = 0;

    if (cs == ipctlGetAddr)
        s = LEASE_WAIT_S;
    else if (cs == TCPRcv)
        s = (unsigned char)((TCPiopb *)pb)->csParam.receive.commandTimeoutValue;   /* 0: wait for ever */
    else if (cs == TCPPassiveOpen)
        s = (unsigned char)((TCPiopb *)pb)->csParam.open.commandTimeoutValue;      /* 0: wait for ever */
    else if (cs == TCPClose)
        s = CLOSE_WAIT_S;
    else if (cs == UDPRead)
        s = ((UDPiopb *)pb)->csParam.receive.timeOut;                             /* 0: wait for ever */
    return s ? TickCount() + (unsigned long)s * 60 : 0;
}

static OSErr timed_out(short cs)
{
    if (cs == ipctlGetAddr)
        return ipBadLapErr;
    if (cs == TCPClose)
        return noErr;                   /* our FIN is out; don't hold the caller for the acknowledgement */
    return commandTimeout;
}

/* One try at a call that may wait: inProgress (1) if it can't finish yet, else the result */
static OSErr step(Pending *p, Boolean first)
{
    ParmBlkPtr pb = p->pb;
    TCPiopb *t = (TCPiopb *)pb;
    GetAddrParamBlock *g = (GetAddrParamBlock *)pb;
    OSErr err;

    switch (p->op) {
    case ipctlGetAddr:
        if (talkip_local_ip() == 0)
            return inProgress;
        g->ourAddress = talkip_local_ip();
        g->ourNetMask = macip_cfg.netmask ? (long)macip_cfg.netmask : (long)0xFFFFFF00L;
        return noErr;
    case TCPActiveOpen:
        if (first) {                    /* asynchronous in the shim: talkip_idle advances it, ioResult says when */
            err = _TCPActiveOpen(t, t->tcpStream, t->csParam.open.remoteHost, t->csParam.open.remotePort, NULL,
                                 NULL, NULL, NULL, true);
            if (err != noErr)
                return err;
        }
        return t->ioResult;
    case TCPPassiveOpen:
        return first ? shim_tcp_passive_start(t) : shim_tcp_passive_try(t);
    case TCPSend:
        return shim_tcp_send_try(t, &p->wi, &p->off);
    case TCPRcv:
        return shim_tcp_rcv_try(t);
    case TCPClose:
        return shim_tcp_close_try(t, first);
    case UDPRead:
        return shim_udp_read_try((UDPiopb *)pb);
    }
    return controlErr;
}

/* Retry the pending calls; finish those that are done, calling their completion routines */
static void poll_pending(void)
{
    int i;

    if (polling)                        /* a completion routine made a call: the outer loop carries on */
        return;
    polling = true;
    for (i = 0; i < MAX_PENDING; i++) {
        Pending *p = &pending[i];
        ParmBlkPtr pb = p->pb;
        OSErr err;

        if (pb == NULL)
            continue;
        err = step(p, false);
        if (err == inProgress && p->deadline && TickCount() > p->deadline) {
            err = timed_out(p->op);
            if (p->op == TCPPassiveOpen)    /* stop listening */
                _TCPAbort((TCPiopb *)pb, ((TCPiopb *)pb)->tcpStream, NULL, NULL, false);
        }
        if (err != inProgress) {
            p->pb = NULL;
            pb->ioParam.ioResult = err;
            ipp_complete(pb);
        }
    }
    polling = false;
}

/* End the pending calls on a stream that is being aborted or released */
static void end_pending(StreamPtr s, Boolean udp)
{
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        Pending *p = &pending[i];
        ParmBlkPtr pb = p->pb;

        if (pb && (udp ? (p->op == UDPRead && ((UDPiopb *)pb)->udpStream == s)
                       : (p->op != UDPRead && p->op != ipctlGetAddr && ((TCPiopb *)pb)->tcpStream == s))) {
            p->pb = NULL;
            pb->ioParam.ioResult = udp ? invalidStreamPtr : connectionTerminated;
            ipp_complete(pb);
        }
    }
}

/*
 * InitApplZone (a launch, or the application quitting) calls the routine in IAZNotify: the application's memory,
 * from ApplZone up to BufPtr, is about to be reused. Calls it left pending, and its streams, are dropped without
 * completion, since their parameter blocks and completion routines are in that memory. Without this, a call still
 * pending at a launch is completed later into the next application's heap: on System 6 without MultiFinder,
 * Retro68's LaunchAPPL server sublaunches with a TCPRcv outstanding, and when the connection ended the driver jumped
 * through a stale ioCompletion ("coprocessor not installed", found in Snow 2026-10-19).
 */
#define LM_APPL_ZONE    (*(Ptr *)0x2AA)
#define LM_BUF_PTR      (*(Ptr *)0x10C)
#define LM_IAZ_NOTIFY   (*(ProcPtr *)0x33C)

ProcPtr ipp_old_iaz;
void ipp_iaz_patch(void);
__asm__(
    "   .text\n"
    "   .even\n"
    "   .globl  ipp_iaz_patch\n"
    "ipp_iaz_patch:\n"
    "   movem.l %d0-%d2/%a0-%a1,-(%sp)\n"
    "   jsr     ipp_forget_app\n"
    "   movem.l (%sp)+,%d0-%d2/%a0-%a1\n"
    "   tst.l   ipp_old_iaz\n"
    "   beq.s   1f\n"
    "   move.l  ipp_old_iaz,-(%sp)\n"      /* chain to whoever was there before */
    "1: rts\n"
);

void ipp_forget_app(void)
{
    Ptr lo = LM_APPL_ZONE, hi = LM_BUF_PTR;
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        if ((Ptr)pending[i].pb >= lo && (Ptr)pending[i].pb < hi)
            pending[i].pb = NULL;
        if ((Ptr)swapped[i].pb >= lo && (Ptr)swapped[i].pb < hi)
            swapped[i].pb = NULL;
    }
    shim_forget_app(lo, hi);
}

/* Only this call is on the driver's queue: it can be taken off and finished by the driver itself */
static Boolean alone(ParmBlkPtr pb, DCtlPtr dce)
{
    return dce->dCtlQHdr.qHead == (QElemPtr)pb && pb->ioParam.qLink == NULL;
}

static OSErr dispatch(ParmBlkPtr pb, DCtlPtr dce)
{
    TCPiopb *t = (TCPiopb *)pb;
    UDPiopb *u = (UDPiopb *)pb;
    short cs = pb->cntrlParam.csCode;
    char hdr[HDR_LEN];
    StreamPtr s;
    OSErr err;

    if (cs == accRun) {
        talkip_idle();
        poll_pending();
        return noErr;
    }

    if (waits(cs)) {
        Pending w;
        int i;

        w.pb = pb;
        w.op = cs;
        w.deadline = deadline_of(pb, cs);
        w.wi = w.off = 0;
        talkip_idle();
        err = step(&w, true);
        if (err != inProgress)
            return err;
        if ((pb->ioParam.ioTrap & ASYNC_TRAP) && alone(pb, dce)) {
            for (i = 0; i < MAX_PENDING && pending[i].pb; i++)
                ;
            if (i < MAX_PENDING) {
                pending[i] = w;
                pb->ioParam.ioResult = inProgress;
                /* Off the Device Manager's queue: the driver isn't busy, more calls can come in */
                Dequeue((QElemPtr)pb, &dce->dCtlQHdr);
                dce->dCtlFlags &= ~DRVR_ACTIVE;
                return inProgress;
            }
        }
        if (pb->ioParam.ioTrap & ASYNC_TRAP) {
            /* no room to keep it, or calls queued behind it: finish it synchronously */
        }
        for (;;) {
            talkip_idle();
            poll_pending();
            err = step(&w, false);
            if (err != inProgress)
                return err;
            if (w.deadline && TickCount() > w.deadline) {
                if (cs == TCPPassiveOpen)
                    _TCPAbort(t, t->tcpStream, NULL, NULL, false);
                return timed_out(cs);
            }
        }
    }

    poll_pending();
    switch (cs) {
    /* TCP: the shim fills the pb's results */
    case TCPCreate:
        memcpy(hdr, pb, HDR_LEN);       /* _TCPCreate clears the whole pb */
        err = _TCPCreate(t, &s, t->csParam.create.rcvBuff, (long)t->csParam.create.rcvBuffLen,
                         t->csParam.create.notifyProc, t->csParam.create.userDataPtr, NULL, false);
        memcpy(pb, hdr, HDR_LEN);
        if (err == noErr)
            t->tcpStream = s;
        return err;
    case TCPAbort:
        end_pending(t->tcpStream, false);
        return _TCPAbort(t, t->tcpStream, NULL, NULL, false);
    case TCPStatus:
        return _TCPStatus(t, t->tcpStream, NULL, NULL, NULL, false);
    case TCPRelease:
        end_pending(t->tcpStream, false);
        return _TCPRelease(t, t->tcpStream, NULL, NULL, false);

    /* UDP */
    case UDPCreate:
        err = _UDPCreate(u, &s, u->csParam.create.rcvBuff, (long)u->csParam.create.rcvBuffLen,
                         u->csParam.create.notifyProc, u->csParam.create.userDataPtr, NULL, false);
        if (err == noErr)
            u->udpStream = s;
        return err;
    case UDPWrite:
        return _UDPSend(u, u->udpStream, (wdsEntry *)u->csParam.send.wdsPtr, u->csParam.send.remoteHost,
                        u->csParam.send.remotePort, NULL, NULL, false);
    case UDPBfrReturn:
        return _UDPBfrReturn(u, u->udpStream, u->csParam.receive.rcvBuff, NULL, NULL, false);
    case UDPRelease:
        end_pending(u->udpStream, true);
        return _UDPRelease(u, u->udpStream, NULL, NULL, false);
    case 9999:                          /* M8 diagnostics */
        return gestalt_err;
    case UDPMaxMTUSize:
        u->csParam.mtu.mtuSize = MACIP_MTU - 28;
        return noErr;

    default:
        return controlErr;
    }
}

long ipp_control(ParmBlkPtr pb, DCtlPtr dce)
{
    OSErr err = dispatch(pb, dce);
    int i;

    if (err == inProgress || !(pb->ioParam.ioTrap & ASYNC_TRAP))
        return err;
    /*
     * An asynchronous call that finished at once. MacTCP returns noErr from PBControlAsync and reports the
     * result only in ioResult (Shiori Classic stops if the call itself fails), and calls the completion routine
     * C-style: so take the call off the queue, finish it here and return noErr.
     */
    if (alone(pb, dce)) {
        Dequeue((QElemPtr)pb, &dce->dCtlQHdr);
        dce->dCtlFlags &= ~DRVR_ACTIVE;
        pb->ioParam.ioResult = err;
        ipp_complete(pb);
        return inProgress;              /* the glue returns noErr without IODone */
    }
    /* Other calls are queued behind (made from a completion routine): IODone must run to start them */
    if (pb->ioParam.ioCompletion) {
        for (i = 0; i < MAX_PENDING && swapped[i].pb; i++)
            ;
        if (i < MAX_PENDING) {
            swapped[i].pb = pb;
            swapped[i].done = (ProcPtr)pb->ioParam.ioCompletion;
            pb->ioParam.ioCompletion = (void *)ipp_tramp;
        }
    }
    return err;
}

/* ---- the INIT: install the driver at boot ---- */

#define UTABLE_BASE     (*(DCtlHandle **)0x11C)
#define UNIT_COUNT      (*(short *)0x1D2)
#define FIRST_UNIT      12          /* 0-11 are Apple's */

static Boolean driver_named(DCtlHandle h, const unsigned char *name)
{
    Ptr drvr;

    if (h == NULL || *h == NULL)
        return false;
    drvr = (Ptr)(*h)->dCtlDriver;
    if ((*h)->dCtlFlags & 0x0040)       /* dRAMBased: dCtlDriver is a handle */
        drvr = drvr ? *(Handle)drvr : NULL;
    return drvr != NULL && memcmp(drvr + 18, name, name[0] + 1) == 0;
}

/*
 * Gestalt 'mtcp', as MacTCP registers it: some applications (Retro68's LaunchAPPL server) check it, not the driver.
 * System 7 has NewGestalt; System 6 answers Gestalt but has no NewGestalt (it returns gestaltUndefSelectorErr),
 * so there the INIT patches the Gestalt trap (0xA1AD, D0 = selector, A0 = response, D0 = result) for 'mtcp' only.
 * The trap word is in D1: NewGestalt (A3AD) and ReplaceGestalt (A5AD) share the trap and go straight through.
 */
#define MTCP_VERSION    0x02068000L         /* MacTCP 2.0.6 */

UniversalProcPtr ipp_old_gestalt;
void ipp_gestalt_patch(void);
__asm__(
    "   .text\n"
    "   .even\n"
    "   .globl  ipp_gestalt_patch\n"
    "ipp_gestalt_patch:\n"
    "   move.w  %d1,%d2\n"
    "   and.w   #0x0600,%d2\n"
    "   bne.s   1f\n"                      /* not plain Gestalt */
    "   cmp.l   #0x6D746370,%d0\n"         /* 'mtcp' */
    "   bne.s   1f\n"
    "   move.l  #0x02068000,%a0\n"         /* MTCP_VERSION */
    "   moveq   #0,%d0\n"
    "   rts\n"
    "1: move.l  ipp_old_gestalt,-(%sp)\n"
    "   rts\n"
);

static pascal OSErr mtcp_gestalt(OSType selector, long *response)
{
    (void)selector;
    *response = MTCP_VERSION;
    return noErr;
}

static void ipp_install(void)
{
    static const unsigned char name[] = "\p.ipp";
    DCtlHandle *table = UTABLE_BASE;
    DCtlHandle dce;
    Handle self;
    short unit;

    for (unit = 0; unit < UNIT_COUNT; unit++)
        if (driver_named(table[unit], name))
            return;                     /* MacTCP, or ourselves: leave it */
    for (unit = UNIT_COUNT - 1; unit >= FIRST_UNIT; unit--)
        if (table[unit] == NULL)
            break;
    if (unit < FIRST_UNIT)
        return;                         /* no free slot */
    dce = (DCtlHandle)NewHandleSysClear(sizeof(DCtlEntry));
    if (dce == NULL)
        return;
    HLock((Handle)dce);
    (*dce)->dCtlDriver = (void *)ipp_header;
    (*dce)->dCtlFlags = IPP_FLAGS;      /* not dRAMBased: dCtlDriver is a pointer */
    (*dce)->dCtlRefNum = ~unit;
    (*dce)->dCtlDelay = IPP_DELAY;
    table[unit] = dce;
    gestalt_err = NewGestalt('mtcp', (SelectorFunctionUPP)mtcp_gestalt);  /* fails harmlessly if MacTCP has it */
    if (gestalt_err == gestaltUndefSelectorErr) {   /* System 6: no NewGestalt */
        ipp_old_gestalt = GetOSTrapAddress(0xA1AD);
        SetOSTrapAddress((UniversalProcPtr)ipp_gestalt_patch, 0xA1AD);
        gestalt_err = 2;                /* patched */
    }
    ipp_old_iaz = LM_IAZ_NOTIFY;
    LM_IAZ_NOTIFY = (ProcPtr)ipp_iaz_patch;

    /* Keep this code (the driver) in the system heap after the INIT's file closes */
    self = Get1Resource('INIT', 128);
    if (self) {
        HLock(self);
        DetachResource(self);
        /* The relocation table after the data is spent: give it back (about 0.7 KB) */
        SetHandleSize(self, (Size)(_edata - *self));
    }
}

/*
 * Relocate in place, as Retro68Relocate does for flat code, without its general cases (multisegment apps, ROM
 * and StripAddress checks, constructors): this is a 68000 in 24-bit mode, and Retro68's relocator plus the
 * multisegment init it pulls in would stay resident as 1.7 KB of dead code. Runs unrelocated: no globals, and calls go through
 * the displacement. The globals (bss) go to the system heap. Returns false if there is no room for them.
 */
static Boolean ipp_relocate(void)
{
    long d;
    char *stext, *edata, *sbss, *ebss, *bss;
    uint32_t disp[4];

    RETRO68_GET_DISPLACEMENT_STRIP24(d);
    __asm__("lea _stext,%0" : "=a"(stext));     /* link addresses: the code isn't relocated yet */
    __asm__("lea _edata,%0" : "=a"(edata));
    __asm__("lea _sbss,%0" : "=a"(sbss));
    __asm__("lea _ebss,%0" : "=a"(ebss));
    bss = NewPtrSysClear(ebss - sbss);
    if (bss == NULL)
        return false;
    disp[0] = (uint32_t)d;                      /* code */
    disp[1] = (uint32_t)d;                      /* data: inside the code resource */
    disp[2] = (uint32_t)(bss - sbss);           /* bss */
    disp[3] = 0;                                /* jump table: none */

    /*
     * The relocation table follows the data: uleb128 values, each (offset from the previous longword << 2) | kind,
     * a pass of absolute then a pass of PC-relative longwords, each ended by 0. Inlined from Retro68's
     * Retro68ApplyRelocations (libretro/relocate.c, GPLv3 with the GCC Runtime Library Exception): calling it
     * would link Retro68's whole relocator and multisegment runtime, resident for good.
     */
    {
        uint8_t *base = (uint8_t *)stext + d;
        uint8_t *rel = base + (edata - stext);
        int relative;

        for (relative = 0; relative <= 1; relative++) {
            uint8_t *p = base - 1;
            while (*rel) {
                uint32_t val = 0, a;
                uint8_t b;
                int i = 0;
                do {
                    b = *rel++;
                    val |= (uint32_t)(b & 0x7F) << i;
                    i += 7;
                } while (b & 0x80);
                p += val >> 2;
                a = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
                a += disp[val & 3];
                if (relative)
                    a -= (uint32_t)p;
                p[0] = (uint8_t)(a >> 24);
                p[1] = (uint8_t)(a >> 16);
                p[2] = (uint8_t)(a >> 8);
                p[3] = (uint8_t)a;
            }
            rel++;
        }
    }
    return true;
}

void _start(void)
{
    long d;

    RETRO68_GET_DISPLACEMENT_STRIP24(d);
    if (!(*(typeof(&ipp_relocate))((char *)&ipp_relocate + d))())
        return;
    ipp_install();
}
