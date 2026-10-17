/* Host test for url.c: url_split and url_resolve against expected results. */
#include <stdio.h>
#include <string.h>
#include "url.h"

static int fails;

static void r(const char *base, const char *href, const char *want)
{
    char out[URL_MAX];
    url_resolve(base, href, out);
    if (strcmp(out, want) != 0) {
        printf("FAIL resolve(%s, %s) = %s, want %s\n", base, href, out, want);
        fails++;
    }
}

static void s(const char *url, int ok, const char *host, unsigned short port, const char *path, int https)
{
    URLParts u;
    int got = url_split(url, &u);
    if (got != ok || (ok && (strcmp(u.host, host) || u.port != port || strcmp(u.path, path) || u.https != https))) {
        printf("FAIL split(%s) = %d %s %u %s %d\n", url, got, u.host, u.port, u.path, u.https);
        fails++;
    }
}

int main(void)
{
    s("http://example.com", 1, "example.com", 80, "/", 0);
    s("http://example.com:8080/a/b?q=1#frag", 1, "example.com", 8080, "/a/b?q=1", 0);
    s("https://en.wikipedia.org/wiki/Mac", 1, "en.wikipedia.org", 443, "/wiki/Mac", 1);
    s("info.cern.ch/hypertext", 1, "info.cern.ch", 80, "/hypertext", 0);
    s("gopher://floodgap.com/", 0, "", 0, "", 0);
    s("http://:80/", 0, "", 0, "", 0);
    s("http://h:99999/", 0, "", 0, "", 0);
    r("http://a.com/d1/d2/page.html", "other.html", "http://a.com/d1/d2/other.html");
    r("http://a.com/d1/d2/page.html", "../up.html", "http://a.com/d1/up.html");
    r("http://a.com/d1/d2/page.html", "../../../x", "http://a.com/x");
    r("http://a.com/d1/d2/page.html", "./same", "http://a.com/d1/d2/same");
    r("http://a.com/d1/page?x=1", "?y=2", "http://a.com/d1/page?y=2");
    r("http://a.com/d1/page", "/root?q=a/../b", "http://a.com/root?q=a/../b");
    r("http://a.com/d1/page", "//cdn.b.org/f.png", "http://cdn.b.org/f.png");
    r("https://a.com/d1/page", "//cdn.b.org/f.png", "https://cdn.b.org/f.png");
    r("http://a.com:8080/x/y", "z", "http://a.com:8080/x/z");
    r("http://a.com/x/y", "http://other.org/p#top", "http://other.org/p");
    r("http://a.com/x/y", "#section", "http://a.com/x/y");
    r("http://a.com/x/", "mailto:me@x.org", "mailto:me@x.org");
    r("http://a.com", "page", "http://a.com/page");
    printf(fails ? "%d FAILURES\n" : "ok\n", fails);
    return fails != 0;
}
