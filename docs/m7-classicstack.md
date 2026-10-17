# M7: talkip through ClassicStack's MacIP gateway

2026-10-16. The M1–M6 tools run against ClassicStack instead of netatalk's macipgw, on an emulated 512Ke,
isolated from any real AppleTalk network.

**Result:** everything works on a clean link: lease, ping, DNS, gopher, telnet, the web. But ClassicStack's
NAT never retransmits toward the Mac, so one lost datagram hangs a TCP connection for good (2 of about 18
connections here, with no loss injected). With macipgw the Mac
talks to the real server's TCP, which resends. macipgw stays the gateway for the real 512Ke.

## Setup

```
network namespace talkipm7 (on the build host)
  lo, multicast 239.192.76.84:1954 (LToUDP) ── Snow 512Ke (Xvfb :77) and ClassicStack, seed net 71, zone "Talkip M7"
  tm7-ns 10.77.0.2 ──veth── tm7-host 10.77.0.1 ── masquerade ── LAN, internet    (ClassicStack's own outbound sockets)
```

- **ClassicStack** 3e8248a (2026-06-07), Go 1.25.11, built with **`-tags macip`**: without the tag MacIP is compiled
  out and the only sign is one log line ("-macip-enabled set but binary was built without -tags macip; ignoring").
  Config [`tests/m7/server.toml`](../tests/m7/server.toml): LToUDP only, MacIP `mode = "nat"`, 192.168.100.0/24,
  gateway .1, DNS from the host's resolver.
- **Isolation** [`tests/m7/netns-up.sh`](../tests/m7/netns-up.sh) / `netns-down.sh`: Snow and ClassicStack both use the
  fixed LToUDP group, so it is routed to `lo` inside the namespace and nft drops any multicast leaving by the veth.
  Nothing reaches TashRouter's LToUDP net 71.
- **Mac:** Snow `snowemu-hd20`, Early512Ke, Mac Plus v3 ROM, a copy of the System 6.0.8 HD20 image with the talkip
  apps, Flynn Lite and Geomys Lite in a `talkip` folder (hfsutils, with the emulator stopped).
- **Driving:** [`tests/m7/m7drive.py`](../tests/m7/m7drive.py) over the emulators' XTEST library. Xvfb has no window
  manager, so it sets X focus on Snow first (otherwise every key is dropped), and it sends keys slowly (faster drops
  keys). Traces: `tcpdump -i lo udp port 1954` in the namespace, decoded by
  [`tests/m7/ltoudp-tcp.py`](../tests/m7/ltoudp-tcp.py).

Run: `sudo sh tests/m7/netns-up.sh`, then in the namespace `classicstack -config server.toml`, `Xvfb :77` and
`snowemu-hd20 m7.snoww` (as an unprivileged user).

## Results

| Test | Through ClassicStack |
|---|---|
| TalkIP Status | IPGATEWAY at 71.254:72; first NBP lookup after boot found nothing, talkip retried and leased 192.168.100.2 (mask 255.255.255.0, the configured DNS) |
| TalkIP Ping | gateway 4/4, 33–133 ms; 1.1.1.1 4/4, 17–167 ms; `example.com` by name → 172.66.147.243, 4/4 |
| TalkIP Lookup | `info.cern.ch` A 188.184.67.127 + CNAME `webafs902.cern.ch`; PTR 1.1.1.1 → `one.one.one.one`; NXDOMAIN reported |
| TalkIP Fetch | `floodgap.com:70/` 5,639 bytes in 2.1–2.5 s (2.3–2.7 KB/s), clean close: 6 of 7 runs; 1 hung (below) |
| Flynn Lite | telnet to telehack.com, banner and commands; 1 of 2 sessions hung mid-banner (631 bytes never arrived) |
| Geomys Lite | Directory › Floodgap Systems: `gopher.floodgap.com`, Done – 92 items |
| TalkIP Web | `http://info.cern.ch/`, link to TheProject.html (2 KB in 1 s), Back |
| 5 % loss toward the Mac (nft, ClassicStack's frames only) | the first lost segment hangs the connection for good; with the new idle timeout Fetch gives up after 91 s and the next fetch works |

The loss trace, relative sequence numbers: ClassicStack sent 2185+69, which was dropped; the Mac acked 2185 three
times; ClassicStack never resent, and once 2185 + the Mac's 1092-byte window was in flight it stopped sending.

```
1.617 net->  PA  seq 2254 ack 3 len 546
1.628 mac->  A   seq 3 ack 2185          duplicate ACK
1.637 mac->  A   seq 3 ack 2185          duplicate ACK
1.644 net->  PA  seq 2800 ack 3 len 477
1.908 mac->  A   seq 3 ack 2185          ... nothing more, ever
```

## Differences that matter

| | macipgw | ClassicStack 3e8248a, NAT mode |
|---|---|---|
| TCP endpoint | the real server; kernel routes and NATs IP | ClassicStack itself: a userspace proxy per flow (`port/nat/ipnat.go`) |
| Lost segment toward the Mac | the server retransmits | **never resent**: sends once (`tcpServerReadLoop`), ignores duplicate ACKs; FIN also sent once |
| Segment from the Mac | forwarded as is | **no sequence check** (`handleTCP`): `macSeq += len`, payload written to the server whatever its seq, so a retransmit is written twice and out-of-order data out of order (from the source; not provoked here) |
| Sending to the Mac | the server's pacing | as much as the Mac's window allows, one segment per server read: telehack's small writes became 4 datagrams in flight at once |
| Mac's FIN | ACKed by the server | flow already deleted after the server's FIN, so not ACKed (harmless: talkip resets after its FIN) |
| Leases | new IP per ASSIGN; pings the client every 30 s of silence | same IP for the same AppleTalk address (192.168.100.2 every launch); never pings |
| ICMP | routed | proxied through the host's ICMP socket: real echoes from outside hosts; the gateway address answers itself |
| UDP/DNS | routed | proxied per flow; A, CNAME, PTR and NXDOMAIN all fine |
| NBP right after the Mac boots | answered | IPGATEWAY missed on the first lookup (2 of 2 boots); talkip's retry finds it 5 s later |

Where drops come from with no injected loss: talkip's socket listener (`src/ddp.c`) queues datagrams in a 4-slot
ring and drops when the app hasn't drained it, e.g. while Flynn redraws. That happens with either gateway; only on
macipgw is it repaired.

## talkip changes from M7

- **Fetch and Web give up on a silent connection** after 90 s ("No data for 90 s, gave up"), so a hung flow (any
  gateway, or a gateway restart) no longer leaves the app waiting forever.
- **Web: status line stuck on "Loading"** after a finished page. An update event (the URL field) repainted only its
  clipped region but cleared the pending full redraw. The update now keeps it pending.
- **Web: home page never loaded** when the gateway wasn't found at startup; the stack's retry got the lease but
  nothing started the page or redrew the status. Web now loads home when MacIP comes up.
- `tests/host/Makefile`: `test_dns` links `tcp.c` (ip.c hands TCP to it since M4); `all` is the default target.

## Recommendations

1. Keep macipgw for the real 512Ke. ClassicStack is fine for short exchanges, but long transfers and chatty telnet
   will hang now and then.
2. talkip: a bigger listener ring (4 → 8 slots, +2.4 KB) would cut drops with both gateways (with macipgw each drop
   costs a retransmit timeout). It touches the listener tuned on the real 512Ke, so it needs a real-Mac retest.
3. ClassicStack upstream: keep unacknowledged data per flow and resend on duplicate ACKs or a timer, and check
   sequence numbers from the Mac. **Patch ready** in [m7-classicstack-patch/](m7-classicstack-patch/) (2026-10-16): under
   5 % loss, Fetch went from 1 of 5 completed to 8 of 8 (2.0–4.6 s); a 3-minute telehack session never stalled.
   **Posted** as
   [ObsoleteMadness/ClassicStack#21](https://github.com/ObsoleteMadness/ClassicStack/pull/21) (2026-10-16, from
   a fork's `nat-tcp-retransmit` 82009f9; the same fix on #20's head is branch
   `nat-tcp-retransmit-on-pr20` a4db29d). Posted text: [PR-posted.md](m7-classicstack-patch/PR-posted.md).
4. ~~Geomys Lite: Return in the address bar didn't navigate in this run.~~ It works in Snow and on the real 512Ke
   (2026-10-16): the M7 miss was in the test rig (most likely a Return dropped on the bare Xvfb), not the app.
5. ~~The Fetch and Web fixes have run in Snow only.~~ Run on the real 512Ke through macipgw, 2026-10-16 (below).

## On the real 512Ke (2026-10-16, through macipgw)

Builds of 51c244e, run from an AppleShare volume.

| Test | Result |
|---|---|
| TalkIP Fetch, `floodgap.com:70/` | 5 of 5: 5,639 bytes in 2,150 / 2,266 / 2,383 / 2,316 / 2,333 ms, closed by the server. Fetch's first run on hardware |
| Fetch idle timeout | a listener on the gateway host (port 7070) that accepts and never answers: "0 bytes in 90033 ms, no data for 90 s, gave up"; the listener saw the Mac's reset |
| TalkIP Web | home page loaded at launch, status "Done: 1 KB in 0 s" (the old build stuck on "Loading"); link to TheProject.html "Done: 2 KB in 2 s"; Back; `https://example.com/` through macproxy |
| Geomys Lite | address bar + Return: `gopher.floodgap.com`, Done – 92 items |

Not provoked on hardware: Web's late-gateway home load (it needs the first NBP lookup to miss; Snow only).

**A bomb, not ours.** After the first Geomys Lite quit, the Finder sat on the watch cursor and showed "address
error" about 6 minutes later. The boot disk, a Floppy Emu in HD20 mode, then reported a checksum error. The same
steps again were fine, and so was Snow. AppleTalk was healthy throughout, and talkip's quit closes socket 72, so
nothing of it runs after the app quits. A later check of the disk image was clean; the Finder was running out of
memory on an oversized Desktop file, and a lean boot image fixed it.
