/*
 * fmt.h - tiny string building without printf (keeps newlib's stdio out of 512 KB apps).
 * Each appends to a NUL-terminated buffer and returns it.
 */
#ifndef TALKIP_FMT_H
#define TALKIP_FMT_H

char *fmt_str(char *buf, const char *s);
char *fmt_ul(char *buf, unsigned long v);
char *fmt_ip(char *buf, unsigned long ip);
char *fmt_err(char *buf, short err);        /* signed decimal */
unsigned char *fmt_pstr(unsigned char *p, const char *s);   /* C string -> Pascal string (max 255) */

#endif
