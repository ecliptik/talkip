/*
 * html.c - streaming HTML -> styled text for TalkIP Web (see html.h).
 *
 * Deliberately small: block tags become line breaks, b/strong/h1-6 bold, a href links, li bullets, hr a rule,
 * pre keeps whitespace, img its alt text, script/style are skipped, entities and UTF-8 (or stray Latin-1) are
 * mapped to Mac Roman where a glyph exists and '?' where not. Everything else just contributes its text.
 * Lines are wrapped as text arrives, measured with the current port's font (CharWidth), so the page can be
 * drawn while it loads.
 */
#include <string.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <Fonts.h>

#include "html.h"

enum { S_TEXT, S_TAG, S_ENTITY, S_COMMENT, S_SKIP };

static struct {
    Doc *d;
    Boolean plain, pre, in_title, space_pending, at_line_start, blank_line;
    short state;
    char tag[200];
    short tag_len;
    char ent[12];
    short ent_len;
    char skip_name[8];
    short skip_match;                   /* chars of "</name" matched so far */
    short comment_dashes;
    short bold_depth;
    short link;                         /* open link index + 1, 0 = none */
    unsigned long cp;                   /* UTF-8 decoding */
    short need;
    /* wrapping */
    short width;
    long ls, pos, last_space;           /* line start, next char to measure, last break point */
    short x, x_at_space;
    Boolean bold_at_ls, bold_now, bold_at_space;
    unsigned char link_at_ls, link_now, link_at_space;
} P;

Boolean doc_init(Doc *d)
{
    memset(d, 0, sizeof(*d));
    d->text = NewPtr(DOC_MAX);
    d->lines = (DocLine *)NewPtr((long)DOC_MAX_LINES * sizeof(DocLine));
    d->link_pool = NewPtr(DOC_LINK_POOL);
    return d->text && d->lines && d->link_pool;
}

const char *doc_link(Doc *d, short index)
{
    if (index < 0 || index >= d->nlinks)
        return "";
    return d->link_pool + d->link_off[index];
}

/* ---- wrapping ---- */

static void add_line(long start, long end, Boolean bold, unsigned char link)
{
    Doc *d = P.d;

    if (d->nlines >= DOC_MAX_LINES) {
        d->truncated = true;
        return;
    }
    d->lines[d->nlines].start = (unsigned short)start;
    d->lines[d->nlines].len = (unsigned short)(end - start);
    d->lines[d->nlines].bold = bold;
    d->lines[d->nlines].link = link;
    d->nlines++;
}

static void wrap(void)
{
    Doc *d = P.d;

    while (P.pos < d->len) {
        unsigned char c = (unsigned char)d->text[P.pos];

        if (c == DOC_BOLD_ON || c == DOC_BOLD_OFF) {
            P.bold_now = (c == DOC_BOLD_ON);
            TextFace(P.bold_now ? bold : 0);
            P.pos++;
            continue;
        }
        if (c == DOC_LINK_ON) {
            if (P.pos + 1 >= d->len)
                return;                 /* index byte not there yet */
            P.link_now = (unsigned char)(d->text[P.pos + 1] + 1);
            P.pos += 2;
            continue;
        }
        if (c == DOC_LINK_OFF) {
            P.link_now = 0;
            P.pos++;
            continue;
        }
        if (c == '\n' || c == DOC_RULE) {
            add_line(P.ls, P.pos + (c == DOC_RULE ? 1 : 0), P.bold_at_ls, P.link_at_ls);
            P.pos++;
            P.ls = P.pos;
            P.x = 0;
            P.last_space = -1;
            P.bold_at_ls = P.bold_now;
            P.link_at_ls = P.link_now;
            continue;
        }
        if (c == ' ') {
            P.last_space = P.pos;
            P.x_at_space = P.x;
            P.bold_at_space = P.bold_now;
            P.link_at_space = P.link_now;
        }
        P.x += CharWidth(c);
        if (P.x > P.width && P.pos > P.ls) {
            if (P.last_space > P.ls) {  /* break at the last space; the space itself is dropped */
                add_line(P.ls, P.last_space, P.bold_at_ls, P.link_at_ls);
                P.ls = P.last_space + 1;
                P.x -= P.x_at_space + CharWidth(' ');
                P.bold_at_ls = P.bold_at_space;
                P.link_at_ls = P.link_at_space;
            } else {                    /* one long word: break it here */
                add_line(P.ls, P.pos, P.bold_at_ls, P.link_at_ls);
                P.ls = P.pos;
                P.x = CharWidth(c);
                P.bold_at_ls = P.bold_now;
                P.link_at_ls = P.link_now;
            }
            P.last_space = -1;
        }
        P.pos++;
    }
}

/* ---- emitting ---- */

static void put(unsigned char c)
{
    Doc *d = P.d;

    if (d->len >= DOC_MAX - 4 || d->nlines >= DOC_MAX_LINES) {
        d->truncated = true;
        return;
    }
    d->text[d->len++] = (char)c;
}

static void newline(void)
{
    put('\n');
    P.blank_line = P.at_line_start;
    P.at_line_start = true;
    P.space_pending = false;
}

static void block_break(void)
{
    if (!P.at_line_start)
        newline();
}

static void para_break(void)
{
    block_break();
    if (!P.blank_line && P.d->len > 0)
        newline();
}

static void text_char(unsigned char c)
{
    if (P.in_title) {
        size_t n = strlen(P.d->title);
        if (n < sizeof(P.d->title) - 1 && !(c == ' ' && n == 0)) {
            P.d->title[n] = (char)c;
            P.d->title[n + 1] = '\0';
        }
        return;
    }
    if (P.pre || P.plain) {
        if (c == '\n')
            newline();
        else if (c == '\t') {
            put(' ');
            put(' ');
            P.at_line_start = false;
        } else if (c != '\r') {
            put(c);
            P.at_line_start = false;
        }
        return;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        if (!P.at_line_start)
            P.space_pending = true;
        return;
    }
    if (P.space_pending) {
        put(' ');
        P.space_pending = false;
    }
    put(c);
    P.at_line_start = false;
    P.blank_line = false;
}

/* Unicode code point -> Mac Roman (common Latin-1 and punctuation); '?' when there's no glyph. */
static unsigned char mac_roman(unsigned long u)
{
    static const struct { unsigned short u; unsigned char m; } map[] = {
        {0xA0, ' '}, {0xA1, 0xC1}, {0xA3, 0xA3}, {0xA7, 0xA4}, {0xA9, 0xA9}, {0xAB, 0xC7}, {0xAE, 0xA8},
        {0xB0, 0xA1}, {0xB1, 0xB1}, {0xB6, 0xA6}, {0xB7, 0xE1}, {0xBB, 0xC8}, {0xBF, 0xC0}, {0xC4, 0x80},
        {0xC5, 0x81}, {0xC7, 0x82}, {0xC9, 0x83}, {0xD1, 0x84}, {0xD6, 0x85}, {0xDC, 0x86}, {0xDF, 0xA7},
        {0xE0, 0x88}, {0xE1, 0x87}, {0xE2, 0x89}, {0xE4, 0x8A}, {0xE5, 0x8C}, {0xE7, 0x8D}, {0xE8, 0x8F},
        {0xE9, 0x8E}, {0xEA, 0x90}, {0xEB, 0x91}, {0xED, 0x92}, {0xEE, 0x94}, {0xEF, 0x95}, {0xF1, 0x96},
        {0xF3, 0x97}, {0xF4, 0x99}, {0xF6, 0x9A}, {0xF7, 0xD6}, {0xFA, 0x9C}, {0xFB, 0x9E}, {0xFC, 0x9F},
        {0x2013, 0xD0}, {0x2014, 0xD1}, {0x2018, 0xD4}, {0x2019, 0xD5}, {0x201C, 0xD2}, {0x201D, 0xD3},
        {0x2022, 0xA5}, {0x2026, 0xC9}, {0x2122, 0xAA}, {0x2032, '\''}, {0x2033, '"'}, {0x2212, '-'},
    };
    unsigned short i;

    if (u < 0x80)
        return (unsigned char)u;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].u == u)
            return map[i].m;
    return '?';
}

/* One byte of page text (outside tags): UTF-8 decoding, falling back to Latin-1 for stray high bytes. */
static void text_byte(unsigned char b)
{
    if (P.need) {
        if ((b & 0xC0) == 0x80) {
            P.cp = (P.cp << 6) | (b & 0x3F);
            if (--P.need == 0)
                text_char(mac_roman(P.cp));
            return;
        }
        P.need = 0;
        text_char('?');                 /* broken sequence */
    }
    if (b < 0x80)
        text_char(b);
    else if ((b & 0xE0) == 0xC0) {
        P.cp = b & 0x1F;
        P.need = 1;
    } else if ((b & 0xF0) == 0xE0) {
        P.cp = b & 0x0F;
        P.need = 2;
    } else if ((b & 0xF8) == 0xF0) {
        P.cp = b & 0x07;
        P.need = 3;
    } else
        text_char(mac_roman(b));        /* Latin-1 byte */
}

/* ---- tags ---- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Copy attribute name's value (e.g. "href") out of the tag text into out (outlen). */
static Boolean attr(const char *tag, const char *name, char *out, short outlen)
{
    short n = (short)strlen(name), i, j;

    for (i = 0; tag[i]; i++) {
        if ((i == 0 || tag[i - 1] == ' ' || tag[i - 1] == '\t' || tag[i - 1] == '\n')) {
            for (j = 0; j < n && lower(tag[i + j]) == name[j]; j++)
                ;
            if (j == n) {
                const char *p = tag + i + n;
                char q = 0;
                short k = 0;
                while (*p == ' ')
                    p++;
                if (*p != '=')
                    continue;
                p++;
                while (*p == ' ')
                    p++;
                if (*p == '"' || *p == '\'')
                    q = *p++;
                while (*p && k < outlen - 1 && (q ? *p != q : (*p != ' ' && *p != '>'))) {
                    if (*p == '&' && strncmp(p, "&amp;", 5) == 0) {   /* the one entity hrefs carry */
                        out[k++] = '&';
                        p += 5;
                    } else {
                        out[k++] = *p++;
                    }
                }
                out[k] = '\0';
                return true;
            }
        }
    }
    return false;
}

static Boolean is(const char *name, const char *a)
{
    return strcmp(name, a) == 0;
}

static void link_open(const char *href)
{
    Doc *d = P.d;
    long n = (long)strlen(href);

    if (P.link)
        put(DOC_LINK_OFF);
    P.link = 0;
    if (d->nlinks >= DOC_MAX_LINKS || d->pool_len + n + 1 > DOC_LINK_POOL)
        return;
    memcpy(d->link_pool + d->pool_len, href, n + 1);
    d->link_off[d->nlinks] = (unsigned short)d->pool_len;
    d->pool_len += n + 1;
    if (P.space_pending) {
        put(' ');
        P.space_pending = false;
    }
    put(DOC_LINK_ON);
    put((unsigned char)d->nlinks);
    P.link = d->nlinks + 1;
    d->nlinks++;
}

static void handle_tag(void)
{
    char name[12];
    char *t = P.tag;
    Boolean closing = false;
    short i = 0;

    P.tag[P.tag_len] = '\0';
    if (*t == '/') {
        closing = true;
        t++;
    }
    while (*t && *t != ' ' && *t != '/' && *t != '\t' && *t != '\n' && i < (short)sizeof(name) - 1)
        name[i++] = lower(*t++);
    name[i] = '\0';

    if (!closing && (is(name, "script") || is(name, "style"))) {
        strcpy(P.skip_name, name);
        P.skip_match = 0;
        P.state = S_SKIP;
        return;
    }
    if (is(name, "br")) {
        newline();
    } else if (is(name, "p") || is(name, "blockquote")) {
        para_break();
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
        if (closing) {
            put(DOC_BOLD_OFF);
            para_break();
        } else {
            para_break();
            put(DOC_BOLD_ON);
        }
    } else if (is(name, "b") || is(name, "strong")) {
        if (closing) {
            if (P.bold_depth > 0 && --P.bold_depth == 0)
                put(DOC_BOLD_OFF);
        } else if (P.bold_depth++ == 0)
            put(DOC_BOLD_ON);
    } else if (is(name, "a")) {
        if (closing) {
            if (P.link) {
                put(DOC_LINK_OFF);
                P.link = 0;
            }
        } else {
            char href[200];
            if (attr(t, "href", href, sizeof(href)) && href[0] && strncmp(href, "javascript:", 11) != 0)
                link_open(href);
        }
    } else if (is(name, "li") || is(name, "dt")) {
        if (!closing) {
            block_break();
            put(0xA5);                  /* bullet */
            put(' ');
            P.at_line_start = false;
        }
    } else if (is(name, "dd")) {
        if (!closing) {
            block_break();
            put(' ');
            put(' ');
            P.at_line_start = false;
        }
    } else if (is(name, "hr")) {
        block_break();
        put(DOC_RULE);
        P.at_line_start = true;
    } else if (is(name, "pre")) {
        block_break();
        P.pre = !closing;
    } else if (is(name, "title")) {
        P.in_title = !closing;
    } else if (is(name, "img") && !closing) {
        char alt[60];
        if (attr(t, "alt", alt, sizeof(alt)) && alt[0]) {
            const char *a;
            text_char('[');
            for (a = alt; *a; a++)
                text_char((unsigned char)*a);
            text_char(']');
        }
    } else if (is(name, "td") || is(name, "th")) {
        if (!closing && !P.at_line_start)
            P.space_pending = true;
    } else if (is(name, "div") || is(name, "tr") || is(name, "table") || is(name, "ul") || is(name, "ol")
               || is(name, "dl") || is(name, "center") || is(name, "section") || is(name, "article")
               || is(name, "header") || is(name, "footer") || is(name, "nav") || is(name, "main")
               || is(name, "form") || is(name, "address") || is(name, "figure")) {
        block_break();
    }
}

static void decode_entity(void)
{
    char *e = P.ent;
    unsigned long u = 0;

    P.ent[P.ent_len] = '\0';
    if (e[0] == '#') {
        if (e[1] == 'x' || e[1] == 'X') {
            for (e += 2; *e; e++)
                u = u * 16 + (unsigned long)(*e <= '9' ? *e - '0' : (lower(*e) - 'a' + 10));
        } else {
            for (e++; *e; e++)
                u = u * 10 + (unsigned long)(*e - '0');
        }
    } else if (is(e, "amp")) u = '&';
    else if (is(e, "lt")) u = '<';
    else if (is(e, "gt")) u = '>';
    else if (is(e, "quot")) u = '"';
    else if (is(e, "apos")) u = '\'';
    else if (is(e, "nbsp")) u = 0xA0;
    else if (is(e, "copy")) u = 0xA9;
    else if (is(e, "reg")) u = 0xAE;
    else if (is(e, "mdash")) u = 0x2014;
    else if (is(e, "ndash")) u = 0x2013;
    else if (is(e, "hellip")) u = 0x2026;
    else if (is(e, "lsquo")) u = 0x2018;
    else if (is(e, "rsquo")) u = 0x2019;
    else if (is(e, "ldquo")) u = 0x201C;
    else if (is(e, "rdquo")) u = 0x201D;
    else if (is(e, "bull")) u = 0x2022;
    else if (is(e, "middot")) u = 0xB7;
    else if (is(e, "eacute")) u = 0xE9;
    else if (is(e, "trade")) u = 0x2122;
    else {
        const char *p;
        text_char('&');                 /* unknown: show it as written */
        for (p = P.ent; *p; p++)
            text_char((unsigned char)*p);
        text_char(';');
        return;
    }
    text_char(u == 0xA0 ? ' ' : mac_roman(u));
}

/* ---- public ---- */

void doc_begin(Doc *d, Boolean plain, short width)
{
    memset(&P, 0, sizeof(P));
    P.d = d;
    P.plain = plain;
    P.width = width;
    P.at_line_start = true;
    P.blank_line = true;
    P.last_space = -1;
    d->len = 0;
    d->nlines = 0;
    d->nlinks = 0;
    d->pool_len = 0;
    d->title[0] = '\0';
    d->truncated = false;
    TextFace(0);
}

void doc_feed(Doc *d, const unsigned char *data, unsigned short len)
{
    unsigned short i;

    (void)d;
    for (i = 0; i < len; i++) {
        unsigned char b = data[i];

        switch (P.state) {
        case S_TEXT:
            if (P.plain)
                text_byte(b);
            else if (b == '<') {
                P.state = S_TAG;
                P.tag_len = 0;
            } else if (b == '&') {
                P.state = S_ENTITY;
                P.ent_len = 0;
            } else
                text_byte(b);
            break;
        case S_TAG:
            if (b == '>') {
                P.state = S_TEXT;
                handle_tag();
            } else if (P.tag_len == 2 && P.tag[0] == '!' && P.tag[1] == '-' && b == '-') {
                P.state = S_COMMENT;
                P.comment_dashes = 0;
            } else if (P.tag_len < (short)sizeof(P.tag) - 1) {
                P.tag[P.tag_len++] = (char)b;
            }
            break;
        case S_ENTITY:
            if (b == ';') {
                P.state = S_TEXT;
                decode_entity();
            } else if (((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '#')
                       && P.ent_len < (short)sizeof(P.ent) - 1) {
                P.ent[P.ent_len++] = (char)b;
            } else {                    /* not an entity after all */
                short k;
                P.state = S_TEXT;
                text_char('&');
                for (k = 0; k < P.ent_len; k++)
                    text_char((unsigned char)P.ent[k]);
                i--;                    /* reprocess this byte as text */
            }
            break;
        case S_COMMENT:
            if (b == '-')
                P.comment_dashes++;
            else if (b == '>' && P.comment_dashes >= 2)
                P.state = S_TEXT;
            else
                P.comment_dashes = 0;
            break;
        case S_SKIP: {                  /* inside script/style: wait for </name */
            static const char lead[] = "</";
            short m = P.skip_match;
            char want = m < 2 ? lead[m] : P.skip_name[m - 2];
            if (want == '\0') {         /* matched "</name": the rest is the tag */
                P.state = S_TAG;
                P.tag[0] = '/';
                strcpy(P.tag + 1, P.skip_name);
                P.tag_len = (short)(1 + strlen(P.skip_name));
                i--;
            } else if (lower((char)b) == want) {
                P.skip_match++;
            } else {
                P.skip_match = (b == '<') ? 1 : 0;
            }
            break;
        }
        }
    }
    wrap();
}

void doc_end(Doc *d)
{
    if (P.link)
        put(DOC_LINK_OFF);
    if (P.bold_depth)
        put(DOC_BOLD_OFF);
    wrap();
    if (P.ls < d->len)
        add_line(P.ls, d->len, P.bold_at_ls, P.link_at_ls);
    TextFace(0);
}
