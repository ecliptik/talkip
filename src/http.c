#include <string.h>
#include <Events.h>

#include "http.h"
#include "dns.h"
#include "ip.h"
#include "tcp.h"
#include "fmt.h"
#include "macip.h"

#define MAX_REDIRECTS 5
/* Give up when an open connection brings nothing for this long. A gateway that never retransmits (ClassicStack's
 * NAT, M7) leaves a lost segment unrepaired and the connection silent forever; so does a gateway restart. */
#define IDLE_TIMEOUT_S 90

static char proxy_host[64];
static unsigned short proxy_port;
static unsigned char rcv_buf[2048];     /* talkip's window is at most 1092 bytes */

void http_set_proxy(const char *host, unsigned short port)
{
    strncpy(proxy_host, host, sizeof(proxy_host) - 1);
    proxy_host[sizeof(proxy_host) - 1] = '\0';
    proxy_port = port ? port : 5001;
}

Boolean http_proxy_set(void)
{
    return proxy_host[0] != '\0';
}

static void fail(HttpReq *r, const char *why)
{
    if (r->conn >= 0) {
        tcp_abort(r->conn);
        tcp_release(r->conn);
        r->conn = -1;
    }
    if (r->state == HTTP_RESOLVING)
        dns_cancel();
    strncpy(r->error, why, sizeof(r->error) - 1);
    r->error[sizeof(r->error) - 1] = '\0';
    r->state = HTTP_FAILED;
}

/* The proxy's host; "gateway" means the MacIP gateway's own address (macproxy on the gateway host). */
static const char *proxy_addr(void)
{
    return strcmp(proxy_host, "gateway") == 0 ? macip_cfg.gateway_name : proxy_host;
}

static void start(HttpReq *r)
{
    const char *host = http_proxy_set() ? proxy_addr() : r->u.host;
    unsigned long ip = ip_parse(host);

    r->status = 0;
    r->content_type[0] = '\0';
    r->location[0] = '\0';
    r->line_len = 0;
    if (ip) {
        unsigned short port = http_proxy_set() ? proxy_port : r->u.port;
        r->conn = tcp_open(ip, port, rcv_buf, sizeof(rcv_buf));
        if (r->conn < 0)
            fail(r, "No connection available");
        else
            r->state = HTTP_CONNECTING;
    } else if (dns_begin(host) == noErr) {
        r->state = HTTP_RESOLVING;
    } else {
        fail(r, "DNS is busy");
    }
}

OSErr http_begin(HttpReq *r, const char *url, HttpStartFn on_start, HttpBodyFn on_body)
{
    memset(r, 0, sizeof(*r));
    r->conn = -1;
    r->on_start = on_start;
    r->on_body = on_body;
    r->started = TickCount();
    strncpy(r->url, url, sizeof(r->url) - 1);
    if (!url_split(r->url, &r->u)) {
        fail(r, "Only http:// and https:// addresses");
        return paramErr;
    }
    if (r->u.https && !http_proxy_set()) {
        fail(r, "https:// needs the proxy (see TalkIP Web Settings)");
        return paramErr;
    }
    start(r);
    return noErr;
}

static void send_request(HttpReq *r)
{
    char req[600] = "";

    fmt_str(req, "GET ");
    if (http_proxy_set()) {
        /* the proxy fetches https itself; old browsers ask it for http:// */
        fmt_str(req, "http://");
        fmt_str(req, r->u.host);
        if (r->u.port != 80 && r->u.port != 443) {
            fmt_str(req, ":");
            fmt_ul(req, r->u.port);
        }
    }
    fmt_str(req, r->u.path);
    fmt_str(req, " HTTP/1.0\r\nHost: ");
    fmt_str(req, r->u.host);
    fmt_str(req, "\r\nUser-Agent: TalkIP-Web/0.6 (Macintosh 512Ke; LocalTalk)\r\n"
                 "Accept: text/html, text/plain\r\nConnection: close\r\n\r\n");
    tcp_send(r->conn, req, (unsigned short)strlen(req));
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static Boolean header_is(const char *line, const char *name)
{
    while (*name) {
        if (lower(*line++) != *name++)
            return false;
    }
    return *line == ':';
}

static const char *header_value(const char *line)
{
    const char *v = strchr(line, ':');

    if (!v)
        return "";
    v++;
    while (*v == ' ' || *v == '\t')
        v++;
    return v;
}

/* One complete header line (CRLF stripped). Returns true at the blank line that ends the headers. */
static Boolean header_line(HttpReq *r, char *line)
{
    if (r->status == 0) {               /* "HTTP/1.x 200 OK" */
        const char *p = strchr(line, ' ');
        r->status = p ? (short)((p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0')) : 500;
        return false;
    }
    if (line[0] == '\0')
        return true;
    if (header_is(line, "content-type")) {
        strncpy(r->content_type, header_value(line), sizeof(r->content_type) - 1);
    } else if (header_is(line, "location")) {
        strncpy(r->location, header_value(line), sizeof(r->location) - 1);
    }
    return false;
}

static void headers_done(HttpReq *r)
{
    if ((r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308)
        && r->location[0]) {
        char next[URL_MAX];
        if (++r->redirects > MAX_REDIRECTS) {
            fail(r, "Too many redirects");
            return;
        }
        url_resolve(r->url, r->location, next);
        tcp_abort(r->conn);
        tcp_release(r->conn);
        r->conn = -1;
        strcpy(r->url, next);
        if (!url_split(r->url, &r->u)) {
            fail(r, "Redirected to an address TalkIP Web can't open");
            return;
        }
        if (r->u.https && !http_proxy_set()) {
            fail(r, "Redirected to https://, which needs the proxy");
            return;
        }
        start(r);
        return;
    }
    if (r->content_type[0] && strncmp(r->content_type, "text/", 5) != 0) {
        char b[80] = "Can't show ";
        fmt_str(b, r->content_type);
        fail(r, b);
        return;
    }
    r->state = HTTP_BODY;
    if (r->on_start)
        r->on_start(r);
}

void http_poll(HttpReq *r)
{
    unsigned char b[512];
    unsigned short n, i;

    switch (r->state) {
    case HTTP_RESOLVING: {
        DNSResult d;
        if (dns_poll(&d) == DNS_PENDING)
            return;
        if (d.err != noErr || d.naddrs == 0) {
            char e[80] = "";
            fmt_str(e, http_proxy_set() ? proxy_addr() : r->u.host);
            fmt_str(e, ": ");
            fmt_str(e, dns_error_text(d.err));
            fail(r, e);
            return;
        }
        r->conn = tcp_open(d.addr[0], http_proxy_set() ? proxy_port : r->u.port, rcv_buf, sizeof(rcv_buf));
        if (r->conn < 0)
            fail(r, "No connection available");
        else
            r->state = HTTP_CONNECTING;
        return;
    }
    case HTTP_CONNECTING: {
        short st = tcp_state(r->conn);
        if (st == TCP_SYN_SENT)
            return;
        if (st != TCP_ESTABLISHED) {
            fail(r, tcp_error(r->conn) == TCP_ERR_REFUSED ? "Connection refused"
                                                          : "Can't reach the server");
            return;
        }
        send_request(r);
        r->state = HTTP_HEADERS;
        r->heard = TickCount();
        return;
    }
    case HTTP_HEADERS:
    case HTTP_BODY:
        while ((n = tcp_recv(r->conn, b, sizeof(b))) > 0) {
            i = 0;
            r->heard = TickCount();
            if (r->state == HTTP_HEADERS) {
                for (; i < n; i++) {
                    char c = (char)b[i];
                    if (c == '\n') {
                        Boolean end;
                        if (r->line_len && r->line[r->line_len - 1] == '\r')
                            r->line_len--;
                        r->line[r->line_len] = '\0';
                        r->line_len = 0;
                        end = header_line(r, r->line);
                        if (end) {
                            i++;
                            headers_done(r);
                            break;
                        }
                    } else if (r->line_len < (short)sizeof(r->line) - 1) {
                        r->line[r->line_len++] = c;
                    }
                }
                if (r->state != HTTP_BODY)
                    return;             /* still in the headers, or redirected / failed */
            }
            if (i < n) {
                r->bytes += n - i;
                if (r->on_body)
                    r->on_body(r, b + i, (unsigned short)(n - i));
            }
        }
        if (tcp_peer_closed(r->conn) && tcp_unread(r->conn) == 0) {
            tcp_close(r->conn);
            tcp_release(r->conn);
            r->conn = -1;
            if (r->state == HTTP_HEADERS)
                fail(r, "The server closed without an answer");
            else
                r->state = HTTP_DONE;
        } else if (tcp_state(r->conn) == TCP_CLOSED) {
            fail(r, tcp_error(r->conn) == TCP_ERR_RESET ? "Connection reset" : "Connection timed out");
        } else if (TickCount() - r->heard > IDLE_TIMEOUT_S * 60UL) {
            fail(r, "No data for 90 s, gave up");
        }
        return;
    default:
        return;
    }
}

void http_cancel(HttpReq *r)
{
    if (r->state != HTTP_IDLE && r->state != HTTP_DONE && r->state != HTTP_FAILED)
        fail(r, "Stopped");
}
