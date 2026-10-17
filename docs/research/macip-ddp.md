# MacIP client over DDP on a 512Ke (System 6.0.8): implementation facts

Sources were cloned under `research/src/` (paths below are relative to it):
- netatalk @ f28210a: `Netatalk_netatalk/repo`
- ClassicStack @ 3e8248a: `ObsoleteMadness_ClassicStack/repo`
- Retro68 @ 91b5d69, plus its multiversal submodule @ 057c802
- uIP @ a49def7
- lwip-contrib-mac (ElWhip) @ 9485de4
- PeerTalk @ cc8eb5d

I downloaded Inside Macintosh: Networking pages 20–260 to `src/im_networking/`; text copies are in `txt/NNNN.txt`. I also fetched Apple's TalkTool `DDPListener.a`, technotes NW02, NW03 and NW13, and draft-ietf-appleip-MacIP-02 (`src/specs/macip-02.txt`).

Confidence tags:
- **[V]**: verified in source.
- **[D]**: derived from verified equates.
- **[U]**: uncertain.

## 1. The MacIP client protocol, as the gateways implement it

### Finding the gateway

Run an NBP lookup for `=:IPGATEWAY@<zone>`.
- Both gateways register a name whose object is the gateway's IP in dotted form, on DDP socket 72 [V]:
  - macipgw: `macip.c:63-65,765-807`
  - ClassicStack: `service/macip/macip.go:211`
- `@*` only matches the Mac's own zone (draft §3.2.2.1). If the gateway sits in another zone, look up that zone by name.

### Sockets and DDP types (both gateways) [V]

- Everything goes to gateway socket 72:
  - DDP type 3 carries the ATP configuration exchange.
  - DDP type 22 carries raw IP datagrams, starting with the IP header. There is no extra framing.
  - macipgw: `macip.c:55-61,513-525`. ClassicStack: `macip.go:32-38,334-339`.
- The maximum IP size is 586 bytes. This is the full DDP data size, and macipgw drops anything larger (`macip.c:349-358`). Its Linux tun MTU is 586 (`tunnel_linux.c:106`).
- ClassicStack fragments anything larger than 586 bytes (`port/nat/iputil.go:10`, `macip.go:513`).
- **ClassicStack always delivers IP to the client's socket 72** (`macip.go:519-526`, `ipnat.go:652-668`).
- macipgw delivers to whatever socket it last learned for that IP (see "ARP" below).
- The draft says the host "MUST register on DDP socket 72" because gateways assume it (`macip-02.txt:617-620`).
- **Conclusion: open DDP socket 72 yourself. Use it for both IP and the ATP request.**

### ATP configuration request (MacIPGP)

This is an at-least-once (ALO) ATP TReq sent to gateway socket 72.

Netatalk's ATP "data" includes the 4 user bytes (`include/atalk/atp.h:73-75`: `ATP_HDRSIZE 5` includes the DDP type byte; `libatalk/atp/atp_rreq.c:121-130`). As a result, `struct macip_req_control {u16 version; u16 pad; i32 function}` maps onto user bytes plus the first 4 data bytes [V].

Request, DDP type 3, as 12 bytes of DDP data:

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | `0x40` | TReq, ALO, no XO |
| 1 | `0x01` | Bitmap: 1 response |
| 2 | TID (2 bytes) | Transaction ID |
| 4 | `00 01 00 00` | User bytes = version 1 + pad. Ignored by both gateways and by the draft (§3.8.6) |
| 8 | `00 00 00 01` | Function, int32 BE: 1 = ASSIGN, 3 = SERVER |
| 12 | optional 4 bytes | Preferred IP. ClassicStack only, `macip.go:378-381`; it honours the address if free (`pool.go:114-119`). macipgw ignores it |

- ClassicStack requires at least 12 bytes and control & 0xC0 == 0x40 (`macip.go:361-370`).
- macipgw handles it in `config_input`, `macip.c:381-485`.

Response, DDP type 3, from gateway socket 72 back to the requesting socket:

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | `0x90` | TResp + EOM |
| 1 | `0x00` | Sequence number |
| 2 | TID | Echoes the request |
| 4 | `00 01 00 00` | User bytes, version 1 |
| 8 | int32 | Function. macipgw echoes 1 or 3, or -1 on error. ClassicStack always answers 1 |
| 12 | 4 bytes | Assigned IP. 0.0.0.0 from ClassicStack when the pool is exhausted |
| 16 | 4 bytes | Nameserver |
| 20 | 4 bytes | Broadcast |
| 24 | 4 bytes | Pad. The draft calls this field "File Server" |
| 28 | 4 bytes | Subnet mask (the draft's first "Other" word) |
| 32 | 12 bytes | Pad. macipgw only |
| 44 | NUL-terminated | Error string. macipgw only |

Sizes:
- macipgw sends 41 bytes of ATP data after the header, or more when it includes an error message (`macip.c:431-474`).
- ClassicStack sends exactly 24 data bytes after the user bytes (`macip.go:414-428`).
- **So parse at least function + 20 bytes, and treat the mask as optional.**

Errors: function = -1 (0xFFFFFFFF). macipgw texts are "No Address Available." and "Unknown Operation." (`macip.c:140-141`).

### How the gateway learns your AppleTalk address (its "ARP")

**macipgw:**
- On ASSIGN it leases a new IP every time. It does not check for an existing lease, so every duplicate or retransmitted ASSIGN burns an address.
- It maps that IP to the requester's net, node and socket (`macip.c:208-224,444-448`).
- Every received type-22 packet overwrites the mapping with the packet's source address (`arp_set`, `macip.c:262-281,335`).
- On a miss it sends NBP lookups for `<ip>:IPADDRESS@<every zone>` (`macip.c:231-255`).
- At startup it looks up `=:IPADDRESS@*` (`macip.c:817`).

**ClassicStack:**
- Keys leases by AppleTalk net and node.
- Re-ASSIGN from the same AppleTalk address renews the existing lease (`pool.go:97-133`).
- Its router normalises short-header source network 0 to the port's network (`router/router.go:266-277`).

### Keepalive and lease expiry

**macipgw:**
- About every 30 s (`ARPTIMEOUT`) with no traffic from you, it sends an ICMP echo *from* its own IP (net+1) to your IP.
- After 10 unanswered pings (about 5 min) it frees the lease (`macip.c:72-73,714-742`).
- SERVER (3) does **not** refresh the lease. **You must answer ICMP echo.**

**ClassicStack:**
- Lease is 5 min since last seen (`pool.go:15`).
- Refreshed by any type-22 packet from you (`macip.go:480`) or by any ATP request (in pool mode both functions go through `assign`).
- It never pings. **Send something, such as SERVER, at least every 2 min when idle.**

**Draft:** the host should also NBP-register `a.b.c.d:IPADDRESS@*` on socket 72 and remove it on exit (`macip-02.txt:604-626`). Neither modern gateway needs it, but macipgw uses it as fallback ARP.

**Routing:** there is no "router" field. Send every IP datagram, for any destination, to the gateway's NBP address on socket 72.

### ClassicStack NAT mode: important quirks

**It is not a NAT. It is a userspace TCP proxy** (`port/nat/ipnat.go:373-532`):
- It dials the real server and then fakes the SYN-ACK. If the dial fails it sends a RST.
- It never retransmits anything toward the Mac. Sent data is lost if the Mac drops it.
- **It does not check sequence numbers on Mac→server data** (`ipnat.go:452-475`). Every payload is appended to `macSeq` and written to the server. A Mac retransmission caused by a lost or late ACK therefore **duplicates bytes in the server stream**.
- A retransmitted SYN after the flow exists is ignored (`:399-404`). A lost SYN-ACK hangs the connection.
- It respects the Mac's advertised window (`:546-563`). Its own window is fixed at 8192, and its MSS is min(546, client MSS) (`:34,409-431`).
- TCP idle timeout is 5 min. UDP and ICMP time out after 30 s (`:24-35`).
- ICMP forwarding needs a raw socket (`:141-147`).
- Only IP protocols 1, 6 and 17 are forwarded (`:173-190`).
- Inbound connections are impossible.

**Client advice:**
- Advertise a small window (1×MSS) and make sure the listener ring always has space.
- Use a conservative RTO.
- Never send overlapping retransmits.

**DNS handed out:**
- `[MacIP] nameserver`.
- If that is empty, it falls back to `ip_gateway`, the upstream LAN router (`internal/app/macip_enabled.go:145-151`).
- In DHCP-relay mode it uses the DHCP-supplied DNS (`macip.go:445-448`).

**ICMP to the gateway:** pings to `nat_gw` are answered locally, in NAT mode only (`macip.go:483-486,566-606`).

### ClassicStack standalone on LToUDP

- The LToUDP group is 239.192.76.84:1954 (`port/localtalk/ltoudp.go:20-22`). The default node is 0xFE (`:35`).
- `seed_network = 0` appears to give a non-seeding port that learns its network from RTMP (`localtalk.go:289-307,449-456`) [U-ish]. Otherwise seed 71 with the exact zone TashRouter uses.
- **Caveat:** MacIP still opens a raw link even in NAT mode. It errors if there is no device, and it needs pcap or CAP_NET_RAW (`macip_enabled.go:59-76,175-178`).
  - An empty `[Bridge] device` disables EtherTalk (`internal/app/config_ini.go:276-288`, `supervisor.go:215`).
  - Give MacIP its own device via `[MacIP.Custom]` (`config/model.go:143-146`).
- **Caveat:** `zone` defaults to the *EtherTalk* zone (`server.toml.example:43`, `macip_enabled.go:153-159`). Set it explicitly.

Example config:

```toml
[Bridge]
device = ""                          # EtherTalk off
[Router]
ports = ["LToUdp"]
[LToUdp]
enabled = true
interface = "0.0.0.0"
seed_network = 71                    # or 0 (non-seed) [U]
seed_zone = "<TashRouter's zone>"
[TashTalk]
port = ""
[MacIP]
enabled = true
mode = "nat"
zone = "<same zone>"
nat_subnet = "192.168.100.0/24"
nat_gw = "192.168.100.1"
ip_gateway = "<LAN router>"
dhcp_relay = false
nameserver = "<DNS>"
[MacIP.Custom]
mode = "pcap"
device = "<eth iface>"
```

Notes on these keys:
- Key names come from `server.toml.example:1-49` and `config/model.go:97-146`.
- `dhcp_relay` must be false for the pool to be used (`macip.go:386-398`).
- macipgw cannot run here, because it needs kernel AppleTalk via atalkd (it uses AF_APPLETALK sockets).

## 2. AppleTalk Manager from C on a 128K ROM / System 6.0.8

### Headers

- Retro68's bundled Multiversal interfaces have **no AppleTalk declarations**. `defs/AppleTalk.yaml` contains only the low-memory globals `PortBUse` (0x291), `ABusVars` (0x2D8) and `ABusDCE` (0x2DC).
- Apple Universal Interfaces are optional (Retro68 README:58-90). Without them, declare the parameter blocks yourself and call `PBControl` directly.
- That is all the "P" calls are: glue that fills in `ioRefNum` and `csCode` and issues `_Control` (NW02).
- The same csCodes are listed as the IM II assembly interface, so ROM drivers do not care whether the "preferred" glue is used.
- **Version check [U]:**
  - The 128K-ROM (Plus) .MPP is v19; System 6.0.8 may have a RAM AppleTalk installed by Network Software Installer (NW13 table).
  - Check `SysEnvirons` → `atDrvrVersNum`, or the byte at .MPP DCE+7 (NW03).
  - The calls below are basic DDP, NBP and ATP; I did not verify them against v19, but I expect them to work.
  - `PGetAppleTalkInfo`, `PNSendRequest` and `verifyFlag` are Phase-2 or IM V additions. Avoid them.

### Opening .MPP

- Call `OpenDriver("\p.MPP",&ref)`. The refNum is -10. NW13 says `MPPOpen` is obsolete.
- With AppleShare already active, the open is a no-op that returns noErr. **Never close .MPP.**

### MPP parameter block (IM:N `N203`, `N83`, `N178`)

Common fields:

| Offset | Field |
|---|---|
| 0 | qLink |
| 4 | qType |
| 6 | ioTrap |
| 8 | ioCmdAddr |
| 12 | ioCompletion |
| 16 | ioResult (word) |
| 18 | ioNamePtr / ATP userData (long) |
| 22 | ioVRefNum / reqTID |
| 24 | ioRefNum |
| 26 | csCode |

DDP fields:

| Offset | Field |
|---|---|
| 28 | socket (byte) |
| 29 | checksumFlag (byte) |
| 30 | listener or wdsPointer (long; the same field, union) |

DDP csCodes: `writeDDP` 246, `closeSkt` 247, `openSkt` 248.

### openSkt

- Set socket = 72. Static sockets are 1–127, and 64–127 are allowed for development.
- `ddpSktErr` is returned if the socket is already open (`N198`).
- The listener must not be NIL.

### writeDDP

- The WDS is a list of `{short len; Ptr p}` entries terminated by a 0 word. BuildDDPwds's version is 14 bytes: header entry, data entry, 0 (`N202`).
- The first entry points to a 16-byte header block **at an odd address**. Use a 17-byte buffer `h` at an even address, so the block starts at `h+1`.
- You fill only these fields [D: block = LAP(3)+long DDP(13), using the `N188` equates]:

| Field | Offset in block | Index in `h` |
|---|---|---|
| dstNet (word) | 7 | `h[8..9]` (word-aligned) |
| dstNode | 11 | `h[12]` |
| dstSkt | 13 | `h[14]` |
| ddpType | 15 | `h[16]` |

- `checksumFlag` only matters for long headers. Use 0, since IP has its own checksums (`N201`, `N188`).
- Data is at most 586 bytes.
- Call it from the main loop, not from the listener.

### Socket listener (`N188`, `N189`; TalkTool `DDPListener.a`)

This must be assembly. Registers on entry:

| Register | Meaning |
|---|---|
| A0, A1 | MPP internal. Preserve until ReadRest |
| A2 | MPP locals. RHA = `toRHA(A2)`, and **toRHA = 1** [D from TalkTool: lapType read at 3(A2)] |
| A3 | First RHA byte after the DDP header, so `-1(A3)` = DDP type |
| A4 | ReadPacket = `JSR (A4)`; **ReadRest = `JSR 2(A4)`** |
| A5 | Free until ReadRest; must be preserved after ReadRest |
| D0.b | Destination socket |
| D1.w | Bytes left (the payload, i.e. the IP datagram length). The high word may be garbage, so zero-extend |
| D2, D3 | Free |

Rules:
- Always preserve A6 and D4–D7.
- ReadRest takes A3 = buffer and D3.w = buffer size. D3 = 0 discards the packet.
- On exit from ReadRest, D3 = 0 means an exact fit, <0 means overflow by -D3, >0 means slack.
- Check the Z flag after every ReadPacket or ReadRest. **Calling ReadRest is mandatory** or the machine crashes.
- After ReadRest, A0–A3 and D0–D3 are free, then `RTS`.
- On LocalTalk you have **<95 µs** before ReadPacket or ReadRest must be called.
- Do not rely on application globals or A5 (`N188`).
- Retro68 code addresses globals by absolute relocated addresses, not A5 (`libretro/relocate.c:230-300`). Globals are therefore usable at interrupt time if CODE is locked, which single-segment apps are [V/U].
- Calling a GCC C function after ReadRest is ABI-safe, since it saves D2–D7 and A2–A6.

RHA byte offsets from A2:

| Header | Field: offset |
|---|---|
| Both | lapDst 1, lapSrc 2, lapType 3 (1 = short, 2 = long) |
| Short | dstSkt 6, srcSkt 7, type 8 |
| Long | len 4, cksum 6, dstNet 8, srcNet 10, dstNode 12, srcNode 13, dstSkt 14, srcSkt 15, type 16 |

These match TalkTool and the `N189` equates.

Design: ReadRest straight into a free 600-byte ring slot, record the source address, type and length, and poll from the main loop. This is the same pattern as IM's free/used queues.

### NBP lookup (csCode lookupName 251; register 253, remove 252, kill 254)

Parameter block:

| Offset | Field |
|---|---|
| 28 | interval (byte, units of 8 ticks) |
| 29 | count (byte) |
| 30 | entityPtr |
| 34 | retBuffPtr |
| 38 | retBuffSize (word) |
| 40 | maxToGet (word) |
| 42 | numGotten (word) |

- The entity is *packed*: `01 '=' 09 "IPGATEWAY" 01 '*'`.
- The reply buffer holds packed tuples: net(2), node(1), socket(1), enumerator(1), then the packed object, type and zone strings (`N83`, `N65`).
- For registering a name, the NTE layout is qLink(4) + AddrBlock(4) + data (108 bytes total, `N69`, `N74`). The exact enumerator and name offsets are [U]. It is optional anyway.

### ATP (PSendRequest, csCode 255)

| Offset | Field |
|---|---|
| 18 | userData |
| 28 | currBitMap |
| 29 | atpFlags |
| 30 | addrBlock |
| 34 | reqLength |
| 36 | reqPointer |
| 40 | bdsPointer |
| 44 | numOfBuffs |
| 45 | timeOutVal (seconds) |
| 46 | numOfResps |
| 47 | retryCount |

BDS entries are 12 bytes: bufSz(2), addr(4), dataSz(2), userBytes(4) (`N178`).

**But** PSendRequest sends from an ATP-assigned dynamic socket (`N160`). macipgw would then map your IP to that socket until your first type-22 packet. **Recommendation:** send the 12-byte TReq yourself with writeDDP (type 3, from socket 72) and parse the TResp in the listener. This avoids .ATP completely.

## 3. Prior art

**uIP 1.0:**
- BSD 3-clause. Code is "a few KB"; RAM can be a few hundred bytes. `uip.c` is 1897 lines.
- Big-endian requires `UIP_CONF_BYTE_ORDER=UIP_BIG_ENDIAN`; the default is little-endian (`uipopt.h:472-476`).
- Set `UIP_CONF_LLH_LEN 0` (the default is 14, `:446-448`) and drop `uip_arp`.
- Set `UIP_CONF_BUFFER_SIZE 586`, giving MSS 546 (`:305,379`).
- `uip_buf` is `u8_t[]` (`uip.h:422`). **Force 2-byte alignment**, because the 68000 faults on odd word access.
- It keeps one unacknowledged segment per connection and has the application regenerate data for retransmits. Window defaults to the MSS (`:317`). This fits ClassicStack's proxy well.
- `UIP_UDP` is off by default; enable it for DNS.

**ElWhip (`lwip-contrib-mac`):**
- lwIP raw API, `NO_SYS`, PPP over the serial port.
- Built with MPW 3.5 and MPW 3.1 libraries (not Retro68). Runs on a 64K-ROM Mac 128K.
- `MEM_SIZE` 2 KB, `TCP_MSS` 536, `TCP_WND` 2×MSS (`ports/mac/include/lwipopts.h`).
- No AppleTalk. Only the main-loop and timer glue (`sys_arch.c`, `clock()`) is reusable. Files use CR line endings.

**DDP listeners:**
- IM:N Listings 7-1..7-7 (`N189`), plus NW13's corrected version.
- Apple TalkTool `DDPListener.a` (minimal and directly adaptable).
- None are in Retro68 samples or netatalk.

**PeerTalk:** its AppleTalk transport is only a design note (`specs/009-appletalk-transport/research.md`). It has no code.

**Mini vMac LToUDP:** emulator-side only.

**NCSA Telnet 2.x:** shipped its own stack with KIP/MacIP support [U: the 2.6.1d1 source is reportedly on Applefritter, but I have not checked it].

**Spec:** draft-ietf-appleip-MacIP-02 (Evans and Ranch, 1992). The behaviour of both gateways matches it, apart from the response length and field meanings noted above.
