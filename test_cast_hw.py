import serial
import time
import sys
import threading
import re
import json
import urllib.request

# Default station: SomaFM Metal Detector (MP3 passthrough, stable upstream;
# the 181.FM server had RTT >1.3 s and TCP resets from this network).
STATION_ID = 'somafm-metal-detector'

class SerialTester:
    def __init__(self, port='COM6', baudrate=115200):
        if hasattr(sys.stdout, 'reconfigure'):
            sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        self.port = port
        self.baudrate = baudrate
        self.lines = []
        self.lock = threading.Lock()
        self.stop = False
        self.ser = None
        self.thread = None
        self.sdio_errors = 0
        self.stream_uri = None

    def start(self):
        print(f"Connecting to {self.port} with dtr=False, rts=False...")
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baudrate
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        buf = b""
        while not self.stop:
            try:
                chunk = self.ser.read(1024)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if '"station.play_stream"' in line and 'WSS rx: ' in line:
                    try:
                        message = json.loads(line.split('WSS rx: ', 1)[1])
                        self.stream_uri = message['payload']['body']['stream_uri']
                    except (ValueError, KeyError):
                        pass
                # Never expose an upstream stream URI in the hardware-test log.
                line = re.sub(r"https?://[^\s'\"]+", "<redacted-uri>", line)
                if 'WSS tx: ' in line or 'WSS rx: ' in line:
                    pass
                elif "eh_sdio: sdio_get_tx_buffer_num: err: 263" in line or "sdmmc_send_cmd returned 0x107" in line:
                    self.sdio_errors += 1
                    if self.sdio_errors <= 4 or self.sdio_errors % 1000 == 0:
                        sys.stdout.write(f"{line} (SDIO errors: {self.sdio_errors})\n")
                        sys.stdout.flush()
                else:
                    sys.stdout.write(line + "\n")
                    sys.stdout.flush()
                with self.lock:
                    self.lines.append(line)

    def send(self, cmd):
        print(f"\n---> SENDING: {cmd}")
        self.ser.write((cmd + "\n").encode("utf-8"))
        self.ser.flush()

    def wait_for(self, pattern, timeout_s, since_idx=0):
        rx = re.compile(pattern)
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            with self.lock:
                for idx in range(since_idx, len(self.lines)):
                    m = rx.search(self.lines[idx])
                    if m:
                        return idx, m
            time.sleep(0.1)
        return None, None

    def current_line_idx(self):
        with self.lock:
            return len(self.lines)

    def close(self):
        self.stop = True
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.ser and self.ser.is_open:
            self.ser.close()

def main():
    tester = SerialTester('COM6', 115200)
    tester.start()

    try:
        print("\nWaiting for device to be online and registered on RockServer WSS (up to 120s)...")
        idx, _ = tester.wait_for(r"(device\.registered|device\.heartbeat_ack)", timeout_s=120)
        if idx is None:
            print("ERROR: Device not registered on WSS within 120s, aborting test.")
            return False

        # Clear a previous Cast relay before catalog search: the active
        # upstream/downstream pair can saturate this board's hosted Wi-Fi.
        tester.send("CMD:UI:CAST:LOCAL")
        time.sleep(3)
        tester.send("CMD:UI:STOP")
        time.sleep(2)

        print(f"\n---> [STEP 1] Starting {STATION_ID} on the speaker...")
        since = tester.current_line_idx()
        tester.send(f"CMD:UI:STATIONID:{STATION_ID}")
        idx, _ = tester.wait_for(rf"Serial trigger station id='{STATION_ID}'", timeout_s=10, since_idx=since)
        if idx is None:
            print(f"ERROR: {STATION_ID} selection failed, aborting test.")
            return False
        # Unified source: one authorized proxy fetch feeds both outputs.
        idx, _ = tester.wait_for(r"rock_relay: Upstream HTTP status 200 OK", timeout_s=40, since_idx=since)
        if idx is None:
            print("ERROR: stream source failed to connect upstream, aborting test.")
            return False
        idx, _ = tester.wait_for(r"rock-player: Pre-roll reached \(\d+ bytes\); playback started",
                                 timeout_s=40, since_idx=since)
        if idx is None:
            print("ERROR: station failed to start on the speaker, aborting test.")
            return False

        print("\n---> [STEP 2] Triggering Cast Discovery (CMD:UI:CAST)...")
        since = tester.current_line_idx()
        tester.send("CMD:UI:CAST")
        idx, _ = tester.wait_for(r"Cast discovery total: [1-9]\d* devices", timeout_s=15, since_idx=since)
        if idx is None:
            print("ERROR: Cast discovery finished without finding a device, aborting test.")
            return False
        print("Cast discovery found at least one device.")
        time.sleep(1)

        print("\n---> [STEP 3] Selecting Chromecast device 0 (CMD:UI:CAST:SELECT:0)...")
        since = tester.current_line_idx()
        tester.send("CMD:UI:CAST:SELECT:0")
        idx, _ = tester.wait_for(
            r"TLS connected to Chromecast at 192\.168\.31\.109:8009",
            timeout_s=20,
            since_idx=since,
        )
        if idx is None:
            print("ERROR: TLS connection to Chromecast failed, aborting test.")
            return False
        print("Chromecast TLS connection established.")

        print("\n---> [STEP 4] Checking automatic speaker-to-Chromecast handoff...")
        since = tester.current_line_idx()
        checks = (
            (r"Sending Cast LOAD req=\d+", "Cast LOAD sent"),
            (r"rock_relay: Relay client connected from 192\.168\.31\.109:", "Chromecast connected to relay"),
            (r"rock_relay: Starting relay streaming \(content-type: audio/(wav|mpeg)", "Relay streaming started"),
            (r"Cast LOAD ready!", "Chromecast accepted the live stream"),
            (r"rock_relay: Relay streamed \d+ bytes to Chromecast", "Relay streamed audio bytes to Chromecast"),
        )
        for pattern, description in checks:
            idx, _ = tester.wait_for(pattern, timeout_s=90, since_idx=since)
            if idx is None:
                print(f"ERROR: Timed out waiting for {description}, aborting test.")
                return False
            print(f"Confirmed: {description}.")

        idx, _ = tester.wait_for(r"Relay streamed (?:[3-9]\d{5}|\d{7,}) bytes to Chromecast",
                                 timeout_s=75, since_idx=since)
        if idx is None:
            print("ERROR: Chromecast stopped consuming the audio stream, aborting test.")
            return False
        print("Confirmed: Chromecast consumed at least 300 KB of audio.")

        print("\n---> [STEP 5] Checking Metrics (CMD:METRICS)...")
        tester.send("CMD:METRICS")
        time.sleep(3)

        print("\n---> [STEP 6] Capturing Screen (CMD:SCREENSHOT)...")
        since = tester.current_line_idx()
        tester.send("CMD:SCREENSHOT")
        idx, m = tester.wait_for(r"===SCREENSHOT_START:(.*?)===", timeout_s=15, since_idx=since)
        if idx is not None:
            raw_meta = m.group(1)
            meta = dict(part.split("=", 1) for part in raw_meta.split(","))
            idx_end, _ = tester.wait_for(r"===SCREENSHOT_END===", timeout_s=15, since_idx=idx)
            if idx_end is not None:
                lines = [tester.lines[i].strip() for i in range(idx + 1, idx_end) if tester.lines[i].strip()]
                import base64
                from PIL import Image
                raw = base64.b64decode("".join(lines))
                w, h = int(meta["w"]), int(meta["h"])
                if meta["cf"] == "RGB565_RLE":
                    data = bytearray()
                    for i in range(0, len(raw), 3):
                        data.extend(raw[i+1:i+3] * raw[i])
                    raw = bytes(data)
                Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16").save("screen.png")
                print("Confirmed: Screen saved to screen.png.")

        print("\nTest finished.")
        return True
    finally:
        tester.close()
        if tester.stream_uri:
            try:
                request = urllib.request.Request(tester.stream_uri, headers={
                    'User-Agent': 'RockCast/0.1', 'Accept': '*/*', 'Icy-MetaData': '1',
                })
                with urllib.request.urlopen(request, timeout=20) as response:
                    sample = response.read(65536)
                    print(f"Upstream PC probe: status={response.status} content-type={response.headers.get('Content-Type')} icy-metaint={response.headers.get('icy-metaint')} length={response.headers.get('Content-Length')} encoding={response.headers.get('Transfer-Encoding')} bytes={len(sample)} prefix={sample[:64].hex(' ')}")
                    sync = [i for i in range(len(sample) - 1) if sample[i] == 0xff and sample[i + 1] & 0xe0 == 0xe0]
                    ts_sync = max((sum(sample[offset + 188 * n] == 0x47 for n in range((len(sample) - offset + 187) // 188)) for offset in range(188)), default=0)
                    print(f"Upstream PC signatures: MPEG/ADTS candidates={len(sync)} first={sync[:12]} TS best sync count={ts_sync} OggS={sample.find(b'OggS')} ID3={sample.find(b'ID3')} RIFF={sample.find(b'RIFF')}")
            except Exception as exc:
                print(f"Upstream PC probe failed: {type(exc).__name__}")

if __name__ == '__main__':
    sys.exit(0 if main() else 1)
