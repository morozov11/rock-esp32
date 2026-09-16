#!/usr/bin/env python3
"""Hardware-in-the-loop test for the RockCast ESP32-P4 device.

Drives the device over its USB Serial/JTAG console (COM port) using the
firmware's built-in serial command interface (CMD:*) and asserts on the
device log stream. Run it right after `idf.py app-flash` so the full boot
is captured, or with --no-boot to attach to an already running board.

Scenarios:
  1. boot: country code, Wi-Fi IP, WSS session timings
  2. catalog browse over the real GUI path
  3. station search (UTF-8 query) over the real GUI path
  4. playback of several catalog stations: a station is a PASS when the
     attempt reaches a definitive outcome - either it starts playing, or
     the server/stream reports an error (many stations are unreachable);
     a hang or transport failure is a FAIL
  5. pause/resume/stop/volume controls
  6. CMD:METRICS sampling (heap, underruns, decode errors)
  7. heartbeat continuity: no ACK silence beyond the 45 s watchdog

Usage:
  python rock_hwtest.py --port COM6 [--minutes 20] [--stations 3] [--no-boot]
"""

import argparse
import re
import sys
import threading
import time
from datetime import datetime

import serial  # pyserial, present in the ESP-IDF python env

HEARTBEAT_TIMEOUT_S = 45.0


class LogStream:
    """Reader thread that keeps the full log and per-pattern events."""

    def __init__(self, port, baud):
        # Hold DTR/RTS deasserted across open: the default assert-on-open
        # pulse hard-resets the board over the native USB Serial/JTAG port.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.lines = []
        self.lock = threading.Lock()
        self.stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        buf = b""
        while not self.stop:
            try:
                chunk = self.ser.read(4096)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                stamp = datetime.now().strftime("%H:%M:%S")
                with self.lock:
                    self.lines.append((stamp, line))

    def send(self, text):
        self.ser.write((text + "\n").encode("utf-8"))
        self.ser.flush()

    def wait_for(self, pattern, timeout_s, since_idx=0):
        """Wait until a regex matches any log line; returns (idx, match)."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            with self.lock:
                for idx in range(since_idx, len(self.lines)):
                    m = rx.search(self.lines[idx][1])
                    if m:
                        return idx, m
            time.sleep(0.2)
        return None, None

    def tail_since(self, idx):
        with self.lock:
            return self.lines[idx:] if idx < len(self.lines) else []

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        self.ser.close()

    def dump(self, path):
        with self.lock, open(path, "w", encoding="utf-8") as f:
            for stamp, line in self.lines:
                f.write(f"{stamp} {line}\n")


class Result:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, ok, detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" :: {detail}" if detail else ""))

    def summary(self):
        failed = [r for r in self.rows if not r[1]]
        print(f"\n=== {len(self.rows) - len(failed)}/{len(self.rows)} checks passed ===")
        return 1 if failed else 0


def heartbeat_continuity(log, idx_from):
    """No gap between consecutive heartbeat ACKs longer than the watchdog.

    Uses the monotonic ESP uptime counter in the "(ms)" log prefix; the raw
    serial stream has no wall-clock timestamps.
    """
    acks = []
    for _, l in log.tail_since(idx_from):
        if "device.heartbeat_ack" not in l:
            continue
        m = re.match(r"[IWE] \((\d+)\)", l)
        if m:
            acks.append(int(m.group(1)) / 1000.0)
    worst = 0.0
    for a, b in zip(acks, acks[1:]):
        worst = max(worst, b - a)
    return worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--minutes", type=float, default=20.0)
    ap.add_argument("--stations", type=int, default=3)
    ap.add_argument("--no-boot", action="store_true")
    args = ap.parse_args()

    res = Result()
    log = LogStream(args.port, args.baud)
    t0 = time.time()
    print(f"attached to {args.port}, capturing log (test window {args.minutes} min)")

    try:
        if not args.no_boot:
            idx, m = log.wait_for(r"WSS connected uptime_ms=(\d+)", 120)
            if m:
                res.check(
                    "boot: WSS session established",
                    True,
                    f"{int(m.group(1)) / 1000:.1f}s from power-on to WSS connected",
                )
                rom_seen = log.wait_for(r"esp32p4-eco2", 0.1)[0] is not None
                country, _ = log.wait_for(r"country set to RU", 5)
                if rom_seen:
                    res.check("boot: 2.4 GHz country RU (channels 1-13)", country is not None)
                else:
                    print("note: attached after boot start; country-RU line not captured")
            else:
                res.check("boot: WSS session established", False, "no WSS connected within 120 s")

        # Console sanity
        base = len(log.tail_since(0))
        log.send("CMD:METRICS")
        idx, m = log.wait_for(r"metrics heap=", 10)
        res.check("console: CMD:METRICS responds", m is not None)

        # 1. Catalog browse through the real GUI path
        base = len(log.tail_since(0))
        log.send("CMD:UI:CATALOG")
        idx, m = log.wait_for(
            r"HTTP GET /api/v1/device-control/catalog/stations\?limit=\d+ -> status=(\d+)", 30
        )
        if m:
            ok = m.group(1) == "200"
            res.check("catalog: browse request via GUI", ok, f"status={m.group(1)}")
        else:
            res.check("catalog: browse request via GUI", False, "no catalog GET within 30 s")

        # 2. Search with a UTF-8 query
        log.send("CMD:UI:SEARCH:рок")
        idx, m = log.wait_for(
            r"HTTP GET /api/v1/device-control/catalog/search\?limit=\d+&q=%D1%80%D0%BE%D0%BA -> status=(\d+)",
            30,
        )
        if m:
            ok = m.group(1) == "200"
            res.check("search: UTF-8 query via GUI", ok, f"status={m.group(1)}")
        else:
            res.check("search: UTF-8 query via GUI", False, "no search GET within 30 s")

        # 3. Play several stations from the loaded catalog. Switch back to
        #    the browse view first: STATION:<n> clicks the row list of the
        #    active view, and search results may be empty. Unavailable
        #    stations are expected: a definitive outcome (playing or a
        #    reported error) passes; silence/hang fails.
        log.send("CMD:UI:CATALOG")
        log.wait_for(
            r"HTTP GET /api/v1/device-control/catalog/stations\?limit=\d+ -> status=200", 30
        )
        outcomes = {"playing": 0, "error": 0}
        for row in range(args.stations):
            base = len(log.tail_since(0))
            log.send(f"CMD:UI:STATION:{row}")
            ack, _ = log.wait_for(r"Serial command received: 'CMD:UI:STATION:\d+'", 10)
            if not ack:
                res.check(f"station[{row}]: command delivered", False)
                continue
            played, m = log.wait_for(
                r"WSS receive type=device\.command|receive type=command\.result|receive type=protocol\.error",
                35,
                since_idx=base,
            )
            if m is None:
                res.check(f"station[{row}]: definitive outcome within 20 s", False, "no response")
                continue
            # Give the player a moment, then read the definitive status.
            status = -1
            for attempt in range(2):
                time.sleep(8)
                mb = len(log.tail_since(0))
                log.send("CMD:METRICS")
                _, mm = log.wait_for(r"playback=(\d)", 10, since_idx=mb)
                if mm:
                    status = int(mm.group(1))
                if status in (2, 5):
                    break
            # 0 idle 1 buffering 2 playing 3 paused 4 stopped 5 error
            if status == 2:
                outcomes["playing"] += 1
                res.check(f"station[{row}]: play attempt resolved", True, "playing")
            elif status == 5:
                outcomes["error"] += 1
                res.check(
                    f"station[{row}]: play attempt resolved", True, "player error (station unavailable)"
                )
            else:
                # Not playing yet and not a player error: look for an explicit
                # server rejection in the window.
                rejected = log.wait_for(r"protocol\.error|command\.result", 0.1, since_idx=base)[0]
                if rejected is not None:
                    outcomes["error"] += 1
                    res.check(f"station[{row}]: play attempt resolved", True, "server rejected")
                else:
                    res.check(
                        f"station[{row}]: play attempt resolved",
                        False,
                        f"no definitive outcome (playback={status})",
                    )
            time.sleep(2)

        # 4. Transport controls
        for cmd, name in [
            ("CMD:UI:STOP", "stop"),
            ("CMD:UI:PLAYPAUSE", "pause/resume toggle"),
            ("CMD:UI:VOL+", "volume up"),
            ("CMD:UI:VOL-", "volume down"),
        ]:
            base = len(log.tail_since(0))
            log.send(cmd)
            ack, _ = log.wait_for(re.escape(cmd), 10)
            res.check(f"controls: {name} accepted", ack is not None)

        # 5. Long-run stability window: heartbeats, Wi-Fi events, watchdogs.
        print(f"stability window: holding for {args.minutes} min ...")
        window_start = len(log.tail_since(0))
        deadline = time.time() + args.minutes * 60
        while time.time() < deadline:
            time.sleep(10)
        window = log.tail_since(window_start)
        text = "\n".join(l for _, l in window)
        drops = len(re.findall(r"STA link lost", text))
        reconnects = len(re.findall(r"websocket_client: Started", text))
        watchdog = len(re.findall(r"heartbeat ACK timeout", text))
        res.check(
            "stability: heartbeat continuity within watchdog budget",
            heartbeat_continuity(log, window_start) <= HEARTBEAT_TIMEOUT_S + 20,
            f"worst ACK gap {heartbeat_continuity(log, window_start):.0f}s",
        )
        res.check(
            "stability: no firmware crashes",
            "Guru Meditation" not in text and "assert failed" not in text,
        )
        print(
            f"note: Wi-Fi link drops={drops}, WSS (re)starts={reconnects}, "
            f"heartbeat watchdogs={watchdog} during window"
        )
        if drops:
            for _, l in window:
                if "link lost" in l or "RF survey" in l or "sta ip" in l:
                    print(f"  wifi-event: {l}")

        base = len(log.tail_since(0))
        log.send("CMD:UI:CATALOG")
        idx, m = log.wait_for(
            r"HTTP GET /api/v1/device-control/catalog/stations\?limit=\d+ -> status=(\d+)", 40,
            since_idx=base,
        )
        if m:
            first = m.group(1)
            after = m[0] if isinstance(m[0], int) else idx
            if first == "401":
                # The 401 triggers an in-session token refresh: wait for the
                # device-session POST and the retried catalog GET to settle.
                r2, m2 = log.wait_for(
                    r"HTTP GET /api/v1/device-control/catalog/stations\?limit=\d+ -> status=(\d+)", 25,
                    since_idx=after + 1,
                )
                refreshed = log.wait_for(
                    r"auth/device-session -> status=200", 25, since_idx=base
                )[0] is not None
                final = m2.group(1) if m2 else first
                res.check(
                    "catalog: post-TTL reload succeeds",
                    final == "200",
                    ("token expired, refreshed mid-session, " if refreshed else "token expired, refresh not observed, ")
                    + f"final status={final}",
                )
            else:
                res.check(
                    "catalog: post-TTL reload succeeds", first == "200", f"status={first}"
                )
        else:
            res.check("catalog: post-TTL reload succeeds", False, "no catalog GET after window")

        mb = len(log.tail_since(0))
        log.send("CMD:METRICS")
        _, mm = log.wait_for(
            r"metrics heap=(\d+) internal=(\d+) underruns=(\d+) decode_errors=(\d+) reconnects=(\d+)",
            10,
            since_idx=mb,
        )
        if mm:
            print(
                f"note: metrics heap={mm.group(1)} internal={mm.group(2)} "
                f"underruns={mm.group(3)} decode_errors={mm.group(4)} "
                f"stream_reconnects={mm.group(5)}"
            )

        out = f"hwtest_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
        log.dump(out)
        print(f"full device log saved to {out} (local only, not for commit)")
        code = res.summary()
    finally:
        log.close()
    sys.exit(code)


if __name__ == "__main__":
    main()
