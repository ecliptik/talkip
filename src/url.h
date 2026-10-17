/* url.h - just enough URL handling for TalkIP Web: split http(s) URLs, resolve links against a page. */
#ifndef TALKIP_URL_H
#define TALKIP_URL_H

#include <MacTypes.h>

#define URL_MAX 256

typedef struct URLParts {
    Boolean https;
    char host[64];
    unsigned short port;
    char path[URL_MAX];                 /* starts with '/', includes the query, no fragment */
} URLParts;

/* "http://host[:port]/path?q#frag" (scheme optional: "host/path" means http). False if unusable. */
Boolean url_split(const char *url, URLParts *u);
/* href relative to base -> absolute URL in out (URL_MAX). */
void url_resolve(const char *base, const char *href, char *out);

#endif
