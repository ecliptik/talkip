/*
 * TalkIP Status - milestone M1: open socket 72, find the MacIP gateway, lease an address, answer pings, and
 * show it all. Stays useful as the diagnostic window for the stack.
 */
#include <string.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <ToolUtils.h>

#include "ddp.h"
#include "macip.h"
#include "ip.h"
#include "fmt.h"

#define APPLE_MENU  128
#define FILE_MENU   129
#define LOG_LINES   7
#define LINE_H      12

static WindowPtr win;
static MenuHandle apple_menu, file_menu;
static char log_text[LOG_LINES][60];
static Boolean quitting;
static unsigned long last_draw;
static MacIPState last_state;

static void tlog(const char *a, const char *b)
{
    int i;

    for (i = 0; i < LOG_LINES - 1; i++)
        strcpy(log_text[i], log_text[i + 1]);
    log_text[LOG_LINES - 1][0] = '\0';
    strncat(log_text[LOG_LINES - 1], a, 59);
    if (b)
        strncat(log_text[LOG_LINES - 1], b, 59 - strlen(log_text[LOG_LINES - 1]));
}

static void tlog_err(const char *what, short err)
{
    char b[16] = "";

    fmt_err(b, err);
    tlog(what, b);
}

static void draw_line(short row, const char *s)
{
    Str255 p;

    MoveTo(8, 14 + row * LINE_H);
    DrawString(fmt_pstr(p, s));
}

static const char *state_name(void)
{
    switch (macip_state) {
    case MACIP_DOWN: return "down";
    case MACIP_LOOKUP: return "looking for a gateway";
    case MACIP_ASSIGN: return "asking for an address";
    case MACIP_UP: return "up";
    default: return "failed";
    }
}

static void draw(void)
{
    char b[96];
    short row = 0;
    int i;

    SetPort(win);
    EraseRect(&win->portRect);

    b[0] = '\0';
    fmt_str(b, "AppleTalk driver v");
    fmt_err(b, ddp_driver_version());
    /* learned from datagrams to socket 72 (GetNodeAddress is MPW glue only; its ABusVars offsets are unverified) */
    fmt_str(b, "   us (seen on socket 72): ");
    if (ddp_my_node) {
        fmt_ul(b, ddp_my_net);
        fmt_str(b, ".");
        fmt_ul(b, ddp_my_node);
    } else {
        fmt_str(b, "-");
    }
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "Gateway: ");
    if (macip_cfg.gateway.node) {
        fmt_str(b, macip_cfg.gateway_name);
        fmt_str(b, " at ");
        fmt_ul(b, macip_cfg.gateway.net);
        fmt_str(b, ".");
        fmt_ul(b, macip_cfg.gateway.node);
        fmt_str(b, ":");
        fmt_ul(b, macip_cfg.gateway.socket);
    } else {
        fmt_str(b, "-");
    }
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "MacIP: ");
    fmt_str(b, state_name());
    if (macip_error[0]) {
        fmt_str(b, " (");
        fmt_str(b, macip_error);
        fmt_str(b, ")");
    }
    if (macip_recovering())
        fmt_str(b, ", retrying");
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "IP ");
    fmt_ip(b, macip_cfg.ip);
    fmt_str(b, "  mask ");
    fmt_ip(b, macip_cfg.netmask);
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "DNS ");
    fmt_ip(b, macip_cfg.nameserver);
    fmt_str(b, "  broadcast ");
    fmt_ip(b, macip_cfg.broadcast);
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "DDP in ");
    fmt_ul(b, ddp_stats.rx);
    fmt_str(b, "  out ");
    fmt_ul(b, ddp_stats.tx);
    fmt_str(b, "  dropped ");
    fmt_ul(b, ddp_rx_dropped);
    fmt_str(b, "  send errors ");
    fmt_ul(b, ddp_stats.tx_err);
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "IP in ");
    fmt_ul(b, ip_stats.rx);
    fmt_str(b, "  out ");
    fmt_ul(b, ip_stats.tx);
    fmt_str(b, "  bad ");
    fmt_ul(b, ip_stats.bad);
    fmt_str(b, "  other ");
    fmt_ul(b, ip_stats.not_ours + ip_stats.unhandled);
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "Pings answered ");
    fmt_ul(b, ip_stats.echo_replies);
    fmt_str(b, "  probes ");
    fmt_ul(b, macip_stats.keepalives);
    fmt_str(b, "  recoveries ");
    fmt_ul(b, macip_stats.recoveries);
    if (macip_state == MACIP_UP) {
        fmt_str(b, "  gateway heard ");
        fmt_ul(b, (TickCount() - macip_last_heard) / 60);
        fmt_str(b, " s ago");
    }
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "Listener calls ");
    fmt_ul(b, ddp_rx_calls);
    fmt_str(b, "  read errors ");
    fmt_ul(b, ddp_rx_errors);
    draw_line(row++, b);

    b[0] = '\0';
    fmt_str(b, "Memory free ");
    fmt_ul(b, (unsigned long)FreeMem() / 1024);
    fmt_str(b, " K, largest block ");
    fmt_ul(b, (unsigned long)MaxBlock() / 1024);
    fmt_str(b, " K");
    draw_line(row++, b);

    row++;
    for (i = 0; i < LOG_LINES; i++)
        draw_line(row++, log_text[i]);
    last_draw = TickCount();
}

static void start(void)
{
    OSErr err;
    char b[40] = "";

    tlog("Opening AppleTalk socket 72", NULL);
    draw();
    err = ddp_open();
    if (err != noErr) {
        tlog_err("Can't open AppleTalk: error ", err);
        return;
    }
    tlog("Looking up =:IPGATEWAY@*", NULL);
    draw();
    if (macip_find_gateway() != noErr) {
        tlog(macip_error, ", will retry");
        macip_retry_later();
        return;
    }
    tlog("Found gateway ", macip_cfg.gateway_name);
    tlog("Asking for an address", NULL);
    draw();
    if (macip_assign() != noErr) {
        tlog(macip_error, ", will retry");
        macip_retry_later();
        return;
    }
    fmt_ip(b, macip_cfg.ip);
    tlog("Leased ", b);
}

static void do_menu(long choice)
{
    short menu = HiWord(choice), item = LoWord(choice);

    if (menu == APPLE_MENU) {
        if (item == 1) {
            tlog("TalkIP Status 0.1: TCP/IP over LocalTalk", NULL);
        } else {
            Str255 name;
            GetMenuItemText(apple_menu, item, name);
            OpenDeskAcc(name);
        }
    } else if (menu == FILE_MENU) {
        if (item == 1) {
            if (macip_cfg.gateway.node == 0) {
                start();
            } else {
                tlog("Renewing the lease", NULL);
                draw();
                if (macip_assign() == noErr) {
                    char b[20] = "";
                    tlog("Leased ", fmt_ip(b, macip_cfg.ip));
                } else {
                    tlog(macip_error, NULL);
                }
            }
            last_state = macip_state;   /* logged here already */
            draw();
        } else if (item == 3) {
            /* Crash-safety test: leave without ddp_close, the way a crashed app leaves. ddp.c's ExitToShell
             * patch must close socket 72; if it didn't, macipgw's next ping would crash the Mac. */
            ExitToShell();
        } else if (item == 4) {
            quitting = true;
        }
    }
    HiliteMenu(0);
}

/* The bomb box's Resume button: leave through ExitToShell, which ddp.c patches to close socket 72 first */
static void resume_proc(void)
{
    ExitToShell();
}

int main(void)
{
    Rect r;
    EventRecord ev;
    unsigned long seen = 0;

    MaxApplZone();                      /* grow the heap now, so FreeMem shows what the app really has */
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs((void *)resume_proc);
    InitCursor();

    apple_menu = NewMenu(APPLE_MENU, "\p\024");
    AppendMenu(apple_menu, "\pAbout TalkIP Status;(-");
    AppendResMenu(apple_menu, 'DRVR');
    InsertMenu(apple_menu, 0);
    file_menu = NewMenu(FILE_MENU, "\pFile");
    AppendMenu(file_menu, "\pRenew Lease/R;(-;Quit via ExitToShell - test;Quit/Q");
    InsertMenu(file_menu, 0);
    DrawMenuBar();

    SetRect(&r, 8, 44, 504, 334);
    win = NewWindow(NULL, &r, "\pTalkIP Status", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);
    TextFont(4);           /* Monaco */
    TextSize(9);

    start();
    last_state = macip_state;
    draw();

    while (!quitting) {
        if (WaitNextEvent(everyEvent, &ev, 2, NULL)) {
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
                    break;
                }
                break;
            case keyDown:
            case autoKey:
                if (ev.modifiers & cmdKey)
                    do_menu(MenuKey(ev.message & charCodeMask));
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
        macip_poll();
        if (macip_state != last_state) {        /* log transitions: probes failing, recovery, lease */
            if (macip_state == MACIP_UP) {
                char b[20] = "";
                tlog("Leased ", fmt_ip(b, macip_cfg.ip));
            } else if (macip_error[0]) {
                tlog(macip_error, NULL);
            }
            last_state = macip_state;
        }
        /* redraw at most twice a second when something moved, and every 5 s for the "heard" age */
        {
            unsigned long now = ddp_stats.rx + ddp_stats.tx + ddp_rx_dropped + ddp_rx_calls + macip_stats.keepalives
                + macip_stats.recoveries + TickCount() / 300;
            if (now != seen && TickCount() - last_draw > 30) {
                seen = now;
                draw();
            }
        }
    }
    ddp_close();
    return 0;
}
