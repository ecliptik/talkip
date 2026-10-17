/*
 * Host test: talkip's real tcp.c/ip.c/udp.c against the Linux kernel's TCP, through a TUN device.
 * Run inside an unprivileged network namespace (no root needed):
 *     make -C tests/host && tests/host/run_tcp_test.sh [loss-percent]
 * The namespace has tun9: the kernel is 10.9.0.1, talkip is 10.9.0.2, MTU 586 like MacIP. The kernel-side
 * servers are plain non-blocking sockets in this same loop. LOSS% of packets are dropped each way.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "tcp.h"
#include "ip.h"
#include "macip.h"
#include "bytes.h"

MacIPConfig macip_cfg;
MacIPState macip_state = MACIP_UP;

static int tunfd, loss_pct, failures;
static unsigned long dropped_in, dropped_out;

unsigned long TickCount(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 60 + (unsigned long)(t.tv_nsec / 16666667);
}

void SystemTask(void) {}
void macip_poll(void) {}

static int lose(void)
{
    return loss_pct > 0 && rand() % 100 < loss_pct;
}

OSErr macip_send(const void *packet, unsigned short len)
{
    if (len > 586) {
        printf("  FAIL: datagram of %u bytes exceeds the MacIP MTU\n", len);
        failures++;
    }
    if (lose()) {
        dropped_out++;
        return noErr;
    }
    if (write(tunfd, packet, len) != len)
        perror("tun write");
    return noErr;
}

static void pump(void)
{
    static unsigned short buf[1024];
    ssize_t n;

    while ((n = read(tunfd, buf, sizeof(buf))) > 0) {
        if (lose()) {
            dropped_in++;
            continue;
        }
        if (n > 586) {
            printf("  FAIL: kernel sent %zd bytes (> 586)\n", n);
            failures++;
            continue;
        }
        ip_input((unsigned char *)buf, (unsigned short)n);
    }
    tcp_poll();
    usleep(2000);
}

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static int listener(int port)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    inet_pton(AF_INET, "10.9.0.1", &a.sin_addr);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
        perror("listen");
        exit(1);
    }
    return s;
}

static int accept_one(int ls)
{
    unsigned long until = TickCount() + 30 * 60;
    int s;

    while ((s = accept4(ls, NULL, NULL, SOCK_NONBLOCK)) < 0) {
        if (TickCount() > until) {
            printf("  FAIL: the kernel never accepted (segs in %lu out %lu)\n", tcp_stats.segs_in, tcp_stats.segs_out);
            exit(1);
        }
        pump();
    }
    return s;
}

static unsigned char pattern(unsigned long i)
{
    return (unsigned char)((i * 7 + (i >> 8)) & 0xFF);
}

static short connect_to(int port, unsigned char *rbuf, unsigned short rlen, unsigned long timeout_s)
{
    unsigned long until = TickCount() + timeout_s * 60;
    short c = tcp_open(0x0A090001, (unsigned short)port, rbuf, rlen);

    while (c >= 0 && tcp_state(c) == TCP_SYN_SENT && TickCount() < until)
        pump();
    return c;
}

static void test_echo(int ls)
{
    static unsigned char rbuf[2048];
    char got[64] = "";
    const char *msg = "hello from talkip\r\n";
    unsigned short n = 0;
    unsigned long until;
    int s;
    short c;

    printf("echo:\n");
    c = connect_to(7000, rbuf, sizeof(rbuf), 30);
    check(c >= 0 && tcp_state(c) == TCP_ESTABLISHED, "connects");
    s = accept_one(ls);
    tcp_send(c, msg, strlen(msg));
    until = TickCount() + 30 * 60;
    while (TickCount() < until) {
        char b[256];
        ssize_t k = read(s, b, sizeof(b));
        if (k > 0)
            write(s, b, k);             /* echo it back */
        pump();
        n += tcp_recv(c, got + n, sizeof(got) - 1 - n);
        if (n >= strlen(msg))
            break;
    }
    got[n] = '\0';
    check(strcmp(got, msg) == 0, "echo comes back intact");
    close(s);                           /* server closes first */
    until = TickCount() + 30 * 60;
    while (!tcp_peer_closed(c) && TickCount() < until)
        pump();
    check(tcp_peer_closed(c) && tcp_state(c) == TCP_CLOSE_WAIT, "sees the server's FIN (close-wait)");
    tcp_close(c);
    until = TickCount() + 30 * 60;
    while (tcp_state(c) != TCP_CLOSED && TickCount() < until)
        pump();
    check(tcp_state(c) == TCP_CLOSED && tcp_error(c) == noErr, "closes cleanly (last-ack -> closed)");
    tcp_release(c);
}

static void test_download(int ls, unsigned long size, unsigned short rlen)
{
    static unsigned char rbuf[8192];
    unsigned char b[600];
    unsigned long sent = 0, got = 0, bad = 0, t0, ms;
    int s, closed = 0;
    short c;

    printf("download %lu bytes, receive buffer %u:\n", size, rlen);
    c = connect_to(7000, rbuf, rlen, 30);
    check(c >= 0 && tcp_state(c) == TCP_ESTABLISHED, "connects");
    s = accept_one(ls);
    t0 = TickCount();
    while (!(tcp_peer_closed(c) && tcp_unread(c) == 0) && TickCount() - t0 < 600 * 60) {
        if (sent < size) {
            unsigned char out[4096];
            unsigned long k = size - sent < sizeof(out) ? size - sent : sizeof(out), i;
            ssize_t w;
            for (i = 0; i < k; i++)
                out[i] = pattern(sent + i);
            w = write(s, out, k);
            if (w > 0)
                sent += w;
        } else if (!closed) {
            shutdown(s, SHUT_WR);
            closed = 1;
        }
        pump();
        for (;;) {
            unsigned short n = tcp_recv(c, b, sizeof(b)), i;
            if (n == 0)
                break;
            for (i = 0; i < n; i++)
                if (b[i] != pattern(got + i))
                    bad++;
            got += n;
        }
    }
    ms = (TickCount() - t0) * 1000 / 60;
    printf("  %lu bytes in %lu ms (%lu bytes/s)\n", got, ms, ms ? got * 1000 / ms : 0);
    check(got == size, "all bytes arrive");
    check(bad == 0, "bytes are in order and intact");
    tcp_close(c);
    while (tcp_state(c) != TCP_CLOSED && TickCount() - t0 < 700 * 60) {
        char x[64];
        while (read(s, x, sizeof(x)) > 0)
            ;
        pump();
    }
    check(tcp_state(c) == TCP_CLOSED, "closes");
    close(s);
    tcp_release(c);
}

static void test_upload(int ls, unsigned long size)
{
    static unsigned char rbuf[1024];
    unsigned long queued = 0, got = 0, bad = 0, t0;
    int s, eof = 0;
    short c;

    printf("upload %lu bytes:\n", size);
    c = connect_to(7000, rbuf, sizeof(rbuf), 30);
    check(c >= 0 && tcp_state(c) == TCP_ESTABLISHED, "connects");
    s = accept_one(ls);
    t0 = TickCount();
    while (!eof && TickCount() - t0 < 600 * 60) {
        unsigned char b[2048];
        ssize_t k;
        if (queued < size) {
            unsigned char out[512];
            unsigned short n = size - queued < sizeof(out) ? (unsigned short)(size - queued) : sizeof(out), i;
            for (i = 0; i < n; i++)
                out[i] = pattern(queued + i);
            queued += tcp_send(c, out, n);
            if (queued == size)
                tcp_close(c);
        }
        pump();
        while ((k = read(s, b, sizeof(b))) > 0) {
            ssize_t i;
            for (i = 0; i < k; i++)
                if (b[i] != pattern(got + i))
                    bad++;
            got += k;
        }
        if (k == 0)
            eof = 1;
    }
    printf("  %lu bytes in %lu ms\n", got, (TickCount() - t0) * 1000 / 60);
    check(got == size && bad == 0, "server receives everything intact, then our FIN");
    close(s);
    while (tcp_state(c) != TCP_CLOSED && tcp_state(c) != TCP_TIME_WAIT && TickCount() - t0 < 700 * 60)
        pump();
    check(tcp_state(c) == TCP_TIME_WAIT || tcp_state(c) == TCP_CLOSED, "ends in time-wait or closed");
    tcp_release(c);
}

static void test_refused(void)
{
    static unsigned char rbuf[256];
    short c;

    printf("refused:\n");
    c = connect_to(7999, rbuf, sizeof(rbuf), 30);
    check(tcp_state(c) == TCP_CLOSED && tcp_error(c) == TCP_ERR_REFUSED, "a closed port gives TCP_ERR_REFUSED");
    tcp_release(c);
}

int main(int argc, char **argv)
{
    struct ifreq ifr;
    int ls;

    setvbuf(stdout, NULL, _IOLBF, 0);
    loss_pct = argc > 1 ? atoi(argv[1]) : 0;
    srand(1);
    tunfd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strcpy(ifr.ifr_name, "tun9");
    if (tunfd < 0 || ioctl(tunfd, TUNSETIFF, &ifr) < 0) {
        perror("tun (run inside run_tcp_test.sh)");
        return 2;
    }
    if (system("ip link set lo up && ip link set tun9 mtu 586 up && ip addr add 10.9.0.1/24 dev tun9") != 0)
        return 2;
    macip_cfg.ip = 0x0A090002;
    ls = listener(7000);
    printf("loss %d%% each way\n", loss_pct);

    test_echo(ls);
    test_refused();
    test_download(ls, 60000, 4096);
    test_download(ls, 20000, 600);      /* a buffer barely over one segment: window 0 and updates */
    test_upload(ls, 20000);

    printf("segments in %lu out %lu, retransmits %lu, bad %lu, resets sent %lu; dropped in %lu out %lu\n",
           tcp_stats.segs_in, tcp_stats.segs_out, tcp_stats.retransmits, tcp_stats.bad,
           tcp_stats.resets_sent, dropped_in, dropped_out);
    printf(failures ? "%d FAILURES\n" : "ok\n", failures);
    return failures != 0;
}
