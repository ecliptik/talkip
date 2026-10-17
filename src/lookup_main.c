/*
 * TalkIP Lookup - milestone M3: DNS from the 512Ke. A name gives its addresses (and canonical name); a dotted
 * address gives its name (PTR). Uses the nameserver the MacIP gateway handed out.
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
#include "dns.h"
#include "ip.h"
#include "macip.h"
#include "fmt.h"

#define LOG_TOP     50
#define LOG_ROWS    16

static WindowPtr win;
static TEHandle name_te;
static ControlHandle look_btn;
static Rect name_rect;
static Boolean quitting, looking;
static char query[DNS_NAME_MAX];

static void start_lookup(void)
{
    CharsHandle h = TEGetText(name_te);
    short n = (**name_te).teLength;

    if (looking)
        return;
    if (n > DNS_NAME_MAX - 1)
        n = DNS_NAME_MAX - 1;
    memcpy(query, *h, n);
    query[n] = '\0';
    if (n == 0)
        return;
    if (dns_begin(query) == noErr) {
        looking = true;
        HiliteControl(look_btn, 255);
    }
}

static void lookup_poll(void)
{
    DNSResult r;
    char b[96];
    unsigned short i;

    if (!looking || dns_poll(&r) == DNS_PENDING)
        return;
    looking = false;
    HiliteControl(look_btn, 0);
    if (r.err != noErr) {
        b[0] = '\0';
        fmt_str(b, query);
        fmt_str(b, ": ");
        fmt_str(b, dns_error_text(r.err));
        ak_log(b, NULL);
        return;
    }
    ak_log(query, NULL);
    for (i = 0; i < r.naddrs; i++) {
        b[0] = '\0';
        fmt_str(b, "  address ");
        fmt_ip(b, r.addr[i]);
        ak_log(b, NULL);
    }
    if (r.cname[0])
        ak_log("  canonical name ", r.cname);
    if (r.ptr[0])
        ak_log("  name ", r.ptr);
    b[0] = '\0';
    fmt_str(b, "  TTL ");
    fmt_ul(b, r.ttl);
    fmt_str(b, " s, ");
    if (r.cached) {
        fmt_str(b, "from the cache");
    } else {
        fmt_ul(b, (r.ticks * 1000UL + 30) / 60);
        fmt_str(b, " ms");
        if (r.tries > 1) {
            fmt_str(b, ", ");
            fmt_ul(b, r.tries);
            fmt_str(b, " tries");
        }
    }
    ak_log(b, NULL);
}

static void draw(void)
{
    char b[96];

    SetPort(win);
    EraseRect(&win->portRect);
    TextFont(0);
    TextSize(12);
    ak_draw_text(8, 21, "Name:");
    FrameRect(&name_rect);
    TEUpdate(&win->portRect, name_te);
    DrawControls(win);
    TextFont(4);
    TextSize(9);
    ak_net_status(b);
    if (macip_state == MACIP_UP) {
        fmt_str(b, ", DNS ");
        fmt_ip(b, macip_cfg.nameserver);
    }
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
        case 3: TECut(name_te); break;
        case 4: TECopy(name_te); break;
        case 5: TEPaste(name_te); break;
        case 6: TEDelete(name_te); break;
        }
    }
    if (about)
        ak_log("TalkIP Lookup 0.3: DNS over LocalTalk (talkip)", NULL);
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
    if (PtInRect(p, &name_rect))
        TEClick(p, (ev->modifiers & shiftKey) != 0, name_te);
    else if (FindControl(p, win, &c) && TrackControl(c, p, NULL) && c == look_btn)
        start_lookup();
}

int main(void)
{
    Rect r, view;
    EventRecord ev;
    char seen_status[96] = "";

    ak_init();
    ak_menus("About TalkIP Lookup", NULL);

    SetRect(&r, 8, 44, 504, 334);
    win = NewWindow(NULL, &r, "\pTalkIP Lookup", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);
    SetRect(&name_rect, 52, 8, 360, 26);
    view = name_rect;
    InsetRect(&view, 3, 2);
    TextFont(0);
    TextSize(12);
    name_te = TENew(&view, &view);
    TESetText("floodgap.com", 12, name_te);
    TESetSelect(0, 32767, name_te);
    SetRect(&r, 370, 7, 450, 27);
    look_btn = NewControl(win, &r, "\pLook Up", true, 0, 0, 1, pushButProc, 0);
    TEActivate(name_te);

    draw();
    ak_net_start();
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
                    if (c == '.' && looking) {
                        dns_cancel();
                        looking = false;
                        HiliteControl(look_btn, 0);
                        ak_log("Cancelled", NULL);
                    } else {
                        do_menu(MenuKey(c));
                    }
                } else if (c == '\r' || c == 3) {
                    start_lookup();
                } else if (c != '\t') {
                    TEKey(c, name_te);
                }
                break;
            }
            case activateEvt:
                if (ev.modifiers & activeFlag)
                    TEActivate(name_te);
                else
                    TEDeactivate(name_te);
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
        TEIdle(name_te);
        ak_net_poll();
        lookup_poll();
        {
            char b[96];
            ak_net_status(b);
            if (ak_log_changed() || strcmp(b, seen_status) != 0) {
                strcpy(seen_status, b);
                draw();
            }
        }
    }
    ak_net_stop();
    return 0;
}
