# nat: retransmit TCP toward the Mac and check the Mac's sequence numbers

Posted as https://github.com/ObsoleteMadness/ClassicStack/pull/21 on 2026-10-16 (head ecliptik:nat-tcp-retransmit 82009f9).

Claude generated commit/PR after running across a bug while testing ClassicStack on a slimmed down TCP/IP stack on a Macintosh 512Ke.

----

## Summary

In MacIP NAT mode, the OS-stack NAT (`port/nat/ipnat.go`) terminates each Mac TCP connection itself, but it sent every
segment toward the Mac exactly once. LocalTalk loses frames, so one lost segment, SYN-ACK or FIN hung the connection
for good. The NAT also wrote the Mac's retransmits to the server a second time. This PR adds retransmission toward the
Mac and only writes in-order bytes from the Mac to the server. All changes are in `port/nat/ipnat.go`, and the tests
are in a new `port/nat/ipnat_test.go`.

## Problem

- `tcpServerReadLoop` sends data and the FIN once and keeps no copy. Duplicate ACKs from the Mac are ignored, and there
  is no retransmission timer. **One lost frame hangs the connection for good.**
- A retransmitted SYN for an existing flow is ignored (`if _, exists := n.tcpFlows[key]; exists { return }`), so a
  lost SYN-ACK hangs the connect.
- `handleTCP` does `macSeq += len(payload)` and writes the payload to the server whatever its sequence number. A
  retransmit from the Mac reaches the server twice, and the NAT then ACKs bytes the Mac never sent. A strict client
  ignores an ACK above its `snd_nxt`, so it never sees its data acknowledged and keeps resending it. Each copy is
  written to the server again.
- The flow is deleted as soon as the server closes, so the Mac's FIN is never ACKed.

LocalTalk does lose frames. Besides collisions, a 512K Mac's driver drops any datagram it has no buffer for while the
application is busy. On a clean LToUDP link, 2 of about 18 connections from an emulated 512Ke hung this way.

The comments in #20's `core/service/macip/tcp.go` and `macip.go` describe the same failure: OSNAT "does not
retransmit, so ... a single dropped segment DEADLOCKED the flow (capture ltoudp-netboot.pcap)".

## Repro

The client is a small MacIP TCP stack on an emulated Macintosh 512Ke (Snow, System 6.0.8) over LocalTalk. The stack
keeps one segment in flight, with MSS 546 and a 1092-byte window. It talks to ClassicStack 3e8248a (`-tags macip`,
`[MacIP] mode = "nat"`) over LToUDP in a private network namespace. nftables drops 5% of ClassicStack's LToUDP frames:

```
nft add rule ip t out udp dport 1954 @th,64,32 <ClassicStack's LToUDP sender id> numgen random mod 100 < 5 drop
```

The test is a gopher fetch of `floodgap.com:70/` (5,639 bytes), repeated.

Before the fix, the first connection lost the NAT's ACK of the 2-byte request. The capture below uses relative
sequence numbers. It was taken on the LToUDP side after the drop rule, so a dropped frame doesn't appear:

```
 0.119 mac->  PA  seq 1    ack 1    len 2      request
 0.962 mac->  PA  seq 1    ack 1    len 2      Mac resends it (the NAT's ACK was lost)
 0.970 net->  A   seq 1    ack 5               request written to the server twice; ACK 5 is above the Mac's snd_nxt (3)
 ...
 1.430 mac->  A   seq 3    ack 2185            a reply segment was lost
 1.443 mac->  A   seq 3    ack 2185            duplicate ACK
 1.669 mac->  A   seq 3    ack 2185            duplicate ACK, never resent
 2.639 mac->  PA  seq 1    ack 2185 len 2      the Mac never saw its request acknowledged
 2.646 net->  A   seq 3277 ack 7               written a third time
 ... (6.3, 13.0, 26.6, 54.2 s: the same again)
79.016 mac->  RA                               the application gives up
```

## What's changed

**Data toward the Mac**
- Data stays in `unacked` until the Mac ACKs it. The oldest segment is resent:
  - on 3 duplicate ACKs (RFC 5681 fast retransmit);
  - at once on a partial ACK during recovery (RFC 6582). The Mac keeps no out-of-order data, so the segment after a
    lost one is gone too;
  - on a timer: 1 s, doubling to 16 s. After 8 resends the flow is reset.
- ACKs above `ourSeq` are ignored.

**Connection setup and close**
- A SYN that repeats the flow's ISN gets the SYN-ACK again.
- The FIN toward the Mac is resent the same way until it is acknowledged. The flow then stays open until the Mac has
  closed its side, or for at most 1 minute, so the Mac's FIN is ACKed.

**Data from the Mac**
- Only in-order bytes are written to the server. The part of a retransmit that was already written is trimmed, and
  out-of-order data is dropped. Any data or FIN segment is answered with an ACK for the next byte expected.

The window logic is unchanged: data in flight stays within the Mac's advertised window. Retransmission state is per
flow and guarded by the existing `flow.mu`. A generation counter stops a timer callback that fires late from acting.

## Results

The same rig at 5% loss, before and after the fix. Times are as reported by the client application:

| | Fetches that completed | Time |
|---|---|---|
| 3e8248a | **1 of 5**. The other 4 hung until the client's 90 s idle timeout | 2.4 s for the one that completed |
| this PR | **8 of 8** | 2.0–4.6 s (median 2.5 s) |

Here is a recovery after the fix. Two segments were lost. The timer resent the first, and the partial ACK resent the
second 7 ms later:

```
185.960 mac->  A   seq 3 ack 4438      duplicate ACKs for 4438
186.890 net->  PA  seq 4438 len 546    timeout resend
186.965 mac->  A   seq 3 ack 4984      partial ACK
186.972 net->  PA  seq 4984 len 546    resent at once
187.020 net->  FA  seq 5640
187.086 mac->  FA  seq 3 ack 5641
187.092 net->  A   seq 5641 ack 4      the Mac's FIN is ACKed
```

Other results after the fix:

- **Lost SYN-ACK:** in one run the SYN-ACK was lost. The Mac resent its SYN 2.6 s later, got a new SYN-ACK, and the
  fetch completed.
- **Telnet under the same loss:** a 3-minute session with telehack.com: the banner, `help` (4 pages) and `cowsay`. 8
  of ClassicStack's frames were dropped and 4 data segments were resent. The session never stalled.
- **Clean link:** 3 of 3 fetches completed. There was one natural drop, a datagram the Mac had no buffer for, and the
  timer repaired it in 1 s. Before the fix, that drop would have hung the fetch.

## Testing

The new `port/nat/ipnat_test.go` drives `Forward` and `tcpServerReadLoop`. It uses a `net.Pipe` as the server and a
router stub that records what is routed to the Mac. It tests that:

- a Mac retransmit, an overlapping retransmit and out-of-order data reach the server once and in order, and each is
  answered with the right ACK;
- 3 duplicate ACKs resend the oldest segment;
- the timeout resends, and an ACK stops the timer;
- a partial ACK resends the next segment at once;
- the FIN is resent until it is ACKed, the Mac's FIN is ACKed, and the flow leaves the table;
- a retransmitted SYN gets the SYN-ACK again.

The following checks pass on the branch, which is based on main at 3e8248a (Go 1.25.11, linux/amd64):

- `go test -race -count=1 ./port/nat/`, run 3 times.
- `go test -count=1 ./...`: 41 packages ok, 0 failed.
- `go test -tags all -race -count=1 ./...`: 47 packages ok, 0 failed.
- `go vet ./...` and `go vet -tags all ./...` are clean.
- `go build` with `-tags macip`, `-tags "afp macip"` and `-tags all` succeeds.
- `golangci-lint run --build-tags=all ./port/nat/...` (v2.5.0) reports 0 issues, and `gofmt -l` is clean.

## Notes

- **#20:** the rewrite moves this code to `adapter/macipgw/nat/osnat.go`, and the change ports over directly. Branch
  [`nat-tcp-retransmit-on-pr20`](https://github.com/ecliptik/ClassicStack/tree/nat-tcp-retransmit-on-pr20) has it on top of #20 if that's more useful.
- Out of scope: there is no persist timer for a zero window. The Mac's window updates, and the resend timer while data
  is outstanding, cover the case seen here.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
