/*
 * http.h - HTTP/1.0 GET for TalkIP Web, polled from the event loop. Direct, or through a proxy (macproxy on
 * the LAN) that fetches https:// pages and simplifies HTML for old browsers; https:// URLs need the proxy.
 * Follows up to 5 redirects. Body bytes go to a callback as they arrive.
 */
#ifndef TALKIP_HTTP_H
#define TALKIP_HTTP_H

#include <MacTypes.h>
#include "url.h"

enum { HTTP_IDLE, HTTP_RESOLVING, HTTP_CONNECTING, HTTP_HEADERS, HTTP_BODY, HTTP_DONE, HTTP_FAILED };

typedef struct HttpReq HttpReq;
typedef void (*HttpStartFn)(HttpReq *r);                    /* headers done, body follows */
typedef void (*HttpBodyFn)(HttpReq *r, const unsigned char *data, unsigned short len);

struct HttpReq {
    short state;
    char url[URL_MAX];                  /* the current URL (after redirects) */
    URLParts u;
    short conn;
    short redirects;
    short status;                       /* HTTP status code */
    char content_type[48];
    char location[URL_MAX];
    char line[300];                     /* header line being collected */
    short line_len;
    unsigned long bytes, started;
    unsigned long heard;                /* TickCount of the last byte (or of the request), for the idle timeout */
    char error[80];
    HttpStartFn on_start;
    HttpBodyFn on_body;
};

void http_set_proxy(const char *host, unsigned short port);    /* host "" = no proxy */
Boolean http_proxy_set(void);
OSErr http_begin(HttpReq *r, const char *url, HttpStartFn on_start, HttpBodyFn on_body);
void http_poll(HttpReq *r);             /* advance; state ends at HTTP_DONE or HTTP_FAILED (r->error) */
void http_cancel(HttpReq *r);

#endif
