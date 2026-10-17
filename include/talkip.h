/*
 * talkip.h - for applications ported to talkip through the MacTCP-style shim (Geomys Lite, Flynn Lite,
 * Shiori Lite). Link libtalkip.a instead of your MacTCP wrapper (tcp.c): the library provides
 * _TCPInit, _TCPCreate ... _UDPRelease and ip2long with the same signatures, using MacTCP.h's types.
 *
 * Unlike MacTCP, talkip runs in the application, not in a driver at interrupt time: call talkip_idle()
 * from the event loop (every pass) so received datagrams are processed and timers run between calls.
 * Every shim call does the same, and blocking calls keep doing it while they wait.
 */
#ifndef TALKIP_PUBLIC_H
#define TALKIP_PUBLIC_H

void talkip_idle(void);
unsigned long talkip_local_ip(void);    /* 0 until the MacIP gateway has given us an address */
unsigned long talkip_nameserver(void);  /* the DNS server the gateway handed out, 0 if none */
const char *talkip_status(void);        /* a short line for a status bar: "MacIP up 192.168.72.3", ... */

#endif
