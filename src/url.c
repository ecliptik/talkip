#include <string.h>

#include "url.h"

static Boolean has_scheme(const char *s)
{
    const char *p = s;

    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+'
           || *p == '-' || *p == '.')
        p++;
    return p > s && *p == ':';
}

static void copy(char *out, const char *s, size_t max)
{
    strncpy(out, s, max - 1);
    out[max - 1] = '\0';
}

Boolean url_split(const char *url, URLParts *u)
{
    const char *p = url, *host_end, *colon;
    size_t n;

    memset(u, 0, sizeof(*u));
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        u->https = true;
    } else if (has_scheme(p)) {
        return false;                   /* gopher:, ftp:, mailto: ... */
    }
    host_end = p;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;
    colon = memchr(p, ':', (size_t)(host_end - p));
    n = (size_t)((colon ? colon : host_end) - p);
    if (n == 0 || n >= sizeof(u->host))
        return false;
    memcpy(u->host, p, n);
    u->host[n] = '\0';
    u->port = u->https ? 443 : 80;
    if (colon) {
        unsigned long port = 0;
        const char *q;
        for (q = colon + 1; q < host_end; q++) {
            if (*q < '0' || *q > '9')
                return false;
            port = port * 10 + (unsigned long)(*q - '0');
        }
        if (port == 0 || port > 65535)
            return false;
        u->port = (unsigned short)port;
    }
    if (*host_end != '/')
        strcpy(u->path, "/");
    n = strlen(u->path);
    copy(u->path + n, host_end, sizeof(u->path) - n);
    if ((p = strchr(u->path, '#')) != NULL)
        *(char *)p = '\0';
    return true;
}

/* Remove "./" and "seg/../" from a path that starts with '/'. */
static void normalize(char *path)
{
    char out[URL_MAX];
    char *q = strchr(path, '?');
    char query[URL_MAX] = "";
    const char *s = path;
    size_t o = 0;

    if (q) {
        copy(query, q, sizeof(query));
        *q = '\0';
    }
    while (*s) {
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || s[2] == '\0')) {
            s += 2;                     /* "/./" -> "/" */
            if (!*s) {
                out[o++] = '/';
                break;
            }
            continue;
        }
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || s[3] == '\0')) {
            while (o > 0 && out[o - 1] != '/')
                o--;                    /* drop the last segment */
            if (o > 0)
                o--;
            s += 3;
            if (!*s) {
                out[o++] = '/';
                break;
            }
            continue;
        }
        if (o < sizeof(out) - 1)
            out[o++] = *s;
        s++;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = '\0';
    copy(path, out, URL_MAX);
    if (query[0]) {
        size_t n = strlen(path);
        copy(path + n, query, URL_MAX - n);
    }
}

void url_resolve(const char *base, const char *href, char *out)
{
    URLParts b;
    char path[URL_MAX];
    const char *scheme;
    char *p;

    while (*href == ' ')
        href++;
    if (has_scheme(href)) {
        copy(out, href, URL_MAX);
        if ((p = strchr(out, '#')) != NULL)
            *p = '\0';
        return;
    }
    if (!url_split(base, &b)) {
        copy(out, href, URL_MAX);
        return;
    }
    scheme = b.https ? "https:" : "http:";
    if (href[0] == '/' && href[1] == '/') {
        strcpy(out, scheme);
        copy(out + strlen(out), href, URL_MAX - strlen(out));
        if ((p = strchr(out, '#')) != NULL)
            *p = '\0';
        return;
    }
    if (href[0] == '#' || href[0] == '\0') {
        copy(path, b.path, sizeof(path));
    } else if (href[0] == '/') {
        copy(path, href, sizeof(path));
    } else if (href[0] == '?') {
        copy(path, b.path, sizeof(path));
        if ((p = strchr(path, '?')) != NULL)
            *p = '\0';
        copy(path + strlen(path), href, sizeof(path) - strlen(path));
    } else {
        copy(path, b.path, sizeof(path));
        if ((p = strchr(path, '?')) != NULL)
            *p = '\0';
        p = strrchr(path, '/');
        p[1] = '\0';                    /* the directory of the base */
        copy(path + strlen(path), href, sizeof(path) - strlen(path));
    }
    if ((p = strchr(path, '#')) != NULL)
        *p = '\0';
    normalize(path);
    out[0] = '\0';
    strcpy(out, scheme);
    strcat(out, "//");
    strcat(out, b.host);
    if (b.port != (b.https ? 443 : 80)) {
        char port[8];
        unsigned short v = b.port;
        int i = 7;
        port[i] = '\0';
        do {
            port[--i] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        strcat(out, ":");
        strcat(out, port + i);
    }
    copy(out + strlen(out), path, URL_MAX - strlen(out));
}
