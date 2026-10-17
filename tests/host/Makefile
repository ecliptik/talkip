# Host tests for the portable parts of talkip (gcc on Linux, stub Mac headers in include/).
SRC = ../../src
CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -g -Iinclude -I$(SRC)

all: test_dns test_tcp test_html test_url

# ip.c hands TCP to tcp_input since M4, so the DNS test links tcp.c too
test_dns: test_dns.c $(SRC)/dns.c $(SRC)/udp.c $(SRC)/ip.c $(SRC)/tcp.c
	$(CC) $(CFLAGS) -o $@ test_dns.c $(SRC)/dns.c $(SRC)/udp.c $(SRC)/ip.c $(SRC)/tcp.c

clean:
	rm -f test_dns test_tcp test_html test_url

test_tcp: test_tcp.c $(SRC)/tcp.c $(SRC)/udp.c $(SRC)/ip.c
	$(CC) $(CFLAGS) -o $@ test_tcp.c $(SRC)/tcp.c $(SRC)/udp.c $(SRC)/ip.c

test_html: test_html.c $(SRC)/html.c
	$(CC) $(CFLAGS) -o $@ test_html.c $(SRC)/html.c

test_url: test_url.c $(SRC)/url.c
	$(CC) $(CFLAGS) -o $@ test_url.c $(SRC)/url.c
