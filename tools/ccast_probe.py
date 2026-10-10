"""Chromecast CASTV2 probe: LOAD an arbitrary URL from the PC.

Two purposes:
1. Serve a local 48 kHz stereo WAV sine stream and LOAD it, verifying the
   CASTV2 conversation independently of the ESP32 board.
2. LOAD the board's relay URL (http://<board-ip>:8080/stream) with the same
   sender flow to test the board's HTTP serving path.

Usage:
  python ccast_probe.py --local            # serve sine on this PC and cast it
  python ccast_probe.py --url http://192.168.31.x:8080/stream
"""

import argparse
import math
import socket
import ssl
import struct
import threading
import time

NS_CONNECTION = "urn:x-cast:com.google.cast.tp.connection"
NS_RECEIVER = "urn:x-cast:com.google.cast.receiver"
NS_MEDIA = "urn:x-cast:com.google.cast.media"
NS_HEARTBEAT = "urn:x-cast:com.google.cast.tp.heartbeat"
DMR_APP = "CC1AD845"


def pb_str(field: int, value: str) -> bytes:
    data = value.encode()
    return bytes([field << 3 | 2]) + varint(len(data)) + data


def varint(n: int) -> bytes:
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def cast_message(src: str, dst: str, ns: str, payload: str) -> bytes:
    body = (
        bytes([0x08, 0x00])            # protocol_version: CASTV2_1_0
        + pb_str(2, src)
        + pb_str(3, dst)
        + pb_str(4, ns)
        + bytes([0x28, 0x00])          # payload_type: STRING
        + pb_str(6, payload)
    )
    return struct.pack(">I", len(body)) + body


class CastChannel:
    def __init__(self, ip: str):
        raw = socket.create_connection((ip, 8009), timeout=10)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        self.sock = ctx.wrap_socket(raw)
        self.inbox = b""

    def send(self, src, dst, ns, payload):
        self.sock.sendall(cast_message(src, dst, ns, payload))

    def recv(self, timeout=1.0):
        self.sock.settimeout(timeout)
        while True:
            if len(self.inbox) >= 4:
                (length,) = struct.unpack(">I", self.inbox[:4])
                if len(self.inbox) >= 4 + length:
                    body = self.inbox[4 : 4 + length]
                    self.inbox = self.inbox[4 + length :]
                    return parse_message(body)
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, ssl.SSLError):
                return None
            if not chunk:
                raise ConnectionError("cast socket closed")
            self.inbox += chunk


def parse_message(body: bytes) -> dict:
    fields = {}
    i = 0
    while i < len(body):
        tag = body[i]
        i += 1
        field = tag >> 3
        wire = tag & 7
        if wire == 0:
            value = 0
            while True:
                b = body[i]
                i += 1
                value = (value << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            fields[field] = value
        elif wire == 2:
            (n,) = struct.unpack(">I", b"\0" + body[i : i + 3]) if False else (None,)
            # length-delimited: read varint length
            n = 0
            while True:
                b = body[i]
                i += 1
                n = (n << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            fields[field] = body[i : i + n]
            i += n
    out = {"src": "", "dst": "", "ns": "", "payload": ""}
    if 2 in fields:
        out["src"] = fields[2].decode(errors="replace")
    if 3 in fields:
        out["dst"] = fields[3].decode(errors="replace")
    if 4 in fields:
        out["ns"] = fields[4].decode(errors="replace")
    if 6 in fields:
        out["payload"] = fields[6].decode(errors="replace")
    return out


class SineServer(threading.Thread):
    HEADERS = (
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/wav\r\n"
        "Content-Length: 4294967339\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Accept-Ranges: none\r\n"
        "X-Audio-Sample-Rate: 48000\r\n"
        "X-Audio-Channels: 2\r\n"
        "X-Audio-Byte-Rate: 192000\r\n"
        "\r\n"
    ).encode()

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.clients = []

    def wav_header(self):
        return (
            b"RIFF" + struct.pack("<I", 0xFFFFFFFF) + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16)
            + b"data"
            + struct.pack("<I", 0xFFFFFFFF)
        )

    def run(self):
        second = b"".join(
            struct.pack("<hh", int(12000 * math.sin(2 * math.pi * 440 * n / 48000)),
                        int(12000 * math.sin(2 * math.pi * 440 * n / 48000)))
            for n in range(48000)
        )
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self.port))
        srv.listen(4)
        print(f"sine server listening on :{self.port}")

        def serve(conn):
            conn.settimeout(15)
            try:
                req = b""
                while b"\r\n\r\n" not in req:
                    part = conn.recv(1024)
                    if not part:
                        return
                    req += part
                print(f"sine server request: {req.splitlines()[0].decode(errors='replace')}")
                conn.sendall(self.HEADERS + self.wav_header())
                conn.settimeout(None)
                chunk = second[:19200]  # 100 ms
                while True:
                    conn.sendall(chunk)
                    time.sleep(0.1)
            except OSError as exc:
                print(f"sine client ended: {exc}")
            finally:
                conn.close()

        while True:
            conn, addr = srv.accept()
            print(f"sine server client {addr}")
            threading.Thread(target=serve, args=(conn,), daemon=True).start()


def local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("192.168.31.1", 9))
    ip = s.getsockname()[0]
    s.close()
    return ip


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.31.109")
    ap.add_argument("--url")
    ap.add_argument("--local", action="store_true")
    ap.add_argument("--port", type=int, default=8081)
    args = ap.parse_args()

    url = args.url
    if args.local:
        SineServer(args.port).start()
        url = f"http://{local_ip()}:{args.port}/stream"
    if not url:
        ap.error("--url or --local required")
    print(f"LOAD url: {url}")

    ch = CastChannel(args.ip)
    ch.send("sender-0", "receiver-0", NS_CONNECTION, '{"type":"CONNECT","userAgent":"ccast-probe"}')

    app = None
    ch.send("sender-0", "receiver-0", NS_RECEIVER, '{"type":"GET_STATUS","requestId":1}')
    deadline = time.time() + 8
    while time.time() < deadline and not app:
        msg = ch.recv(1)
        if not msg:
            continue
        if msg["ns"] != NS_RECEIVER:
            continue
        if '"CC1AD845"' in msg["payload"] or "transportId" in msg["payload"]:
            import json
            data = json.loads(msg["payload"])
            for entry in data.get("status", {}).get("applications", []):
                if entry.get("appId") in (DMR_APP, "E8C28D3C") or "transportId" in entry:
                    app = entry
                    break
            if not app and "applications" in data.get("status", {}):
                apps = data["status"]["applications"]
                if apps:
                    app = apps[0]

    if not app:
        print("DMR not running; launching")
        ch.send("sender-0", "receiver-0", NS_RECEIVER, f'{{"type":"LAUNCH","requestId":2,"appId":"{DMR_APP}"}}')
        deadline = time.time() + 12
        while time.time() < deadline and not app:
            msg = ch.recv(1)
            if not msg or msg["ns"] != NS_RECEIVER:
                continue
            if "transportId" in msg["payload"]:
                import json
                data = json.loads(msg["payload"])
                for entry in data.get("status", {}).get("applications", []):
                    if entry.get("appId") == DMR_APP:
                        app = entry
                        break

    if not app:
        print("ERROR: no DMR app session")
        return 1
    print(f"DMR app: {app}")

    transport = app["transportId"]
    ch.send("sender-0", transport, NS_CONNECTION, '{"type":"CONNECT","userAgent":"ccast-probe"}')

    import json
    load = {
        "type": "LOAD",
        "requestId": 3,
        "sessionId": app.get("sessionId", ""),
        "autoplay": True,
        "currentTime": 0,
        "media": {
            "contentId": url,
            "streamType": "LIVE",
            "contentType": "audio/wav",
            "metadata": {"metadataType": 0, "title": "ccast probe"},
        },
        "customData": {},
    }
    ch.send("sender-0", transport, NS_MEDIA, json.dumps(load))
    print("LOAD sent; watching MEDIA_STATUS for 40 s")
    deadline = time.time() + 40
    nudged = False
    while time.time() < deadline:
        msg = ch.recv(1)
        if not msg:
            continue
        if msg["ns"] == NS_HEARTBEAT:
            if "PING" in msg["payload"]:
                ch.send("sender-0", msg["src"], NS_HEARTBEAT, '{"type":"PONG"}')
            continue
        if msg["ns"] == NS_MEDIA:
            print(f"MEDIA: {msg['payload'][:300]}")
            if not nudged and "mediaSessionId" in msg["payload"]:
                time.sleep(1)
                data = json.loads(msg["payload"])
                mid = None
                for st in data.get("status", []):
                    mid = st.get("mediaSessionId")
                if mid is not None:
                    nudged = True
                    ch.send("sender-0", transport, NS_MEDIA,
                            json.dumps({"type": "PLAY", "requestId": 4, "mediaSessionId": mid}))
                    print(f"sent PLAY nudge mid={mid}")
            if '"playerState":"PLAYING"' in msg["payload"]:
                print("PLAYING confirmed")
                time.sleep(2)
                return 0
    print("no PLAYING within 40 s")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
