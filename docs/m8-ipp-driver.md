# M8: a MacTCP-compatible .ipp driver (prototype)

2026-10-16. Approach: prototype first, a minimal `.ipp` driver in Snow with its memory cost measured, then the real
Mac.

**Result:** it works, in Snow and on the real 512Ke. On the real Mac, unmodified MacTCP builds of Geomys and
Flynn ran through the driver, with no talkip code in them:
- Geomys browsed gopher.floodgap.com: menu, a document, Back;
- Flynn telnetted to telehack.com.

In Snow, stock Shiori Classic and Retro68's LaunchAPPL server run on it too.

The driver keeps 18.1 KB of code and 7.3 KB of data resident in the system heap: 24.8 KB.
- It was 26.7 KB as first built, and 23.4 KB after trimming.
- Asynchronous calls, passive open and Gestalt then added 1.4 KB.

On the real 512Ke (with a lean System 6.0.8 boot disk) the application heap loses 18-19K. In Snow it lost 5-7K, because that system heap had
room to spare.

## What it is

`src/ipp/ipp.c`, built as the INIT **TalkIP Driver** (`TalkIPDriver.bin`, type INIT, creator tkip) for the
System Folder:

- **The INIT** runs once at boot. Its resource is `sysheap, locked`. Retro68 relocates it in place, and the
  globals go to the system heap, since the code is there. It then finds a free unit-table slot (it took unit 95,
  refNum −96, in Snow) and makes a DCE whose `dCtlDriver` points at a driver header inside the same code. Last,
  it detaches its own resource so it stays after its file closes. A `sysz` resource asks INIT31 for 28 KB of
  system heap.
- **The driver** is named `.ipp`, like MacTCP, so `OpenDriver(".ipp")` finds it. Control calls go to tcpshim, the
  same MacTCP-semantics layer that Geomys Lite, Flynn Lite and Shiori Lite link in:
  - TCPCreate, ActiveOpen, Send, Rcv (honouring commandTimeoutValue), Close, Abort, Status, Release;
  - UDPCreate, Write, Read, BfrReturn, Release, MaxMTUSize;
  - ipctlGetAddr (waits up to 20 s for the lease).

  Open starts the stack (socket 72, NBP lookup, ASSIGN). `accRun`, every 6 ticks while applications call
  SystemTask, runs the stack between calls (macipgw's keepalive pings). Close returns closErr, so the driver
  stays open, as MacTCP's does.
- **Stack changes**, all under `TALKIP_RESIDENT`: no ExitToShell patch (the driver is resident; nothing to clean up
  when an app quits), and no SystemTask inside the stack's wait loops (it could re-enter the driver).

## Measurements (Snow, Early512Ke, System 6.0.8, 512 KB)

| | Without the driver | With the driver |
|---|---|---|
| About the Finder: System | 256K | 264K |
| About the Finder: Finder | 256K | 248K |
| TalkIP Status: free in its heap | 214K | 207K |
| After mounting an AppleShare volume | (not measured) | System bar full; mount OK |

Driver size (`m68k-apple-macos-size`): text 17,052, data 268, bss 8,664 bytes. The largest pieces:
- the DDP ring, 8 × 598 bytes;
- TCP control blocks, 2.2 KB;
- tcp_input 1.4 KB and ipp_control 0.8 KB;
- Retro68's relocator and multiseg init, 1.3 KB together, which stay resident although only used once.

The 7-8 KB cost is what this configuration saw, because its system heap had about 20 KB spare. A Mac whose system
heap is full at boot pays up to the full 28 KB that `sysz` asks for. The real 512Ke showed System 224K and 288K
for applications on 2026-10-15, a different split from Snow's. Its cost needs measuring there.

For comparison, each talkip Lite application carries its own copy of the stack. That's about 12-15 KB of code,
the same 8-slot ring, and buffers, and it's freed when the app quits. With the driver, a MacTCP application
carries none of it, but the 26 KB stays.

## Checks run

1. **IPP Probe** (`tests/m8/ipp_probe.c`, MacTCP calls only):
   - OpenDriver `.ipp` returned 0, refNum −96;
   - ipctlGetAddr gave 192.168.72.2, mask 255.255.255.0;
   - UDP: a DNS query to the LAN's DNS server was answered (57 bytes);
   - TCP: gopher floodgap.com:70 returned 5,639 bytes in 2.7 s, ending with connectionClosing (−23005, the
     server's close), then Close 0 and Release 0.
2. **Stock Geomys 8358881, minimal preset** (Geomys main, unmodified, built for MacTCP):
   - launched with no "MacTCP" error;
   - Directory › Floodgap Systems: "Done – 92 items";
   - the "Does this gopher menu look correct?" document (1,080 bytes); Back; Quit; the Finder was fine.
3. The same again **after mounting an AppleShare volume** (AppleShare 3.6 / 2.0.2) with the driver installed.

Rig: `tests/m8/run.sh up|down` (Xvfb :78, Snow with an HD20 System 6.0.8 disk, LToUDP on the local AppleTalk
network through TashRouter to macipgw). Driving: xdotool with screen coordinates; `tests/m8/m8drive.py` for typing.
As in M7, Return in a text field is dropped on the bare Xvfb, so Geomys was driven from its Directory menu.

## Trimmed (2026-10-16)

| | Before | After |
|---|---|---|
| Code + data kept (the INIT's handle) | 17,997 B (including the spent relocation table) | 16,264 B |
| Globals (system heap) | 8,664 B | 7,128 B |
| **Resident** | **26.7 KB** | **23.4 KB** (−3.3 KB) |
| About the Finder, System (Snow; 256K without the driver) | 264K | 261K |

- **Own relocation loop.** Retro68's format, inlined into `ipp_relocate`, replaces `RETRO68_RELOCATE`. That macro
  linked Retro68's general relocator and its multisegment runtime: 1.7 KB, dead after boot.
- **Spent relocation table freed.** After relocating, the handle is shrunk to `_edata`: the table after the data
  isn't needed again (0.7 KB).
- **One-segment TCP send buffer** in the driver (`TCP_SNDBUF` = MSS under `TALKIP_RESIDENT`). Only one segment is
  ever in flight (2 × 478 B).
- **UDP built in place.** Sends are gathered straight into the IP output buffer (`udp_payload()`) instead of a
  558-byte staging copy, in the Lite apps too.

Retested in Snow after the changes: IPP Probe gave the same results as before (ipctlGetAddr, UDP DNS, TCP 5,639
bytes).

## Lite apps and the driver together

The driver's header carries a signature, `TkIP` at offset 24. When a talkip Lite app finds socket 72 taken by the
TalkIP Driver, it opens socket 73 instead and leases its own address; the gateway answers whichever socket asked.
Before this, the app took socket 72 over and the driver lost it until reboot. The driver does the same in reverse
if a Lite app holds 72 when the driver opens.

In Snow:
- the driver had 192.168.72.3 on socket 72;
- TalkIP Status leased 192.168.72.4 on socket 73 and answered the gateway's ping;
- after Status quit, IPP Probe still worked through the driver on .3.

## On the real 512Ke (2026-10-16)

Boot disk: a lean System 6.0.8 HD20 image ("lean608") on a Floppy Emu. The final driver build (async, passive open,
Gestalt) was copied from an AppleShare volume into the System Folder, then the Mac was restarted.

| | Without the driver | With the driver |
|---|---|---|
| About the Finder: System | 186K | 205K |
| About the Finder: Finder | 326K | 307K |
| TalkIP Status: free | 281K (largest 279K) | 263K (largest 260K) |

The real cost is **18-19K**: the lean disk's system heap has no spare room, so the driver's 23.7 KB (minus what INIT31
already had) comes out of the application heap. (For comparison, a fuller System Folder gave 224K/288K and 253K free
without the driver.)

- **IPP Probe:** Gestalt 'mtcp' 0x02068000; `.ipp` refNum −48; ipctlGetAddr 192.168.72.3; UDP DNS answered;
  TCP floodgap 5,639 bytes in 1.8 s; async: 5,639 bytes in 10 receives (2.1 s), completion routine 11 times,
  TCPStatus during a pending receive 6 of 6, async close 8 ticks.
- **Stock Geomys** (minimal preset, MacTCP build): gopher.floodgap.com "Done – 92 items", a document (1,080
  bytes), Back.
- **Stock Flynn** (minimal + finger, MacTCP build): telnet to telehack.com, banner and `cowsay moo`.
- **Geomys Lite alongside the driver:** floodgap, 92 items (its own socket and lease).
- Afterwards the driver went back to "Optional (512K)" (not loaded at boot), and the Mac was shut down and powered
  off.

## LaunchAPPL over TCP, and the crash readout (2026-10-19)

With the gateway firewall allowing the build host to port 1984, Retro68's LaunchAPPL pushes and runs an
application on the real 512Ke over MacIP (`LaunchAPPL -e tcp --tcp-address <Mac's MacIP address>`). On System 6
without MultiFinder this is push-and-run only: the server sublaunches the application, and when it quits the server
is launched again from scratch and forgets the connection, so the build host never gets the result back. Results
come back over a serial connection, or as a file the application writes (for example into a shared folder).

- **The relaunch bomb:** the server sublaunches with a TCPRcv still pending. With the 10-16 driver, the driver
  later completed that call into the next application's heap and jumped through a stale ioCompletion
  ("coprocessor not installed", found in Snow). Fixed (dab825d): the driver chains onto IAZNotify, which
  InitApplZone calls at every launch and quit, and drops the calls and streams that live in the application's
  memory (ApplZone up to BufPtr).
- **An unexplained bomb:** that build alone bombed twice on the real Mac with "illegal instruction" when the
  server switched to MacTCP, and never in Snow (not even with RAM filled at power-on, which the ROM overwrites
  anyway). With the crash readout below built in, the same step passed twice on the real Mac, so no PC was
  captured. Cause unknown: something that depends on code layout or on timing (the SCC, MacIP traffic).
- **The crash readout** (src/ipp/trapcatch.c, on by default): the INIT also takes the bus error, address error,
  illegal instruction and Line-F vectors, draws the vector, PC, PC minus the driver's base, registers and stack in
  hex on the bottom 64 rows of the screen, then goes on to the system's handler. If the driver ever bombs, a photo
  of the screen gives the PC; `m68k-apple-macos-nm TalkIPDriver.flt.gdb` maps PC minus base to a function. It costs
  1.3 KB.
- **Verified on the real 512Ke** with the readout build: LaunchAPPL server → MacTCP, a push of a test application
  that ran and wrote its file, the server back with no bomb; stock Geomys (MacTCP build) loading floodgap, quitting,
  relaunching and loading again.

## Other stock MacTCP apps (Snow)

- **Shiori Classic, full build** (shiori 915f4ff, SIZE 1024K/640K; System 6 without MultiFinder ignores SIZE):
  search and a page against a test server. The first try failed with "Can't reach Hister (-23005)": async calls
  that finished at once returned their result from PBControlAsync; MacTCP returns noErr and reports only in
  ioResult. Fixed. The "small" build (same code, SIZE 320K) showed 90K free at launch.
- **Retro68's LaunchAPPL server** (MacTCP build): needed TCPPassiveOpen (added to talkip's TCP) and Gestalt
  'mtcp' (System 6 has no NewGestalt, so the INIT patches the Gestalt trap for that selector). It listens on port
  1984 and accepts connections (a connection and data from the gateway host arrived). A remote launch from the build
  host needs a route from the LAN into the MacIP subnet, which this setup's gateway firewall doesn't allow.
- **NCSA Telnet 2.3.SF** (1989) bombs with "illegal instruction" at startup on this System 6.0.8, with or without
  the driver; 2.7b5 is a 615K fat binary, too big. Not a driver issue.

## Prototype limits

- ~~Every call completes before the Control call returns.~~ Asynchronous calls now stay pending and complete from
  accRun (e568f7f, 720fcfb). They complete when the application calls SystemTask/WaitNextEvent, not at interrupt time.
- **Not implemented:** ASRs (notifyProc), TCPNoCopyRcv/RcvBfrReturn, ICMP echo
  (ipctlEchoICMP), and MacTCP's DNR. The resolver is code in the MacTCP control panel, not the driver, so apps
  that call OpenResolver need a separate 'dnrp' resolver.
- **Two TCP streams, two UDP streams** (talkip's limits).
- **Two addresses per Mac.** A Lite app and the driver each use their own lease.

## Next steps, if it goes ahead

1. ~~Measure on the real 512Ke~~: done (above).
2. ~~Real asynchronous calls~~: done.
3. ~~Trim 2-4 KB~~: done, 3.3 KB (above).
4. ~~LaunchAPPL remote launches need a network path from the build host to the MacIP subnet~~: done (above),
   push-and-run only on System 6.
5. More stock apps: a newer NCSA Telnet (2.5/2.6), Fetch-style tools; MacTCP's DNR ('dnrp') for apps that resolve
   names through it.
