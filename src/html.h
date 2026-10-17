/*
 * html.h - a streaming HTML renderer for TalkIP Web: feed bytes as they arrive, get a styled text document
 * (bold, links, line breaks, bullets, rules) and its wrapped lines. Only rendered text is kept, never the raw
 * page, so a 100 KB page costs as much as the text it shows (up to DOC_MAX).
 *
 * Document encoding (doc.text): printable Mac Roman, plus control bytes
 *   DOC_BOLD_ON / DOC_BOLD_OFF, DOC_LINK_ON <index byte> / DOC_LINK_OFF, '\n' (line break), DOC_RULE.
 */
#ifndef TALKIP_HTML_H
#define TALKIP_HTML_H

#include <MacTypes.h>

#define DOC_MAX         (32L * 1024L)
#define DOC_MAX_LINES   2500
#define DOC_MAX_LINKS   250
#define DOC_LINK_POOL   (10L * 1024L)

#define DOC_BOLD_ON     0x01
#define DOC_BOLD_OFF    0x02
#define DOC_LINK_ON     0x03
#define DOC_LINK_OFF    0x04
#define DOC_RULE        0x05

typedef struct DocLine {
    unsigned short start;               /* offset in doc.text */
    unsigned short len;
    unsigned char bold;                 /* style at the line's start */
    unsigned char link;                 /* link index + 1 at the line's start, 0 = none */
} DocLine;

typedef struct Doc {
    char *text;                         /* DOC_MAX bytes */
    long len;
    DocLine *lines;                     /* DOC_MAX_LINES */
    short nlines;
    char *link_pool;                    /* DOC_LINK_POOL bytes of NUL-terminated hrefs */
    long pool_len;
    unsigned short link_off[DOC_MAX_LINKS];
    short nlinks;
    char title[64];
    Boolean truncated;                  /* hit DOC_MAX or DOC_MAX_LINES */
} Doc;

/* Allocate the buffers (once). Returns false if memory is short. */
Boolean doc_init(Doc *d);
/* Start a new page. plain = true for text/plain (no HTML parsing, whitespace kept). width = wrap width (px)
 * in the current port's font; the port's font, size and face must be set before feeding. */
void doc_begin(Doc *d, Boolean plain, short width);
void doc_feed(Doc *d, const unsigned char *data, unsigned short len);
void doc_end(Doc *d);                   /* flush and wrap the last line */
const char *doc_link(Doc *d, short index);

#endif
