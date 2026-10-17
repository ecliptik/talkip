# talkip: design and milestones

## Approach

- **Stack:** a tiny stack of our own, linked into each application first, to prove it out and learn. Then a
  MacTCP-compatible driver on the same code, so unmodified MacTCP applications run too (M8).
- **Gateways:** netatalk's `macipgw` as the primary gateway, and ClassicStack's MacIP mode, compared (M7). The stack
  must work with both.
- **Web:** HTTP/1.0 on the Mac, plus an optional proxy on the LAN that fetches HTTPS and simplifies pages
  ([macproxy](https://github.com/rdmark/macproxy_classic)).
- **Ported clients:** Geomys (Gopher) and Flynn (Telnet) get a talkip build option in their own repos: their MacTCP
  wrapper (`tcp.c`) is swapped for talkip's shim and heavy features are turned off. They are "Lite" builds, not forks.

## Target

Macintosh 512Ke: 68000, 128K ROM (AppleTalk in ROM), 512 KB RAM, System 6.0.8 (Finder, no MultiFinder), with an
AppleShare volume over LocalTalk (about 230 kbit/s). MacTCP plus MacPPP fail at boot on this machine with a memory
error, which is the reason for talkip.

Budget per application: a 128 KB partition minimum, with the stack and its buffers under 16 KB.

## Prior art

- NCSA Telnet 2.5 (1991) shipped its own TCP stack (KIP/MacIP, SLIP) and ran on a 512Ke; closed source.
- ElWhip (`ep00ch/lwip-contrib-mac`): lwIP + PPP over serial on a Mac 128K. It proves the RAM is enough; no
  AppleTalk, built with MPW.
- The MacIP draft (draft-ietf-appleip-MacIP-02, 1992) and two live gateways: netatalk's macipgw and ClassicStack.
- Nothing current or open source does TCP/IP over LocalTalk on a 512K Mac.

## Architecture

```
app (Status / Ping / Lookup / Fetch / Web / Geomys Lite / Flynn Lite)      or: a MacTCP app ── .ipp (TalkIP Driver)
  └─ tcpshim: the MacTCP-style API Flynn and Geomys use (_TCPCreate, _TCPActiveOpen, _TCPSend, _TCPRcv, _UDPSend, ...)
       └─ tcp / udp / ip: our own TCP (one segment in flight, in-order receive), UDP, ICMP echo; 586-byte MTU, MSS 546
            └─ macip: NBP =:IPGATEWAY, ATP ASSIGN/SERVER over raw DDP type 3, lease keepalive
                 └─ ddp: .MPP socket 72, writeDDP, an assembly socket listener → receive ring
```

- **ddp** (`ddp.c`): opens `.MPP`, then static socket 72 with our own listener.
  - The listener runs at interrupt time. It calls `ReadRest` first, straight into a free slot of an 8 × 598-byte
    ring, and copies the header fields after. Copying first overran the SCC's FIFO on a real 512Ke.
  - The main loop drains the ring.
  - Sends go through `writeDDP` (csCode 246), from the main loop only.
  - While the socket is open, `ExitToShell` is patched to close it, so a crashed application can't leave a
    listener pointing into a dead heap.
- **macip** (`macip.c`):
  - **Gateway and lease:** an NBP lookup `=:IPGATEWAY@*`, then an ATP TReq (function 1, ASSIGN) built by hand and
    sent from socket 72 as DDP type 3, so the gateway maps our IP to socket 72. `.ATP`'s PSendRequest would use a
    dynamic socket. The TResp gives the IP, nameserver, broadcast and an optional netmask.
  - **ASSIGN once:** macipgw leases a new address on every ASSIGN, so the lease is kept for the session.
  - **Keepalive:** answer ICMP echo (macipgw pings after 30 s of silence and frees the lease after about 5 min),
    and send SERVER every ~2 min when idle (ClassicStack never pings).
  - **Recovery:** if the gateway goes quiet, probe it, then look it up again.
- **tcp** (`tcp.c`): active and passive open, one segment in flight each way, and a window of at most two
  segments, so a sender can't overrun the receive ring.
  - RTO from a smoothed RTT (Jacobson/Karn): 1 s minimum, 3 s initial, doubling.
  - Receive is in order only: anything else is dropped and acked.
  - This suits ClassicStack's NAT, a userspace proxy that (before PR #21) neither checked sequence numbers nor
    retransmitted toward the Mac.
- **tcpshim** (`tcpshim.c`): the API Flynn's and Geomys's `tcp.c` expose (an NCSA-derived wrapper over MacTCP
  parameter blocks), implemented over talkip with the application's own receive buffer.
  - Synchronous calls block while pumping the stack.
  - TCPActiveOpen can also run asynchronously.
  - Flynn's and Geomys's own resolvers (`dns.c`, over UDP) then work unchanged.
- **TalkIP Driver** (`src/ipp/ipp.c`, M8): the same stack as a resident `.ipp` driver installed by an INIT, with
  MacTCP's Control calls on top of tcpshim. See [m8-ipp-driver.md](m8-ipp-driver.md).

## Gateways

| | netatalk macipgw | ClassicStack (MacIP, NAT mode) |
|---|---|---|
| Where | a Linux host beside atalkd, reaching LocalTalk through TashRouter (or any AppleTalk router) | anywhere with LToUDP (or its own LocalTalk port) |
| IP | kernel tun + NAT (iptables): real end-to-end TCP | a userspace proxy NAT |
| Leases | a new IP per ASSIGN; it pings the client | keyed by AppleTalk address, 5 min; it never pings |
| DNS handed out | its `-n` option | `[MacIP] nameserver` |

## Milestones

| # | Milestone | Result |
|---|---|---|
| M0 | Gateway: macipgw | `nbplkup =:IPGATEWAY` lists it |
| M1 | TalkIP Status: socket 72, gateway lookup, lease, answers ping | ✅ in Snow, then on the real 512Ke: 60/60 pings after the listener fix |
| M2 | TalkIP Ping | ✅ the 512Ke pings 1.1.1.1 and hosts by name |
| M3 | UDP + TalkIP Lookup | ✅ A, CNAME, PTR, NXDOMAIN through the leased nameserver |
| M4 | TCP + tcpshim | ✅ Gopher and HTTP fetches; host-tested against Linux TCP at 0/5/15 % loss |
| M5 | Geomys Lite, Flynn Lite | ✅ on the real 512Ke: Gopher browsing, Telnet |
| M6 | TalkIP Web + proxy | ✅ on the real 512Ke: Wikipedia over HTTPS through macproxy |
| M7 | ClassicStack comparison | ✅ [m7-classicstack.md](m7-classicstack.md); upstream fix ClassicStack#21 |
| M8 | MacTCP-compatible driver | ✅ on the real 512Ke: stock Geomys and Flynn through the TalkIP Driver; [m8-ipp-driver.md](m8-ipp-driver.md) |

## Testing

1. Host tests (`tests/host`): TCP against Linux TCP with loss, DNS, HTML and URL parsing.
2. Snow, an emulated 512Ke, on LToUDP through an AppleTalk router to the MacIP gateway.
3. A real 512Ke over LocalTalk: builds are copied to an AppleShare volume and run there.

## Build and test tooling looked at

- **classicmac-mcp / classicmac-kb** (mplsllc): a curated knowledge base plus C89/compatibility checks, aimed at
  CodeWarrior and PowerPC. The KB has no AppleTalk, DDP or MacTCP records yet. Its idea of recording evidence per
  level (static, Retro68, hardware) is a good one.
- **classic-mac-hardware-mcp** (Matthew Deaves): deploys over FTP and runs builds with Retro68's LaunchAPPL. Both
  need MacTCP. With the TalkIP Driver, LaunchAPPL's server runs on a 512Ke and accepts connections (M8). Using it
  remotely needs a route from the build host to the MacIP subnet.
