# Changelog

## [0.1.0] - 2026-10-22

The first release: TCP/IP for a Macintosh 512Ke over LocalTalk, through a MacIP gateway, without MacTCP.

- TalkIP Status, Ping, Lookup, Fetch and Web: tools with the stack linked in. They lease an address from the
  gateway, answer ping, look up names and fetch over TCP (Gopher, HTTP/1.0, HTTPS through a proxy such as macproxy).
- TalkIP Driver: an INIT that installs a MacTCP-compatible `.ipp` driver, so applications built for MacTCP (the
  regular Geomys and Flynn editions) run on a Mac that has no room for MacTCP. On a crash it draws the PC and the
  stack at the bottom of the screen before the bomb box.
- AppleTalk On: an INIT that turns AppleTalk on at every startup, for Macs without a PRAM battery.
- Tested on a real Macintosh 512Ke with System 6.0.8 and AppleShare, through netatalk's `macipgw`.
