/*
 * TalkIP Web - milestone M6: an extremely basic web browser for the 512Ke. HTTP/1.0 over talkip, HTML
 * rendered as text with bold, headings, bullets and underlined links (html.c), scrolling, Back.
 * https:// sites go through a proxy (macproxy) named in "TalkIP Web Settings", a text file next to the
 * app:   proxy gateway:5001       ("gateway" = the MacIP gateway's address: macproxy on the gateway host)
 *        home http://info.cern.ch/
 */
#include <string.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Events.h>
#include <Files.h>
#include <ToolUtils.h>

#include "appkit.h"
#include "html.h"
#include "http.h"
#include "url.h"
#include "macip.h"
#include "fmt.h"

#define HISTORY         8
#define SETTINGS_NAME   "\pTalkIP Web Settings"
#define FONT_ID         3               /* Geneva */
#define FONT_SIZE       10

static WindowPtr win;
static TEHandle url_te;
static ControlHandle back_btn, go_btn, vscroll;
static Rect url_rect, page_rect;
static Boolean quitting;
static Doc doc;
static HttpReq req;
static char page_url[URL_MAX];
static char history[HISTORY][URL_MAX];
static short hist_len;
static char home[URL_MAX] = "http://info.cern.ch/";
static char status[96] = "";
static short line_h, ascent, top_line, page_lines;
static unsigned long last_draw;
static Boolean dirty, going_back;
static Boolean home_pending;            /* no gateway at startup: load home once MacIP comes up */
static short shown_macip = -1;          /* macip_state when the status line was last marked for redraw */

/* ---- settings ---- */

static void load_settings(void)
{
    short ref;
    char buf[400];
    long n = sizeof(buf) - 1;
    char *line, *next;

    if (FSOpen(SETTINGS_NAME, 0, &ref) != noErr) {
        static const char defaults[] =
            "home http://info.cern.ch/\r"
            "proxy gateway:5001\r"
            "-- TalkIP Web: proxy host:port (macproxy) for https sites; gateway = the MacIP gateway; none = off\r";
        long len = sizeof(defaults) - 1;
        if (Create(SETTINGS_NAME, 0, 'ttxt', 'TEXT') == noErr && FSOpen(SETTINGS_NAME, 0, &ref) == noErr) {
            FSWrite(ref, &len, defaults);
            FSClose(ref);
        }
        http_set_proxy("gateway", 5001);    /* the defaults just written */
        return;
    }
    if (FSRead(ref, &n, buf) != noErr && n <= 0)
        n = 0;
    FSClose(ref);
    buf[n] = '\0';
    for (line = buf; line && *line; line = next) {
        next = strpbrk(line, "\r\n");
        if (next)
            *next++ = '\0';
        if (strncmp(line, "home ", 5) == 0) {
            strncpy(home, line + 5, sizeof(home) - 1);
        } else if (strncmp(line, "proxy ", 6) == 0 && strcmp(line + 6, "none") != 0) {
            char host[64];
            char *colon = strchr(line + 6, ':');
            unsigned short port = 5001;
            size_t hn = colon ? (size_t)(colon - (line + 6)) : strlen(line + 6);
            if (hn >= sizeof(host))
                hn = sizeof(host) - 1;
            memcpy(host, line + 6, hn);
            host[hn] = '\0';
            if (colon) {
                port = 0;
                for (colon++; *colon >= '0' && *colon <= '9'; colon++)
                    port = (unsigned short)(port * 10 + (*colon - '0'));
            }
            http_set_proxy(host, port);
        }
    }
}

/* ---- drawing ---- */

static void set_status(const char *a, const char *b)
{
    status[0] = '\0';
    strncat(status, a, sizeof(status) - 1);
    if (b)
        strncat(status, b, sizeof(status) - 1 - strlen(status));
    dirty = true;
}

static void draw_status(void)
{
    Rect r;
    char b[96];

    SetRect(&r, 0, win->portRect.bottom - 14, win->portRect.right, win->portRect.bottom);
    EraseRect(&r);
    MoveTo(0, r.top);
    LineTo(r.right, r.top);
    TextFont(FONT_ID);
    TextSize(9);
    TextFace(0);
    strcpy(b, status);
    if (macip_state != MACIP_UP)
        strcpy(b, macip_recovering() ? "Looking for the MacIP gateway..." : "MacIP: no address");
    ak_draw_text(4, r.bottom - 3, b);
}

static void draw_line(short i, short y)
{
    DocLine *l = &doc.lines[i];
    const char *t = doc.text + l->start;
    short n = (short)l->len, k = 0, run = 0;
    Boolean b = l->bold;
    Boolean link = l->link != 0;

    MoveTo(page_rect.left + 4, y);
    TextFace((b ? bold : 0) | (link ? underline : 0));
    while (k <= n) {
        unsigned char c = k < n ? (unsigned char)t[k] : 0;
        if (k == n || c < 0x20) {
            if (k > run)
                DrawText((Ptr)t, run, k - run);
            if (k == n)
                break;
            if (c == DOC_BOLD_ON || c == DOC_BOLD_OFF)
                b = (c == DOC_BOLD_ON);
            else if (c == DOC_LINK_ON) {
                link = true;
                k++;                    /* the index byte */
            } else if (c == DOC_LINK_OFF)
                link = false;
            else if (c == DOC_RULE) {
                Point p;
                GetPen(&p);
                MoveTo(page_rect.left + 4, y - ascent / 2);
                LineTo(page_rect.right - 4, y - ascent / 2);
                MoveTo(p.h, p.v);
            }
            TextFace((b ? bold : 0) | (link ? underline : 0));
            run = k + 1;
        }
        k++;
    }
}

static void draw_page(void)
{
    short i, y;
    RgnHandle save = NewRgn();

    GetClip(save);
    ClipRect(&page_rect);
    EraseRect(&page_rect);
    TextFont(FONT_ID);
    TextSize(FONT_SIZE);
    for (i = top_line, y = page_rect.top + ascent + 2; i < doc.nlines && y < page_rect.bottom + ascent;
         i++, y += line_h)
        draw_line(i, y);
    TextFace(0);
    SetClip(save);
    DisposeRgn(save);
}

static void update_scroll(void)
{
    short max = doc.nlines - page_lines;

    if (max < 0)
        max = 0;
    if (top_line > max)
        top_line = max;
    SetControlMaximum(vscroll, max);
    SetControlValue(vscroll, top_line);
}

static void draw_all(void)
{
    SetPort(win);
    EraseRect(&win->portRect);
    TextFont(0);
    TextSize(12);
    FrameRect(&url_rect);
    TEUpdate(&win->portRect, url_te);
    DrawControls(win);
    MoveTo(0, page_rect.top - 1);
    LineTo(win->portRect.right, page_rect.top - 1);
    draw_page();
    draw_status();
    last_draw = TickCount();
    dirty = false;
}

/* ---- navigation ---- */

static void set_url_field(const char *u)
{
    TESetText((Ptr)u, strlen(u), url_te);
    TESetSelect(0, 32767, url_te);
    SetPort(win);
    InvalRect(&url_rect);
}

static void on_start(HttpReq *r)
{
    Boolean plain = strncmp(r->content_type, "text/plain", 10) == 0;

    SetPort(win);
    TextFont(FONT_ID);
    TextSize(FONT_SIZE);
    doc_begin(&doc, plain, (short)(page_rect.right - page_rect.left - 8));
    top_line = 0;
    strcpy(page_url, r->url);
    set_url_field(r->url);
    set_status("Loading ", r->u.host);
}

static void on_body(HttpReq *r, const unsigned char *data, unsigned short len)
{
    (void)r;
    SetPort(win);
    TextFont(FONT_ID);
    TextSize(FONT_SIZE);
    doc_feed(&doc, data, len);
    dirty = true;
}

static void go(const char *url)
{
    char full[URL_MAX];

    if (strstr(url, "://") == NULL) {   /* "example.com" -> "http://example.com" */
        strcpy(full, "http://");
        strncat(full, url, sizeof(full) - 8);
    } else {
        strncpy(full, url, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    }
    http_cancel(&req);
    if (!going_back && page_url[0]) {
        if (hist_len == HISTORY) {
            memmove(history[0], history[1], (HISTORY - 1) * URL_MAX);
            hist_len--;
        }
        strcpy(history[hist_len++], page_url);
    }
    going_back = false;
    HiliteControl(back_btn, hist_len ? 0 : 255);
    SetControlTitle(go_btn, "\pStop");
    if (http_begin(&req, full, on_start, on_body) == noErr)
        set_status("Opening ", req.u.host);
    else
        set_status(req.error, NULL);
}

static void go_back(void)
{
    if (hist_len == 0)
        return;
    going_back = true;
    go(history[--hist_len]);
}

static void poll_http(void)
{
    short before = req.state;

    if (req.state == HTTP_IDLE || req.state == HTTP_DONE || req.state == HTTP_FAILED)
        return;
    http_poll(&req);
    if (req.state == HTTP_DONE && before != HTTP_DONE) {
        char b[96] = "Done: ";
        SetPort(win);
        TextFont(FONT_ID);
        TextSize(FONT_SIZE);
        doc_end(&doc);
        fmt_ul(b, (req.bytes + 512) / 1024);
        fmt_str(b, " KB in ");
        fmt_ul(b, (TickCount() - req.started) / 60);
        fmt_str(b, " s");
        if (doc.truncated)
            fmt_str(b, " (cut short: too long for this Mac)");
        set_status(b, NULL);
        SetControlTitle(go_btn, "\pGo");
        if (doc.title[0]) {
            Str255 p;
            SetWTitle(win, fmt_pstr(p, doc.title));
        }
    } else if (req.state == HTTP_FAILED) {
        set_status(req.error, NULL);
        SetControlTitle(go_btn, "\pGo");
    } else if (req.state == HTTP_HEADERS && before != HTTP_HEADERS) {
        set_status("Waiting for ", req.u.host);
    } else if (req.state == HTTP_BODY && (TickCount() - last_draw) > 30) {
        char b[96] = "Loading: ";
        fmt_ul(b, req.bytes / 1024);
        fmt_str(b, " KB");
        set_status(b, NULL);
    }
}

/* ---- clicks ---- */

static void click_page(Point p)
{
    short i = top_line + (short)((p.v - page_rect.top - 2) / line_h);
    DocLine *l;
    const char *t;
    short k, x;
    Boolean b;
    short link;

    if (i < 0 || i >= doc.nlines)
        return;
    l = &doc.lines[i];
    t = doc.text + l->start;
    b = l->bold;
    link = l->link;
    x = page_rect.left + 4;
    TextFont(FONT_ID);
    TextSize(FONT_SIZE);
    for (k = 0; k < (short)l->len; k++) {
        unsigned char c = (unsigned char)t[k];
        if (c == DOC_BOLD_ON || c == DOC_BOLD_OFF) {
            b = (c == DOC_BOLD_ON);
            continue;
        }
        if (c == DOC_LINK_ON) {
            link = (unsigned char)t[++k] + 1;
            continue;
        }
        if (c == DOC_LINK_OFF) {
            link = 0;
            continue;
        }
        if (c < 0x20)
            continue;
        TextFace(b ? bold : 0);
        x += CharWidth(c);
        if (x > p.h) {
            if (link) {
                char target[URL_MAX];
                url_resolve(page_url, doc_link(&doc, (short)(link - 1)), target);
                TextFace(0);
                go(target);
            }
            break;
        }
    }
    TextFace(0);
}

static pascal void scroll_action(ControlHandle c, short part)
{
    short delta = 0, v;

    switch (part) {
    case inUpButton: delta = -1; break;
    case inDownButton: delta = 1; break;
    case inPageUp: delta = -(page_lines - 1); break;
    case inPageDown: delta = page_lines - 1; break;
    default: return;
    }
    v = GetControlValue(c) + delta;
    SetControlValue(c, v);
    if (GetControlValue(c) != top_line) {
        top_line = GetControlValue(c);
        draw_page();
    }
}

static void content_click(EventRecord *ev)
{
    Point p = ev->where;
    ControlHandle c;
    short part;

    SetPort(win);
    GlobalToLocal(&p);
    if (PtInRect(p, &url_rect)) {
        TEClick(p, (ev->modifiers & shiftKey) != 0, url_te);
        return;
    }
    part = FindControl(p, win, &c);
    if (c == vscroll && part) {
        if (part == inThumb) {
            TrackControl(c, p, NULL);
            top_line = GetControlValue(c);
            draw_page();
        } else {
            TrackControl(c, p, (ControlActionUPP)scroll_action);
        }
    } else if (part && TrackControl(c, p, NULL)) {
        if (c == back_btn)
            go_back();
        else if (c == go_btn) {
            if (req.state == HTTP_RESOLVING || req.state == HTTP_CONNECTING || req.state == HTTP_HEADERS
                || req.state == HTTP_BODY) {
                http_cancel(&req);
                SetControlTitle(go_btn, "\pGo");
                set_status("Stopped", NULL);
            } else {
                char u[URL_MAX];
                short n = (**url_te).teLength;
                if (n > URL_MAX - 1)
                    n = URL_MAX - 1;
                memcpy(u, *TEGetText(url_te), n);
                u[n] = '\0';
                go(u);
            }
        }
    } else if (PtInRect(p, &page_rect)) {
        click_page(p);
    }
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
        set_status("TalkIP Web 0.6: HTTP/1.0 over LocalTalk (talkip)", NULL);
    if (file_item == 1)
        go(home);
    else if (file_item == 2)
        go_back();
    if (quit)
        quitting = true;
    HiliteMenu(0);
}

int main(void)
{
    Rect r, view;
    EventRecord ev;
    FontInfo fi;

    ak_init();
    ak_menus("About TalkIP Web", "Home/H;Back/[");
    load_settings();

    SetRect(&r, 2, 40, 510, 340);
    win = NewWindow(NULL, &r, "\pTalkIP Web", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);

    SetRect(&r, 4, 4, 30, 22);
    back_btn = NewControl(win, &r, "\p<", true, 0, 0, 1, pushButProc, 0);
    HiliteControl(back_btn, 255);
    SetRect(&url_rect, 36, 4, 452, 22);
    view = url_rect;
    InsetRect(&view, 3, 2);
    TextFont(0);
    TextSize(12);
    url_te = TENew(&view, &view);
    SetRect(&r, 458, 4, 504, 22);
    go_btn = NewControl(win, &r, "\pGo", true, 0, 0, 1, pushButProc, 0);
    SetRect(&page_rect, 0, 27, win->portRect.right - 15, win->portRect.bottom - 14);
    SetRect(&r, win->portRect.right - 16, 26, win->portRect.right + 1, win->portRect.bottom - 13);
    vscroll = NewControl(win, &r, "\p", true, 0, 0, 0, scrollBarProc, 0);

    TextFont(FONT_ID);
    TextSize(FONT_SIZE);
    GetFontInfo(&fi);
    ascent = fi.ascent;
    line_h = fi.ascent + fi.descent + fi.leading;
    page_lines = (short)((page_rect.bottom - page_rect.top - 2) / line_h);

    if (!doc_init(&doc)) {
        set_status("Not enough memory for TalkIP Web", NULL);
        draw_all();
    } else {
        set_url_field(home);
        set_status("Looking for the MacIP gateway", NULL);
        draw_all();
        if (ak_net_start())
            go(home);
        else
            home_pending = true;        /* e.g. the gateway missed the first NBP lookup after boot (M7) */
    }
    TEActivate(url_te);

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
                    if (c == '.') {
                        http_cancel(&req);
                        SetControlTitle(go_btn, "\pGo");
                        set_status("Stopped", NULL);
                    } else {
                        do_menu(MenuKey(c));
                    }
                } else if (c == '\r' || c == 3) {
                    char u[URL_MAX];
                    short n = (**url_te).teLength;
                    if (n > URL_MAX - 1)
                        n = URL_MAX - 1;
                    memcpy(u, *TEGetText(url_te), n);
                    u[n] = '\0';
                    go(u);
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
                if (w == win) {
                    /* BeginUpdate clips drawing to the update region (often just the URL field, after
                     * set_url_field), so this paints only that much: keep a pending full redraw pending.
                     * Clearing it left "Loading: 1 KB" on screen for a page that had finished (M7). */
                    Boolean pending = dirty;
                    draw_all();
                    dirty = pending;
                }
                EndUpdate(w);
                break;
            }
        }
        TEIdle(url_te);
        ak_net_poll();
        if (macip_state != shown_macip) {
            /* the status line shows the MacIP state while it is down: redraw when it changes */
            shown_macip = macip_state;
            dirty = true;
            if (macip_state == MACIP_UP && home_pending) {
                home_pending = false;
                go(home);
            }
        }
        poll_http();
        if (dirty && TickCount() - last_draw > 30) {
            SetPort(win);
            update_scroll();
            draw_page();
            draw_status();
            last_draw = TickCount();
            dirty = false;
        }
    }
    http_cancel(&req);
    ak_net_stop();
    return 0;
}
