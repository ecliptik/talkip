/*
 * dns.c - a small stub resolver (RFC 1035): recursion-desired queries to the gateway-supplied nameserver,
 * A and PTR, CNAME chains as the server returns them, name compression in replies, 3 tries 2 s apart.
 */
#include <string.h>
#include <Events.h>
#include <OSUtils.h>

#include "dns.h"
#include "udp.h"
#include "ip.h"
#include "macip.h"
#include "bytes.h"

#define DNS_PORT        53
#define TYPE_A          1
#define TYPE_CNAME      5
#define TYPE_PTR        12
#define CLASS_IN        1
#define TRY_TICKS       (2 * 60)
#define TRIES           3
#define CACHE_SLOTS     4
#define CACHE_MIN_S     30
#define CACHE_MAX_S     3600

static struct {
    Boolean active, done;
    unsigned short id, port, qtype;
    unsigned char query[300];
    unsigned short qlen;
    unsigned long started, sent_at;
    char name[DNS_NAME_MAX];
    DNSResult r;
} q;

static struct {
    char name[DNS_NAME_MAX];
    unsigned long addr;
    unsigned long expires;      /* TickCount */
} cache[CACHE_SLOTS];

const char *dns_error_text(OSErr err)
{
    switch (err) {
    case noErr: return "OK";
    case DNS_ERR_TIMEOUT: return "no answer from the DNS server";
    case DNS_ERR_NXDOMAIN: return "no such name";
    case DNS_ERR_SERVFAIL: return "the DNS server failed";
    case DNS_ERR_FORMAT: return "bad name or reply";
    case DNS_ERR_NODATA: return "no address for that name";
    case DNS_ERR_NOSERVER: return "no DNS server (no lease yet?)";
    case DNS_ERR_BUSY: return "another lookup is running";
    default: return "network error";
    }
}

static Boolean name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return false;
    }
    return *a == *b;
}

static Boolean cache_get(const char *name, DNSResult *r)
{
    int i;

    for (i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].name[0] && name_eq(cache[i].name, name) && TickCount() < cache[i].expires) {
            memset(r, 0, sizeof(*r));
            r->naddrs = 1;
            r->addr[0] = cache[i].addr;
            r->ttl = (cache[i].expires - TickCount()) / 60;
            r->cached = true;
            return true;
        }
    }
    return false;
}

static void cache_put(const char *name, unsigned long addr, unsigned long ttl)
{
    int i, slot = 0;
    unsigned long oldest = 0xFFFFFFFFUL;

    if (ttl < CACHE_MIN_S)
        ttl = CACHE_MIN_S;
    if (ttl > CACHE_MAX_S)
        ttl = CACHE_MAX_S;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].name[0] == '\0' || name_eq(cache[i].name, name)) {
            slot = i;
            break;
        }
        if (cache[i].expires < oldest) {
            oldest = cache[i].expires;
            slot = i;
        }
    }
    strncpy(cache[slot].name, name, DNS_NAME_MAX - 1);
    cache[slot].name[DNS_NAME_MAX - 1] = '\0';
    cache[slot].addr = addr;
    cache[slot].expires = TickCount() + ttl * 60;
}

/* "www.example.com" -> 3www7example3com0 (a trailing dot is allowed). Returns bytes written, 0 if invalid. */
static unsigned short encode_name(unsigned char *out, const char *name)
{
    unsigned char *p = out;
    const char *s = name;
    size_t total = strlen(name);

    if (total == 0 || total > 253)
        return 0;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t n = dot ? (size_t)(dot - s) : strlen(s);

        if (n == 0 || n > 63)           /* an empty label ("a..b", ".a") or a long one */
            return 0;
        *p++ = (unsigned char)n;
        memcpy(p, s, n);
        p += n;
        s += n;
        if (*s == '.')
            s++;
    }
    *p++ = 0;
    return (unsigned short)(p - out);
}

/* Decode a possibly compressed name at off into out (dotted, truncated to outlen-1; out may be NULL to skip).
 * Returns the offset just past the name where it started, or -1 if malformed. */
static int decode_name(const unsigned char *m, unsigned short mlen, unsigned short off, char *out, unsigned short outlen)
{
    int end = -1, jumps = 0;
    unsigned short o = off, w = 0;

    if (out && outlen)
        out[0] = '\0';
    for (;;) {
        unsigned char len;

        if (o >= mlen)
            return -1;
        len = m[o];
        if ((len & 0xC0) == 0xC0) {     /* compression pointer */
            if (o + 1 >= mlen || ++jumps > 16)
                return -1;
            if (end < 0)
                end = o + 2;
            o = (unsigned short)(((len & 0x3F) << 8) | m[o + 1]);
            continue;
        }
        if (len & 0xC0)
            return -1;
        o++;
        if (len == 0)
            break;
        if (o + len > mlen)
            return -1;
        if (out) {
            unsigned char k;
            if (w && w < outlen - 1)
                out[w++] = '.';
            for (k = 0; k < len; k++)
                if (w < outlen - 1)
                    out[w++] = (char)m[o + k];
            out[w] = '\0';
        }
        o += len;
    }
    return end < 0 ? o : end;
}

static void reply(unsigned long src, unsigned short sport, unsigned short dport, const unsigned char *m, unsigned short len)
{
    DNSResult *r = &q.r;
    unsigned short flags, qd, an, i;
    int off;

    (void)dport;
    if (!q.active || q.done || src != macip_cfg.nameserver || sport != DNS_PORT || len < 12 || get16(m) != q.id)
        return;
    flags = get16(m + 2);
    if (!(flags & 0x8000))
        return;
    q.done = true;
    r->ticks = TickCount() - q.started;
    switch (flags & 0x000F) {
    case 0: break;
    case 3: r->err = DNS_ERR_NXDOMAIN; return;
    case 2: case 5: r->err = DNS_ERR_SERVFAIL; return;
    default: r->err = DNS_ERR_FORMAT; return;
    }
    qd = get16(m + 4);
    an = get16(m + 6);
    off = 12;
    for (i = 0; i < qd; i++) {
        off = decode_name(m, len, off, NULL, 0);
        if (off < 0 || off + 4 > len) {
            r->err = DNS_ERR_FORMAT;
            return;
        }
        off += 4;
    }
    for (i = 0; i < an; i++) {
        unsigned short type, cls, rdlen;
        unsigned long ttl;

        off = decode_name(m, len, off, NULL, 0);
        if (off < 0 || off + 10 > len) {
            r->err = DNS_ERR_FORMAT;
            return;
        }
        type = get16(m + off);
        cls = get16(m + off + 2);
        ttl = get32(m + off + 4);
        rdlen = get16(m + off + 8);
        off += 10;
        if (off + rdlen > len) {
            r->err = DNS_ERR_FORMAT;
            return;
        }
        if (cls == CLASS_IN) {
            if (type == TYPE_A && rdlen == 4 && r->naddrs < DNS_MAX_ADDRS) {
                if (r->naddrs == 0)
                    r->ttl = ttl;
                r->addr[r->naddrs++] = get32(m + off);
            } else if (type == TYPE_CNAME && r->cname[0] == '\0') {
                decode_name(m, len, off, r->cname, sizeof(r->cname));
            } else if (type == TYPE_PTR && r->ptr[0] == '\0') {
                decode_name(m, len, off, r->ptr, sizeof(r->ptr));
                r->ttl = ttl;
            }
        }
        off += rdlen;
    }
    if (q.qtype == TYPE_A && r->naddrs == 0)
        r->err = DNS_ERR_NODATA;
    else if (q.qtype == TYPE_PTR && r->ptr[0] == '\0')
        r->err = DNS_ERR_NODATA;
    else if (q.qtype == TYPE_A)
        cache_put(q.name, r->addr[0], r->ttl);
}

static OSErr send_query(void)
{
    q.sent_at = TickCount();
    q.r.tries++;
    return udp_send(macip_cfg.nameserver, q.port, DNS_PORT, q.query, q.qlen);
}

OSErr dns_begin(const char *name)
{
    unsigned long addr;
    unsigned char *p;
    unsigned short n;
    char ptrname[32];
    OSErr err;

    if (q.active && !q.done)
        return DNS_ERR_BUSY;
    dns_cancel();
    memset(&q.r, 0, sizeof(q.r));
    strncpy(q.name, name, DNS_NAME_MAX - 1);
    q.name[DNS_NAME_MAX - 1] = '\0';
    q.started = TickCount();
    q.active = true;
    q.done = false;

    addr = ip_parse(name);
    if (addr == 0 && cache_get(name, &q.r)) {
        q.done = true;
        return noErr;
    }
    if (macip_state != MACIP_UP || macip_cfg.nameserver == 0) {
        q.r.err = DNS_ERR_NOSERVER;
        q.done = true;
        return noErr;
    }
    q.qtype = addr ? TYPE_PTR : TYPE_A;
    if (addr) {     /* d.c.b.a.in-addr.arpa */
        char *s = ptrname;
        int i;
        ptrname[0] = '\0';
        for (i = 0; i < 4; i++) {
            unsigned long octet = (addr >> (8 * i)) & 0xFF;
            char d[4];
            int k = 0;
            do { d[k++] = '0' + (char)(octet % 10); octet /= 10; } while (octet);
            while (k)
                *s++ = d[--k];
            *s++ = '.';
        }
        strcpy(s, "in-addr.arpa");
        name = ptrname;
    }

    q.id = (unsigned short)(TickCount() * 2654435761UL >> 16);
    p = q.query;
    put16(p, q.id);
    put16(p + 2, 0x0100);           /* standard query, recursion desired */
    put16(p + 4, 1);                /* one question */
    put16(p + 6, 0);
    put16(p + 8, 0);
    put16(p + 10, 0);
    n = encode_name(p + 12, name);
    if (n == 0) {
        q.r.err = DNS_ERR_FORMAT;
        q.done = true;
        return noErr;
    }
    put16(p + 12 + n, q.qtype);
    put16(p + 14 + n, CLASS_IN);
    q.qlen = 16 + n;

    q.port = udp_ephemeral_port();
    err = udp_bind(q.port, reply);
    if (err != noErr) {
        q.r.err = err;
        q.done = true;
        return noErr;
    }
    err = send_query();
    if (err != noErr) {
        q.r.err = err;
        q.done = true;
    }
    return noErr;
}

short dns_poll(DNSResult *result)
{
    if (!q.active)
        return DNS_DONE;
    if (!q.done && TickCount() - q.sent_at > TRY_TICKS) {
        if (q.r.tries >= TRIES) {
            q.r.err = DNS_ERR_TIMEOUT;
            q.r.ticks = TickCount() - q.started;
            q.done = true;
        } else {
            send_query();
        }
    }
    if (!q.done)
        return DNS_PENDING;
    if (result)
        *result = q.r;
    dns_cancel();
    return DNS_DONE;
}

void dns_cancel(void)
{
    if (q.port)
        udp_unbind(q.port);
    q.port = 0;
    q.active = false;
}

OSErr dns_resolve(const char *name, unsigned long *ip)
{
    DNSResult r;
    OSErr err = dns_begin(name);

    if (err != noErr)
        return err;
    while (dns_poll(&r) == DNS_PENDING) {
        macip_poll();
        SystemTask();
    }
    if (r.err == noErr)
        *ip = r.naddrs ? r.addr[0] : 0;
    return r.err;
}
