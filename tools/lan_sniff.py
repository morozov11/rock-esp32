"""Capture IP packets to/from a host on the local LAN (Windows, admin).

Writes JSON lines: ts, dir (rx/tx), proto, sport, dport, tcp_flags, seq, ack,
payload_len and a hex prefix of the payload (enough to read HTTP headers).
"""

import json
import socket
import struct
import sys
import time

SIO_RCVALL = 0x98000001

CC_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.31.109"
OUT = sys.argv[2] if len(sys.argv) > 2 else "sniff.jsonl"
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0
BIND_IP = sys.argv[4] if len(sys.argv) > 4 else "192.168.31.133"
MAX_HEX = 512

FLAG_NAMES = "FSRPAUEC"


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_IP)
    s.bind((BIND_IP, 0))
    s.ioctl(SIO_RCVALL, 1)
    s.settimeout(0.5)
    print(f"sniffing {CC_IP} via {BIND_IP} -> {OUT} for {DURATION}s", flush=True)
    out = open(OUT, "w", encoding="utf-8")
    count = 0
    deadline = time.time() + DURATION
    while time.time() < deadline:
        try:
            pkt = s.recvfrom(65535)[0]
        except socket.timeout:
            continue
        ihl = (pkt[0] & 0x0F) * 4
        proto = pkt[9]
        src = socket.inet_ntoa(pkt[12:16])
        dst = socket.inet_ntoa(pkt[16:20])
        if CC_IP not in (src, dst):
            continue
        direction = "rx" if src == CC_IP else "tx"
        rec = {
            "ts": round(time.time(), 3),
            "dir": direction,
            "proto": proto,
            "src": src,
            "dst": dst,
        }
        if proto == 6:  # TCP
            tcp = pkt[ihl:]
            if len(tcp) < 20:
                continue
            sport, dport, seq, ack = struct.unpack(">HHII", tcp[:12])
            off = (tcp[12] >> 4) * 4
            flags = tcp[13]
            rec.update(
                sport=sport,
                dport=dport,
                seq=seq,
                ack=ack,
                flags="".join(FLAG_NAMES[i] for i in range(8) if flags & (1 << i)),
                win=struct.unpack(">H", tcp[14:16])[0],
            )
            payload = tcp[off:]
            rec["payload_len"] = len(payload)
            # Include the TCP header + options (up to 96 bytes) so handshake
            # option bytes are analyzable offline.
            rec["hex"] = tcp[:off + 96].hex() if len(tcp) > off else tcp.hex()
            if len(tcp) - off > 96:
                rec["hex"] += "..."
                rec["truncated"] = True
        elif proto == 17:  # UDP (mDNS etc.)
            udp = pkt[ihl:]
            if len(udp) < 8:
                continue
            sport, dport, length = struct.unpack(">HHH", udp[:6])
            rec.update(sport=sport, dport=dport, payload_len=max(0, length - 8))
        else:
            rec["payload_len"] = len(pkt) - ihl
        out.write(json.dumps(rec) + "\n")
        count += 1
        if count % 2000 == 0:
            out.flush()
            print(f"  {count} packets...", flush=True)
    out.close()
    print(f"done: {count} packets -> {OUT}", flush=True)


if __name__ == "__main__":
    main()
