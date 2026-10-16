"""UC-Audio hardware acceptance driver (plan-gui-playback-refactoring, Etap 4).

Drives the board over the USB-JTAG serial console (CMD: protocol, see
components/rock_gui/rock_serial.c) and measures the plan's acceptance
criteria from the device's internal I(...) millisecond timestamps.

Phases: boot checks (3.1) -> UC-Audio-1 latency -> UC-Audio-3 stress taps
-> UC-Audio-5 soak (10 switches + 10 min playback, metrics before/after).

Usage: python tools/uc_audio_acceptance.py [port] [log_path]
"""
import re
import sys
import time
import datetime

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
LOG = sys.argv[2] if len(sys.argv) > 2 else "telemetry/uc_audio_acceptance.log"
TS = re.compile(rb"I \((\d+)\)")

MARKS = {
    "wifi": b"Wi-Fi connection established",
    "mdns_warm": b"mDNS warmed up before playback",
    "welcomed": b"protocol.welcome",
    "cmd_echo": b"Serial command received",
    "catalog_ok": b"catalog/stations?limit=20 -> status=200",
    "station_cmd": b"WSS receive type=device.command",
    "device_session": b"POST /api/v1/auth/device-session",
    "upstream": b"Upstream HTTP status 200 OK",
    "preroll": b"Pre-roll reached (131072",
    "ignored": b"Playback play ignored: station connecting",
    "cancelled": b"Station connecting cancelled by user stop",
    "play_skip": b"PlayStream skipped: pending launch was cancelled",
    "mp3_err": b"ESP_MP3_DEC",
    "metrics": b"metrics heap=",
    "invalid_row": b"Serial trigger station row",
}


class Driver:
    def __init__(self, port, log_path):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = False   # opening must not reset the board
        self.ser.rts = False
        self.ser.open()
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.buf = b""
        self.t0 = time.time()
        self.events = []  # (name, device_ms, wall_s)

    def pump(self):
        raw = self.ser.read(4096)
        if raw:
            text = self.buf + raw
            lines = text.split(b"\n")
            self.buf = lines.pop()
            now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for line in lines:
                clean = line.rstrip(b"\r")
                out = f"[{now} +{time.time()-self.t0:7.2f}s] ".encode() + clean
                self.log.write(out.decode("utf-8", "replace") + "\n")
                print(out.decode("utf-8", "replace"), flush=True)
                for name, pat in MARKS.items():
                    if pat in clean:
                        m = TS.search(clean)
                        dev_ms = int(m.group(1)) if m else -1
                        self.events.append((name, dev_ms, time.time() - self.t0))
        self.log.flush()

    def wait(self, name, timeout, quiet=False):
        deadline = time.time() + timeout
        seen = 0
        while time.time() < deadline:
            self.pump()
            n = sum(1 for e in self.events if e[0] == name)
            if n > seen:
                return self.events[-1]
            time.sleep(0.05)
        if not quiet:
            print(f"!! TIMEOUT waiting for '{name}' ({timeout}s)", flush=True)
        return None

    def send(self, cmd):
        print(f">> {cmd}", flush=True)
        self.ser.write(cmd.encode() + b"\n")
        self.ser.flush()

    def drain(self, secs):
        deadline = time.time() + secs
        while time.time() < deadline:
            self.pump()
            time.sleep(0.05)

    def dev_ms(self, name, occurrence=-1):
        hits = [e[1] for e in self.events if e[0] == name]
        return hits[occurrence] if hits else None

    def count(self, name):
        return sum(1 for e in self.events if e[0] == name)


def phase_boot(d):
    print("\n===== PHASE 0: boot checks (3.1 warmup, session) =====", flush=True)
    d.wait("wifi", 90)
    d.wait("mdns_warm", 15)
    d.wait("welcomed", 60)
    d.drain(2)
    print(f"boot: mdns_warm={'OK' if d.count('mdns_warm') else 'MISS'} "
          f"session={'OK' if d.count('welcomed') else 'MISS'}", flush=True)


def phase_latency(d, switch_no, row):
    print(f"\n===== UC-Audio-1 sample #{switch_no}: station row {row} =====", flush=True)
    before = len(d.events)
    d.send(f"CMD:UI:STATION:{row}")
    d.wait("preroll", 25)
    d.drain(1.5)
    a = d.dev_ms("cmd_echo", -1)
    # the click echo is followed by the trigger log; use station command arrival
    b = d.dev_ms("station_cmd")
    c = d.dev_ms("upstream")
    e = d.dev_ms("preroll")
    sess = sum(1 for x in d.events[before:] if x[0] == "device_session")
    if None not in (b, c, e):
        print(f"  result #{switch_no}: click->cmd={b-a if a else '?'}ms "
              f"cmd->upstream={c-b}ms upstream->preroll={e-c}ms "
              f"TOTAL(approx cmd->preroll)={e-b}ms device_session_posts={sess}",
              flush=True)
    else:
        print(f"  result #{switch_no}: INCOMPLETE b={b} c={c} e={e}", flush=True)
    return e - b if (b is not None and e is not None) else None


def phase_catalog(d):
    print("\n===== load catalog =====", flush=True)
    d.send("CMD:UI:CATALOG")
    d.wait("catalog_ok", 30)
    d.drain(3)


def phase_stress(d):
    print("\n===== UC-Audio-3: stress taps during connect =====", flush=True)
    # (a) spam PLAYPAUSE during the connect window -> must be ignored, stream survives
    d.send("CMD:UI:STATION:1")
    time.sleep(0.8)
    for _ in range(3):
        d.send("CMD:UI:PLAYPAUSE")
        time.sleep(0.35)
    d.wait("preroll", 25)
    d.drain(2)
    print(f"  (a) play-spam: ignored={d.count('ignored')} preroll_ok={'YES' if d.count('preroll')>=1 else 'NO'}",
          flush=True)
    # (b) STOP during connect -> clean cancel, no zombie stream
    d.send("CMD:UI:STATION:2")
    time.sleep(0.8)
    d.send("CMD:UI:STOP")
    d.drain(8)
    print(f"  (b) stop-cancel: cancelled={d.count('cancelled')} play_skip={d.count('play_skip')}",
          flush=True)
    d.send("CMD:METRICS")
    d.drain(3)


def phase_soak(d):
    print("\n===== UC-Audio-5: soak (10 switches + 10 min) =====", flush=True)
    d.send("CMD:METRICS")
    d.drain(2)
    rows = [0, 1, 2, 3, 0, 1, 2, 3, 0, 1]
    for i, row in enumerate(rows):
        d.send(f"CMD:UI:STATION:{row}")
        d.wait("preroll", 25, quiet=True)
        d.drain(6)
        print(f"  switch {i+1}/10 row={row} done "
              f"(mp3_err_total={d.count('mp3_err')})", flush=True)
    print("  10 switches done; soaking 600 s on current station...", flush=True)
    deadline = time.time() + 600
    while time.time() < deadline:
        d.pump()
        time.sleep(0.2)
    d.send("CMD:METRICS")
    d.drain(3)
    print(f"  soak done: mp3_err_total={d.count('mp3_err')} upstream={d.count('upstream')} "
          f"preroll={d.count('preroll')}", flush=True)


def main():
    d = Driver(PORT, LOG)
    print(f"capture -> {LOG}; port {PORT}", flush=True)
    phase_boot(d)
    phase_catalog(d)
    phase_latency(d, 1, 0)
    phase_latency(d, 2, 1)
    phase_stress(d)
    phase_soak(d)
    print("\n===== ALL PHASES COMPLETE =====", flush=True)
    d.drain(2)


if __name__ == "__main__":
    main()
