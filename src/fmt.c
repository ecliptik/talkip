#include <string.h>

#include "fmt.h"

char *fmt_str(char *buf, const char *s)
{
    return strcat(buf, s);
}

char *fmt_ul(char *buf, unsigned long v)
{
    char tmp[12];
    int i = sizeof(tmp) - 1;

    tmp[i] = '\0';
    do {
        tmp[--i] = '0' + (char)(v % 10);
        v /= 10;
    } while (v && i > 0);
    return strcat(buf, tmp + i);
}

char *fmt_ip(char *buf, unsigned long ip)
{
    int shift;

    for (shift = 24; shift >= 0; shift -= 8) {
        fmt_ul(buf, (ip >> shift) & 0xFF);
        if (shift)
            strcat(buf, ".");
    }
    return buf;
}

char *fmt_err(char *buf, short err)
{
    if (err < 0) {
        strcat(buf, "-");
        return fmt_ul(buf, (unsigned long)(-(long)err));
    }
    return fmt_ul(buf, (unsigned long)err);
}

unsigned char *fmt_pstr(unsigned char *p, const char *s)
{
    size_t n = strlen(s);

    if (n > 255)
        n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    return p;
}
