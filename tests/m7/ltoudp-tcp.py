#!/usr/bin/env python3
"""ltoudp-tcp.py: list the TCP segments inside a pcap of LToUDP (UDP 1954) traffic.

    tcpdump -i lo -w m7.pcap udp port 1954     (inside the talkipm7 namespace)
    ltoudp-tcp.py [--raw] m7.pcap

LToUDP payload: 4-byte sender id, LLAP (dst, src, type 1 short DDP / 2 long DDP), DDP, then the IP packet
(DDP type 22). Prints time, direction, flags, seq, ack, window and payload length, relative to each side's ISN.
"""
import struct
import sys


def packets(path):
    with open(path, "rb") as f:
        magic, = struct.unpack("<I", f.read(4))
        endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
        _, _, _, _, _, linktype = struct.unpack(endian + "HHiIII", f.read(20))
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            sec, frac, caplen, _ = struct.unpack(endian + "IIII", h)
            yield sec + frac / (1e9 if nano else 1e6), linktype, f.read(caplen)


def udp_payload(linktype, frame):
    if linktype == 1:      # Ethernet (lo is captured as Ethernet with zero MACs)
        ip = frame[14:]
    elif linktype == 113:  # Linux cooked
        ip = frame[16:]
    elif linktype == 276:  # Linux cooked v2
        ip = frame[20:]
    else:
        ip = frame
    ihl = (ip[0] & 15) * 4
    return ip[ihl + 8:]


def main(path, raw=False):
    isn = {}
    t0 = None
    for ts, lt, frame in packets(path):
        p = udp_payload(lt, frame)[4:]          # drop the LToUDP sender id
        if len(p) < 3:
            continue
        llap_type = p[2]
        if llap_type == 1:
            ddp, hdr = p[3:], 5
            ddptype = ddp[4]
        elif llap_type == 2:
            ddp, hdr = p[3:], 13
            ddptype = ddp[12]
        else:
            continue
        if ddptype != 22:
            continue
        ip = ddp[hdr:]
        if ip[9] != 6:
            continue
        ihl = (ip[0] & 15) * 4
        tcp = ip[ihl:]
        src = "%d.%d.%d.%d" % tuple(ip[12:16])
        sport, dport, seq, ack, off, flags, win = struct.unpack(">HHIIBBH", tcp[:16])
        plen = struct.unpack(">H", ip[2:4])[0] - ihl - (off >> 4) * 4
        mac = src.startswith("192.168.100.")
        side = "mac" if mac else "net"
        other = "net" if mac else "mac"
        if flags & 2 and not raw:
            isn[side] = seq
        if t0 is None:
            t0 = ts
        fl = "".join(c for c, b in zip("FSRPA", (1, 2, 4, 8, 16)) if flags & b)
        rs = (seq - isn.get(side, 0)) & 0xFFFFFFFF
        ra = (ack - isn.get(other, 0)) & 0xFFFFFFFF if flags & 16 else 0
        print("%8.3f %s->  %-5s seq %10d ack %10d win %5d len %4d" % (ts - t0, side, fl, rs, ra, win, plen))


if __name__ == "__main__":
    # --raw: absolute sequence numbers (for a capture that starts mid-connection, with no SYN)
    main(sys.argv[-1], raw="--raw" in sys.argv)
