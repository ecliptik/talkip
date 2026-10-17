/*
 * ddp.c - .MPP driver calls (open/close socket, writeDDP, NBP lookup) and the socket listener.
 *
 * Retro68's Multiversal interfaces have no AppleTalk parameter blocks, so MPPPB below declares the IM II /
 * IM: Networking layout and the calls go straight to PBControl on .MPP (refNum -10), which is all the "P"
 * glue does. Only calls that exist in the 128K ROM's AppleTalk are used (no Phase 2 additions).
 */
#include <stddef.h>
#include <string.h>
#include <Devices.h>
#include <OSUtils.h>

#include "ddp.h"

enum {
    csWriteDDP = 246,
    csCloseSkt = 247,
    csOpenSkt = 248,
    csLookupName = 251
};
#define ddpSktErr (-91)

typedef struct MPPPB {
    void *qLink;
    short qType;
    short ioTrap;
    Ptr ioCmdAddr;
    void *ioCompletion;
    OSErr ioResult;
    long userData;
    short reqTID;
    short ioRefNum;
    short csCode;
    union {
        struct {
            unsigned char socket;
            unsigned char checksumFlag;
            void *listener_or_wds;      /* openSkt: listener; writeDDP: write-data structure */
        } ddp;
        struct {
            unsigned char interval;     /* retry interval, 8-tick units */
            unsigned char count;        /* tries */
            Ptr entityPtr;
            Ptr retBuffPtr;
            short retBuffSize;
            short maxToGet;
            short numGotten;
        } nbp;
    } u;
} MPPPB;

_Static_assert(offsetof(MPPPB, csCode) == 26, "MPPPB csCode");
_Static_assert(offsetof(MPPPB, u.ddp.listener_or_wds) == 30, "MPPPB listener");
_Static_assert(offsetof(MPPPB, u.nbp.numGotten) == 42, "MPPPB numGotten");
_Static_assert(offsetof(DDPRxSlot, data) == 12 && sizeof(DDPRxSlot) == 598, "DDPRxSlot layout (listener)");

/* Shared with the listener (absolute addresses: Retro68 doesn't address globals through A5) */
DDPRxSlot ddp_rx[DDP_RX_SLOTS];
DDPRxSlot *const ddp_rx_ptr[DDP_RX_SLOTS] = {
    &ddp_rx[0], &ddp_rx[1], &ddp_rx[2], &ddp_rx[3], &ddp_rx[4], &ddp_rx[5], &ddp_rx[6], &ddp_rx[7]
};
volatile unsigned char ddp_rx_head;
volatile unsigned long ddp_rx_dropped;
volatile unsigned long ddp_rx_calls;       /* listener invocations */
volatile unsigned long ddp_rx_errors;      /* ReadRest failed: the frame was lost on the wire */

DDPStats ddp_stats;
unsigned char ddp_my_node;
unsigned short ddp_my_net;

static unsigned char rx_tail;
unsigned char ddp_socket = TALKIP_SOCKET;
static short mpp_ref;
static Boolean socket_open;

void talkip_listener(void);

/*
 * The DDP socket listener. .MPP calls it at interrupt time with
 *   A2 = .MPP's variables; the read-header area (RHA) starts at 1(A2): LAP dst 1, LAP src 2, LAP type 3
 *        (1 = short DDP header: dstSkt 6, srcSkt 7, type 8; 2 = long: dstNet 8, srcNet 10, srcNode 13,
 *        srcSkt 15, type 16), and this network's number at 0x1A(A2) (as TalkTool reads it)
 *   A4 = ReadPacket, ReadRest = 2(A4); D1.w = bytes left (the DDP payload)
 * It must call ReadRest (within ~95 us on LocalTalk, so do nothing else first), preserve A0/A1 until then
 * and A4-A6/D4-D7 always,
 * and may use A0-A3/D0-D3 after it. Z set after ReadRest = success.
 * It fills ddp_rx[ddp_rx_head] if that slot is free and the payload fits, else discards (D3 = 0).
 * Slot offsets: full 0, type 1, len 2, src_net 4, dst_net 6, src_node 8, src_socket 9, dst_node 10, data 12.
 */
__asm__(
    "   .text\n"
    "   .even\n"
    "   .globl  talkip_listener\n"
    "talkip_listener:\n"
    /* Before ReadRest: only find the slot and check it (about 150 cycles). The SCC's 3-byte FIFO overflows
     * about 100 us into the rest of the frame (230.4 kbit/s), so header copying waits until after ReadRest,
     * as Apple's TalkTool listener does: the read-header area and A2 are still valid then. Copying first
     * made ReadRest fail on 3 of 4 frames on a real 512Ke (2026-10-15); Snow doesn't model the timing. */
    "   moveq   #0,%d2\n"
    "   move.b  ddp_rx_head,%d2\n"
    "   lsl.w   #2,%d2\n"
    "   lea     ddp_rx_ptr,%a3\n"
    "   move.l  0(%a3,%d2.w),%a3\n"     /* the slot */
    "   tst.b   (%a3)\n"
    "   bne.w   9f\n"                   /* ring full */
    "   cmp.w   #586,%d1\n"
    "   bhi.w   9f\n"                   /* can't be a valid DDP payload */
    "   move.w  %d1,2(%a3)\n"           /* len */
    "   lea     12(%a3),%a3\n"
    "   moveq   #0,%d3\n"
    "   move.w  %d1,%d3\n"
    "   jsr     2(%a4)\n"               /* ReadRest the whole payload into the slot */
    "   bne.w   7f\n"                   /* error: leave the slot empty, count it */
    "   moveq   #0,%d2\n"
    "   move.b  ddp_rx_head,%d2\n"
    "   move.w  %d2,%d0\n"
    "   lsl.w   #2,%d2\n"
    "   lea     ddp_rx_ptr,%a3\n"
    "   move.l  0(%a3,%d2.w),%a3\n"
    "   cmp.b   #2,3(%a2)\n"
    "   beq.s   1f\n"
    "   move.b  8(%a2),1(%a3)\n"        /* short header: type */
    "   move.w  0x1A(%a2),4(%a3)\n"     /*   src net = this net */
    "   move.w  0x1A(%a2),6(%a3)\n"     /*   dst net = this net */
    "   move.b  2(%a2),8(%a3)\n"        /*   src node = LAP source */
    "   move.b  7(%a2),9(%a3)\n"        /*   src socket */
    "   bra.s   2f\n"
    "1: move.b  16(%a2),1(%a3)\n"       /* long header: type */
    "   move.w  10(%a2),4(%a3)\n"       /*   src net */
    "   move.w  8(%a2),6(%a3)\n"        /*   dst net */
    "   move.b  13(%a2),8(%a3)\n"       /*   src node */
    "   move.b  15(%a2),9(%a3)\n"       /*   src socket */
    "2: move.b  1(%a2),10(%a3)\n"       /* dst node = LAP destination */
    "   st      (%a3)\n"                /* publish: full = 0xFF */
    "   addq.b  #1,%d0\n"
    "   and.b   #7,%d0\n"               /* DDP_RX_SLOTS - 1 */
    "   move.b  %d0,ddp_rx_head\n"
    "   addq.l  #1,ddp_rx_calls\n"
    "   rts\n"
    "7: addq.l  #1,ddp_rx_errors\n"
    "   addq.l  #1,ddp_rx_calls\n"
    "   rts\n"
    "9: moveq   #0,%d3\n"
    "   jsr     2(%a4)\n"               /* ReadRest with size 0 discards */
    "   addq.l  #1,ddp_rx_dropped\n"
    "   addq.l  #1,ddp_rx_calls\n"
    "   rts\n"
);
_Static_assert(DDP_RX_SLOTS == 8, "the listener masks with 7 and ddp_rx_ptr lists 8 slots");

static void pb_init(MPPPB *pb, short cs)
{
    memset(pb, 0, sizeof(*pb));
    pb->ioRefNum = mpp_ref;
    pb->csCode = cs;
}

static OSErr open_socket(void)
{
    MPPPB pb;

    pb_init(&pb, csOpenSkt);
    pb.u.ddp.socket = ddp_socket;
    pb.u.ddp.listener_or_wds = (void *)talkip_listener;
    return PBControlSync((ParmBlkPtr)&pb);
}

static void close_socket(void)
{
    MPPPB pb;

    pb_init(&pb, csCloseSkt);
    pb.u.ddp.socket = ddp_socket;
    PBControlSync((ParmBlkPtr)&pb);
}

/*
 * Crash safety. Socket 72's listener lives in this application's code: if the app dies with the socket still
 * open, the next datagram for socket 72 (macipgw pings an idle client within 30 s) jumps into a dead heap and
 * takes the Mac down. Every way out of an application goes through ExitToShell (a normal quit, MacsBug's "es",
 * the bomb box's Resume when the app's resume procedure calls ExitToShell, as talkip apps' do), so while the
 * socket is open ExitToShell is patched to close it first. Under
 * MultiFinder an application's patches are switched with it, so this stays per-application.
 */
#ifndef TALKIP_RESIDENT                 /* the .ipp driver is resident: nothing to clean up */
#define TRAP_EXIT_TO_SHELL 0xA9F4

static ProcPtr old_exit_to_shell;

static void exit_patch(void)
{
    ProcPtr old = old_exit_to_shell;

    ddp_close();                        /* closes socket 72 and puts ExitToShell back */
    ((void (*)(void))old)();            /* never returns */
}

static void install_exit_patch(void)
{
    if (old_exit_to_shell == NULL) {    /* once: a second ddp_open must not patch over our own patch */
        old_exit_to_shell = GetToolTrapAddress(TRAP_EXIT_TO_SHELL);
        SetToolTrapAddress((ProcPtr)exit_patch, TRAP_EXIT_TO_SHELL);
    }
}

static void remove_exit_patch(void)
{
    if (old_exit_to_shell != NULL) {
        SetToolTrapAddress(old_exit_to_shell, TRAP_EXIT_TO_SHELL);
        old_exit_to_shell = NULL;
    }
}
#else
#define install_exit_patch() ((void)0)
#define remove_exit_patch() ((void)0)
#endif

/* Is the TalkIP Driver installed? A unit-table driver named .ipp whose header carries TALKIP_DRIVER_SIG. */
Boolean talkip_driver_present(void)
{
    DCtlHandle *table = *(DCtlHandle **)0x11C;     /* UTableBase */
    short count = *(short *)0x1D2;                 /* UnitNtryCnt */
    short u;

    for (u = 0; u < count; u++) {
        DCtlHandle h = table[u];
        unsigned char *drvr;

        if (h == NULL || *h == NULL || ((*h)->dCtlFlags & 0x0040))  /* none, or RAM-based (not ours) */
            continue;
        drvr = (unsigned char *)(*h)->dCtlDriver;
        if (drvr && memcmp(drvr + 18, "\p.ipp", 5) == 0 && memcmp(drvr + 24, TALKIP_DRIVER_SIG, 4) == 0)
            return true;
    }
    return false;
}

OSErr ddp_open(void)
{
    OSErr err;

    /* AppleShare has AppleTalk open already: this just returns .MPP's refNum. Never close .MPP. */
    err = OpenDriver("\p.MPP", &mpp_ref);
    if (err != noErr)
        return err;
    memset(ddp_rx, 0, sizeof(ddp_rx));
    ddp_rx_head = 0;
    rx_tail = 0;
    ddp_socket = TALKIP_SOCKET;
    err = open_socket();
    if (err == ddpSktErr && talkip_driver_present()) {
        /* The TalkIP Driver (M8) holds socket 72 for MacTCP applications: use 73, with a lease of our own */
        ddp_socket = TALKIP_SOCKET_ALT;
        err = open_socket();
    } else if (err == ddpSktErr) {
#ifdef TALKIP_RESIDENT
        /* The driver opened while a talkip app holds 72: leave the app its socket */
        ddp_socket = TALKIP_SOCKET_ALT;
        err = open_socket();
#else
        /* Left open by a talkip app that crashed: its listener points into a dead heap. Take it over. */
        close_socket();
        err = open_socket();
#endif
    }
    if (err == noErr) {
        socket_open = true;
        install_exit_patch();
    }
    return err;
}

void ddp_close(void)
{
    if (socket_open) {
        close_socket();
        socket_open = false;
    }
    remove_exit_patch();
}

OSErr ddp_write(const ATAddr *dst, unsigned char type, const void *data, unsigned short len)
{
    /* The header block starts at an odd address (h + 1): 3 LAP bytes + a 13-byte long DDP header.
     * .MPP fills in everything except these fields. */
    static short hbuf[9];
    unsigned char *h = (unsigned char *)hbuf;
    struct { short len; const void *ptr; } wds[3];
    MPPPB pb;
    OSErr err;

    if (len > DDP_MAX_DATA)
        return paramErr;
    memset(hbuf, 0, sizeof(hbuf));
    *(unsigned short *)(h + 8) = dst->net;
    h[12] = dst->node;
    h[14] = dst->socket;
    h[16] = type;
    wds[0].len = 16;
    wds[0].ptr = h + 1;
    wds[1].len = len;
    wds[1].ptr = data;
    wds[2].len = 0;

    pb_init(&pb, csWriteDDP);
    pb.u.ddp.socket = ddp_socket;
    pb.u.ddp.checksumFlag = 0;          /* IP has its own checksums */
    pb.u.ddp.listener_or_wds = wds;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr)
        ddp_stats.tx++;
    else
        ddp_stats.tx_err++;
    return err;
}

DDPRxSlot *ddp_next(void)
{
    DDPRxSlot *s = &ddp_rx[rx_tail];

    if (!s->full)
        return NULL;
    ddp_stats.rx++;
    if (s->dst_node != 0xFF)
        ddp_my_node = s->dst_node;
    if (s->dst_net != 0)
        ddp_my_net = s->dst_net;
    return s;
}

void ddp_release(DDPRxSlot *s)
{
    s->full = 0;
    rx_tail = (rx_tail + 1) & (DDP_RX_SLOTS - 1);
}

short ddp_driver_version(void)
{
    SysEnvRec env;

    if (SysEnvirons(1, &env) != noErr)
        return -1;
    return env.atDrvrVersNum;
}

static unsigned char *pack(unsigned char *p, const char *s)
{
    size_t n = strlen(s);

    if (n > 32)
        n = 32;
    *p++ = (unsigned char)n;
    memcpy(p, s, n);
    return p + n;
}

OSErr nbp_lookup(const char *object, const char *type, const char *zone, ATAddr *addr, char *object_out)
{
    unsigned char entity[100];
    unsigned char reply[120];           /* one tuple: address(4) + enumerator(1) + three names */
    unsigned char *p;
    MPPPB pb;
    OSErr err;

    p = pack(entity, object);
    p = pack(p, type);
    pack(p, zone);

    pb_init(&pb, csLookupName);
    pb.u.nbp.interval = 3;              /* 24 ticks between tries */
    pb.u.nbp.count = 5;
    pb.u.nbp.entityPtr = (Ptr)entity;
    pb.u.nbp.retBuffPtr = (Ptr)reply;
    pb.u.nbp.retBuffSize = sizeof(reply);
    pb.u.nbp.maxToGet = 1;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr)
        return err;
    if (pb.u.nbp.numGotten < 1)
        return TALKIP_ERR_NOTFOUND;

    addr->net = (unsigned short)((reply[0] << 8) | reply[1]);
    addr->node = reply[2];
    addr->socket = reply[3];
    if (object_out) {
        unsigned char n = reply[5];     /* reply[4] is the enumerator */
        if (n > 32)
            n = 32;
        memcpy(object_out, reply + 6, n);
        object_out[n] = '\0';
    }
    return noErr;
}
