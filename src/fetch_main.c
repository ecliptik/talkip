/*
 * TalkIP Fetch - milestone M4's test tool: open a TCP connection, send a request, show the first lines of
 * the reply and how fast it came. "host:port/path": port 70 sends a Gopher selector (default), 80 an HTTP/1.0
 * GET, 79 a Finger query (the path is the user); anything else sends the path and CRLF.
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
#include "tcp.h"
#include "macip.h"
#include "fmt.h"

#define LOG_TOP     50
#define LOG_ROWS    16
#define SHOW_LINES  10
#define IDLE_TIMEOUT_S 90   /* give up on a silent connection (a lost segment a gateway never resends, M7) */

enum { IDLE, RESOLVING, CONNECTING, RECEIVING };

static WindowPtr win;
static TEHandle url_te;
static ControlHandle go_btn;
static Rect url_rect;
static Boolean quitting;
static short phase = IDLE, conn = -1;
static unsigned char rbuf[4096];
static char host[64], path[128];
static unsigned short port;
static unsigned long started, received, heard;
static char line[80];
static short line_len, lines_shown;

static void parse_url(const char *s)
{
    const char *colon, *slash;
    size_t n;

    if (strncmp(s, "gopher://", 9) == 0)
        s += 9;
    else if (strncmp(s, "http://", 7) == 0) {
        s += 7;
        port = 80;
    }
    slash = strchr(s, '/');
    colon = strchr(s, ':');
    if (colon && slash && colon > slash)
        colon = NULL;
    n = colon ? (size_t)(colon - s) : slash ? (size_t)(slash - s) : strlen(s);
    if (n > sizeof(host) - 1)
        n = sizeof(host) - 1;
    memcpy(host, s, n);
    host[n] = '\0';
    if (colon) {
        port = 0;
        for (colon++; *colon >= '0' && *colon <= '9'; colon++)
            port = (unsigned short)(port * 10 + (*colon - '0'));
    } else if (port == 0) {
        port = 70;
    }
    strncpy(path, slash ? slash + (port == 70 || port == 79 ? 1 : 0) : (port == 80 ? "/" : ""), sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
}

static void finish(const char *why)
{
    char b[96] = "";
    unsigned long ms = (TickCount() - started) * 1000UL / 60;

    if (line_len && lines_shown < SHOW_LINES)
        ak_log("  ", line);
    fmt_ul(b, received);
    fmt_str(b, " bytes in ");
    fmt_ul(b, ms);
    fmt_str(b, " ms");
    if (ms) {
        fmt_str(b, " (");
        fmt_ul(b, received * 1000UL / ms);
        fmt_str(b, " bytes/s)");
    }
    fmt_str(b, ", ");
    fmt_str(b, why);
    ak_log(b, NULL);
    if (conn >= 0)
        tcp_release(conn);
    conn = -1;
    phase = IDLE;
    HiliteControl(go_btn, 0);
}

static void connect_to(unsigned long ip)
{
    char b[96] = "";

    conn = tcp_open(ip, port, rbuf, sizeof(rbuf));
    if (conn < 0) {
        ak_log("Can't open a connection", NULL);
        phase = IDLE;
        HiliteControl(go_btn, 0);
        return;
    }
    fmt_str(b, "Connecting to ");
    fmt_ip(b, ip);
    fmt_str(b, ":");
    fmt_ul(b, port);
    ak_log(b, NULL);
    phase = CONNECTING;
    started = TickCount();
}

static void start(void)
{
    CharsHandle h = TEGetText(url_te);
    short n = (**url_te).teLength;
    char url[200];
    unsigned long ip;

    if (phase != IDLE)
        return;
    if (n > (short)sizeof(url) - 1)
        n = sizeof(url) - 1;
    memcpy(url, *h, n);
    url[n] = '\0';
    port = 0;
    parse_url(url);
    if (host[0] == '\0')
        return;
    received = 0;
    line_len = 0;
    lines_shown = 0;
    HiliteControl(go_btn, 255);
    ip = ip_parse(host);
    if (ip) {
        connect_to(ip);
    } else if (dns_begin(host) == noErr) {
        phase = RESOLVING;
    }
}

static void take_bytes(const unsigned char *p, unsigned short n)
{
    unsigned short i;

    received += n;
    for (i = 0; i < n && lines_shown < SHOW_LINES; i++) {
        char c = (char)p[i];
        if (c == '\n' || line_len == (short)sizeof(line) - 1) {
            line[line_len] = '\0';
            ak_log("  ", line);
            lines_shown++;
            line_len = 0;
        } else if (c != '\r') {
            line[line_len++] = (c == '\t') ? ' ' : c;
        }
    }
}

static void fetch_poll(void)
{
    if (phase == RESOLVING) {
        DNSResult r;
        if (dns_poll(&r) == DNS_PENDING)
            return;
        if (r.err != noErr || r.naddrs == 0) {
            ak_log(host, ": not found");
            phase = IDLE;
            HiliteControl(go_btn, 0);
            return;
        }
        connect_to(r.addr[0]);
    } else if (phase == CONNECTING) {
        short st = tcp_state(conn);
        if (st == TCP_SYN_SENT)
            return;
        if (st != TCP_ESTABLISHED) {
            finish(tcp_error(conn) == TCP_ERR_REFUSED ? "refused" : "no connection");
            return;
        }
        {
            char req[200] = "";
            if (port == 80) {
                fmt_str(req, "GET ");
                fmt_str(req, path);
                fmt_str(req, " HTTP/1.0\r\nHost: ");
                fmt_str(req, host);
                fmt_str(req, "\r\nUser-Agent: TalkIP/0.4\r\n\r\n");
            } else {
                fmt_str(req, path);
                fmt_str(req, "\r\n");
            }
            tcp_send(conn, req, (unsigned short)strlen(req));
        }
        ak_log("Connected, request sent", NULL);
        phase = RECEIVING;
        heard = TickCount();
    } else if (phase == RECEIVING) {
        unsigned char b[512];
        unsigned short n;
        while ((n = tcp_recv(conn, b, sizeof(b))) > 0) {
            heard = TickCount();
            take_bytes(b, n);
        }
        if (tcp_peer_closed(conn) && tcp_unread(conn) == 0) {
            tcp_close(conn);
            finish("closed by the server");
        } else if (tcp_state(conn) == TCP_CLOSED) {
            finish(tcp_error(conn) == TCP_ERR_RESET ? "reset" : "timed out");
        } else if (TickCount() - heard > IDLE_TIMEOUT_S * 60UL) {
            tcp_abort(conn);
            finish("no data for 90 s, gave up");
        }
    }
}

static void draw(void)
{
    char b[96];

    SetPort(win);
    EraseRect(&win->portRect);
    TextFont(0);
    TextSize(12);
    ak_draw_text(8, 21, "URL:");
    FrameRect(&url_rect);
    TEUpdate(&win->portRect, url_te);
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
        case 3: TECut(url_te); break;
        case 4: TECopy(url_te); break;
        case 5: TEPaste(url_te); break;
        case 6: TEDelete(url_te); break;
        }
    }
    if (about)
        ak_log("TalkIP Fetch 0.4: raw TCP requests over LocalTalk (talkip)", NULL);
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
    if (PtInRect(p, &url_rect))
        TEClick(p, (ev->modifiers & shiftKey) != 0, url_te);
    else if (FindControl(p, win, &c) && TrackControl(c, p, NULL) && c == go_btn)
        start();
}

int main(void)
{
    Rect r, view;
    EventRecord ev;
    char seen_status[96] = "";

    ak_init();
    ak_menus("About TalkIP Fetch", NULL);

    SetRect(&r, 8, 44, 504, 334);
    win = NewWindow(NULL, &r, "\pTalkIP Fetch", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);
    SetRect(&url_rect, 44, 8, 380, 26);
    view = url_rect;
    InsetRect(&view, 3, 2);
    TextFont(0);
    TextSize(12);
    url_te = TENew(&view, &view);
    TESetText("floodgap.com:70/", 16, url_te);
    TESetSelect(0, 32767, url_te);
    SetRect(&r, 390, 7, 450, 27);
    go_btn = NewControl(win, &r, "\pFetch", true, 0, 0, 1, pushButProc, 0);
    TEActivate(url_te);

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
                    if (c == '.' && phase != IDLE) {
                        if (phase == RESOLVING)
                            dns_cancel();
                        if (conn >= 0)
                            tcp_abort(conn);
                        finish("cancelled");
                    } else {
                        do_menu(MenuKey(c));
                    }
                } else if (c == '\r' || c == 3) {
                    start();
                } else if (c != '\t') {
                    TEKey(c, url_te);
                }
                break;
            }
            case activateEvt:
                if (ev.modifiers & activeFlag)
                    TEActivate(url_te);
                else
                    TEDeactivate(url_te);
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
        TEIdle(url_te);
        ak_net_poll();
        fetch_poll();
        {
            char b[96];
            ak_net_status(b);
            if (ak_log_changed() || strcmp(b, seen_status) != 0) {
                strcpy(seen_status, b);
                draw();
            }
        }
    }
    if (conn >= 0)
        tcp_abort(conn);
    ak_net_stop();
    return 0;
}
