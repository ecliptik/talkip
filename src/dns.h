/*
 * dns.h - DNS resolver over talkip's UDP, using the nameserver the MacIP gateway hands out.
 * One query at a time, asynchronous: dns_begin(), then dns_poll() from the event loop until it returns
 * DNS_DONE. A lookup of a dotted address is a reverse (PTR) lookup. A small cache keeps recent A answers
 * for their TTL (30 s to 1 h). dns_resolve() is the blocking form for simple callers.
 */
#ifndef TALKIP_DNS_H
#define TALKIP_DNS_H

#include <MacTypes.h>

#define DNS_ERR_TIMEOUT     (-28010)    /* no answer after all tries */
#define DNS_ERR_NXDOMAIN    (-28011)    /* no such name */
#define DNS_ERR_SERVFAIL    (-28012)    /* the server failed or refused */
#define DNS_ERR_FORMAT      (-28013)    /* bad name, or a reply we can't parse */
#define DNS_ERR_NODATA      (-28014)    /* the name exists but has no address (or PTR) */
#define DNS_ERR_NOSERVER    (-28015)    /* no lease, or the gateway gave no nameserver */
#define DNS_ERR_BUSY        (-28016)    /* a query is already running */

#define DNS_MAX_ADDRS   4
#define DNS_NAME_MAX    64

typedef struct DNSResult {
    OSErr err;
    unsigned short naddrs;
    unsigned long addr[DNS_MAX_ADDRS];
    unsigned long ttl;                  /* seconds, of the first address */
    char cname[DNS_NAME_MAX];           /* the canonical name, if a CNAME was followed */
    char ptr[DNS_NAME_MAX];             /* the answer to a reverse lookup */
    unsigned long ticks;                /* time taken */
    short tries;
    Boolean cached;
} DNSResult;

#define DNS_PENDING 1
#define DNS_DONE    0

OSErr dns_begin(const char *name);
short dns_poll(DNSResult *result);      /* DNS_PENDING, or DNS_DONE with *result filled in (result->err) */
void dns_cancel(void);
OSErr dns_resolve(const char *name, unsigned long *ip);    /* blocking: polls the stack until done */
const char *dns_error_text(OSErr err);

#endif
