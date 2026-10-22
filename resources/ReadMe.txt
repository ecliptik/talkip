talkip @VERSION@

TCP/IP for a Macintosh 512Ke over LocalTalk, without MacTCP.

The Mac sends IP inside AppleTalk (MacIP) to a MacIP gateway on the network, such as netatalk's macipgw or ClassicStack, which passes it on to the internet. AppleTalk must be on (Chooser).

Needs: a Macintosh with the 128K ROM or later (512Ke, Plus, ...), System 6, LocalTalk, and a MacIP gateway on the same AppleTalk network.

THE TOOLS

TalkIP Status - leases an address from the gateway, answers ping, shows counters and free memory.
TalkIP Ping - ping a host name or IP address.
TalkIP Lookup - DNS lookups through the gateway's nameserver.
TalkIP Fetch - a Gopher/TCP fetch, a test tool.
TalkIP Web - a very basic web browser (HTTP/1.0; HTTPS sites through a proxy such as macproxy).

Each tool has its own copy of the network stack, so nothing needs installing. Run one at a time.

TALKIP DRIVER

An INIT that lets applications built for MacTCP (for example the regular Geomys and Flynn editions) use talkip instead. Drag it into the System Folder and restart. It takes about 19K of memory. Don't install it on a Mac that has MacTCP.

If the Mac crashes while it is installed, it draws the crash address and the stack at the bottom of the screen before the bomb box. A photo of that helps a bug report.

APPLETALK ON

An INIT that turns AppleTalk on at every startup, for Macs whose PRAM battery is dead or missing (they forget the Chooser setting). Drag it into the System Folder and restart.

More: https://github.com/ecliptik/talkip
License: ISC
