/*
 * TalkIP Ping - milestone M2: ICMP echo from the 512Ke to any address, through the MacIP gateway.
 * Four pings a second apart, 3 s timeout each, then min/avg/max. Times come from TickCount, so they are
 * multiples of 1/60 s (about 17 ms). A host name is looked up first (DNS, M3).
 */
#include <string.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Events.h>
#include <ToolUtils.h>

#include "appkit.h"
#include "ip.h"
#include "macip.h"
#include "fmt.h"
#include "dns.h"

#define PING_ID         0x5450      /* 'TP' */
#define PING_COUNT      4
#define PING_INTERVAL   60          /* ticks */
#define PING_TIMEOUT    (3 * 60)
#define PING_PAYLOAD    56          /* like Unix ping: 84-byte datagrams */
#define LOG_TOP         50
#define LOG_ROWS        16

static WindowPtr win;
static TEHandle host_te;
static ControlHandle ping_btn, stop_btn;
static Rect host_rect;
static Boolean quitting;

static Boolean pinging, resolving;
static char host[64];
static unsigned long target;
static unsigned short seq_next;
static short sent, received;
static unsigned long next_send, rtt_min, rtt_max, rtt_sum;
static struct { unsigned long sent_at; Boolean waiting; } slot[16];

static unsigned long ticks_to_ms(unsigned long t)
{
    return (t * 1000UL + 30) / 60;
}

static void finish(void)
{
    char b[96] = "";

    pinging = false;
    HiliteControl(ping_btn, 0);
    HiliteControl(stop_btn, 255);
    fmt_ul(b, sent);
    fmt_str(b, " sent, ");
    fmt_ul(b, received);
    fmt_str(b, " received, ");
    fmt_ul(b, sent ? (unsigned long)(sent - received) * 100 / sent : 0);
    fmt_str(b, "% loss");
    if (received) {
        fmt_str(b, "; min/avg/max ");
        fmt_ul(b, ticks_to_ms(rtt_min));
        fmt_str(b, "/");
        fmt_ul(b, ticks_to_ms(rtt_sum) / received);
        fmt_str(b, "/");
        fmt_ul(b, ticks_to_ms(rtt_max));
        fmt_str(b, " ms");
    }
    ak_log(b, NULL);
}

static void echo_reply(unsigned long src, unsigned short id, unsigned short seq,
                       const unsigned char *data, unsigned short len)
{
    char b[80] = "";
    unsigned long rtt;

    (void)data;
    if (!pinging || id != PING_ID || src != target || !slot[seq & 15].waiting)
        return;                     /* not ours, a duplicate, or too late */
    slot[seq & 15].waiting = false;
    rtt = TickCount() - slot[seq & 15].sent_at;
    received++;
    rtt_sum += rtt;
    if (received == 1 || rtt < rtt_min)
        rtt_min = rtt;
    if (rtt > rtt_max)
        rtt_max = rtt;
    fmt_ul(b, len + 8);
    fmt_str(b, " bytes from ");
    fmt_ip(b, src);
    fmt_str(b, ": seq=");
    fmt_ul(b, seq);
    fmt_str(b, rtt ? " time=" : " time<");
    fmt_ul(b, rtt ? ticks_to_ms(rtt) : 17);
    fmt_str(b, " ms");
    ak_log(b, NULL);
}

static void begin_ping(void)
{
    char b[80] = "";

    memset(slot, 0, sizeof(slot));
    sent = received = 0;
    rtt_min = rtt_max = rtt_sum = 0;
    seq_next = 1;
    next_send = TickCount();
    pinging = true;
    HiliteControl(ping_btn, 255);
    HiliteControl(stop_btn, 0);
    fmt_str(b, "PING ");
    fmt_ip(b, target);
    fmt_str(b, ": 56 data bytes");
    ak_log(b, NULL);
}

static void start_ping(void)
{
    CharsHandle h = TEGetText(host_te);
    short n = (**host_te).teLength;

    if (pinging || resolving)
        return;
    if (n > (short)sizeof(host) - 1)
        n = sizeof(host) - 1;
    memcpy(host, *h, n);
    host[n] = '\0';
    if (n == 0)
        return;
    if (macip_state != MACIP_UP) {
        ak_log("No address from the gateway yet", NULL);
        return;
    }
    target = ip_parse(host);
    if (target != 0) {
        begin_ping();
    } else if (dns_begin(host) == noErr) {
        resolving = true;
        HiliteControl(ping_btn, 255);
    }
}

static void resolve_poll(void)
{
    DNSResult r;
    char b[96] = "";

    if (!resolving || dns_poll(&r) == DNS_PENDING)
        return;
    resolving = false;
    HiliteControl(ping_btn, 0);
    fmt_str(b, host);
    if (r.err != noErr || r.naddrs == 0) {
        fmt_str(b, ": ");
        fmt_str(b, dns_error_text(r.err));
        ak_log(b, NULL);
        return;
    }
    target = r.addr[0];
    fmt_str(b, " is ");
    fmt_ip(b, target);
    ak_log(b, NULL);
    begin_ping();
}

static void ping_poll(void)
{
    unsigned long now = TickCount();
    Boolean waiting = false;
    int s;

    if (!pinging)
        return;
    if (sent < PING_COUNT && now >= next_send) {
        unsigned char payload[PING_PAYLOAD];
        unsigned short seq = seq_next++;
        OSErr err;
        int i;

        for (i = 0; i < PING_PAYLOAD; i++)
            payload[i] = (unsigned char)(0x10 + i);
        slot[seq & 15].sent_at = now;
        slot[seq & 15].waiting = true;
        err = ip_send_echo(target, PING_ID, seq, payload, PING_PAYLOAD);
        if (err != noErr) {
            char b[16] = "";
            slot[seq & 15].waiting = false;
            ak_log("Send failed: error ", fmt_err(b, err));
        }
        sent++;
        next_send += PING_INTERVAL;
    }
    for (s = 1; s <= sent; s++) {
        if (slot[s & 15].waiting) {
            if (now - slot[s & 15].sent_at > PING_TIMEOUT) {
                char b[40] = "seq=";
                slot[s & 15].waiting = false;
                fmt_ul(b, s);
                fmt_str(b, ": timed out");
                ak_log(b, NULL);
            } else {
                waiting = true;
            }
        }
    }
    if (sent >= PING_COUNT && !waiting)
        finish();
}

static void draw(void)
{
    char b[80];

    SetPort(win);
    EraseRect(&win->portRect);
    TextFont(0);
    TextSize(12);
    ak_draw_text(8, 21, "Host:");
    FrameRect(&host_rect);
    TEUpdate(&win->portRect, host_te);
    DrawControls(win);
    TextFont(4);
    TextSize(9);
    ak_net_status(b);
    ak_draw_text(8, 44, b);
    MoveTo(8, LOG_TOP - 2);
    LineTo(win->portRect.right - 8, LOG_TOP - 2);
    ak_draw_log(8, LOG_TOP, LOG_ROWS);
}

static void do_menu(long choice)
{
    short file_item;
    Boolean about, quit;
    short menu = HiWord(choice), item = LoWord(choice);

    if (!ak_menu_common(choice, &file_item, &about, &quit) && menu == AK_EDIT_MENU) {
        switch (item) {
        case 3: TECut(host_te); break;
        case 4: TECopy(host_te); break;
        case 5: TEPaste(host_te); break;
        case 6: TEDelete(host_te); break;
        }
    }
    if (about)
        ak_log("TalkIP Ping 0.3: ICMP echo over LocalTalk (talkip)", NULL);
    if (quit)
        quitting = true;
    HiliteMenu(0);
}

static void content_click(EventRecord *ev)
{
    Point p = ev->where;
    ControlHandle c;

    SetPort(win);
    GlobalToLocal(&p);
    if (PtInRect(p, &host_rect)) {
        TEClick(p, (ev->modifiers & shiftKey) != 0, host_te);
    } else if (FindControl(p, win, &c) && TrackControl(c, p, NULL)) {
        if (c == ping_btn)
            start_ping();
        else if (c == stop_btn && pinging)
            finish();
    }
}

int main(void)
{
    Rect r, view;
    EventRecord ev;
    char seen_status[80] = "";

    ak_init();
    ak_menus("About TalkIP Ping", NULL);

    SetRect(&r, 8, 44, 504, 334);
    win = NewWindow(NULL, &r, "\pTalkIP Ping", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);

    SetRect(&host_rect, 48, 8, 300, 26);
    view = host_rect;
    InsetRect(&view, 3, 2);
    TextFont(0);
    TextSize(12);
    host_te = TENew(&view, &view);
    SetRect(&r, 310, 7, 370, 27);
    ping_btn = NewControl(win, &r, "\pPing", true, 0, 0, 1, pushButProc, 0);
    SetRect(&r, 378, 7, 438, 27);
    stop_btn = NewControl(win, &r, "\pStop", true, 0, 0, 1, pushButProc, 0);
    HiliteControl(stop_btn, 255);
    TEActivate(host_te);

    ip_echo_reply_hook = echo_reply;
    draw();
    if (ak_net_start()) {
        char b[20] = "";
        fmt_ip(b, ip_parse(macip_cfg.gateway_name));
        TESetText(b, strlen(b), host_te);   /* the gateway: Return pings it at once */
        TESetSelect(0, 32767, host_te);
    }
    draw();

    while (!quitting) {
        if (WaitNextEvent(everyEvent, &ev, 1, NULL)) {
            WindowPtr w;
            switch (ev.what) {
            case mouseDown:
                switch (FindWindow(ev.where, &w)) {
                case inMenuBar:
                    do_menu(MenuSelect(ev.where));
                    break;
                case inSysWindow:
                    SystemClick(&ev, w);
                    break;
                case inDrag:
                    DragWindow(w, ev.where, &qd.screenBits.bounds);
                    break;
                case inGoAway:
                    if (TrackGoAway(w, ev.where))
                        quitting = true;
                    break;
                case inContent:
                    if (w != FrontWindow())
                        SelectWindow(w);
                    else
                        content_click(&ev);
                    break;
                }
                break;
            case keyDown:
            case autoKey: {
                char c = ev.message & charCodeMask;
                if (ev.modifiers & cmdKey) {
                    if (c == '.' && pinging)
                        finish();
                    else if (c == '.' && resolving) {
                        dns_cancel();
                        resolving = false;
                        HiliteControl(ping_btn, 0);
                        ak_log("Cancelled", NULL);
                    }
                    else
                        do_menu(MenuKey(c));
                } else if (c == '\r' || c == 3) {
                    start_ping();
                } else if (c != '\t') {
                    TEKey(c, host_te);
                }
                break;
            }
            case activateEvt:
                if (ev.modifiers & activeFlag)
                    TEActivate(host_te);
                else
                    TEDeactivate(host_te);
                break;
            case updateEvt:
                w = (WindowPtr)ev.message;
                BeginUpdate(w);
                if (w == win)
                    draw();
                EndUpdate(w);
                break;
            }
        }
        TEIdle(host_te);
        ak_net_poll();
        resolve_poll();
        ping_poll();
        {
            char b[80];
            ak_net_status(b);
            if (ak_log_changed() || strcmp(b, seen_status) != 0) {
                strcpy(seen_status, b);
                SetPort(win);
                draw();
            }
        }
    }
    ak_net_stop();
    return 0;
}
