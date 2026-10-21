"""Long-running ESP32 console and network monitor (no firmware changes)."""

import argparse
import datetime as dt
import json
import re
import subprocess
import threading
import time
import urllib.request
from pathlib import Path

import serial
from serial.tools import list_ports


USB_VID_PID = (0x303A, 0x1001)
IP_RE = re.compile(r"sta ip: (\d+\.\d+\.\d+\.\d+)")


def stamp():
    return dt.datetime.now(dt.timezone.utc).astimezone().isoformat(timespec="milliseconds")


class Logs:
    def __init__(self, directory):
        self.directory = directory
        self.lock = threading.Lock()
        self.files = {}

    def write(self, stream, message):
        day = dt.datetime.now().strftime("%Y-%m-%d")
        with self.lock:
            current = self.files.get(stream)
            if current is None or current[0] != day:
                if current is not None:
                    current[1].close()
                path = self.directory / f"{stream}-{day}.log"
                current = (day, path.open("a", encoding="utf-8", buffering=1))
                self.files[stream] = current
            current[1].write(f"{stamp()} {message}\n")

    def close(self):
        with self.lock:
            for _, file in self.files.values():
                file.close()


def find_port(requested):
    if requested != "auto":
        return requested
    matches = [port.device for port in list_ports.comports()
               if (port.vid, port.pid) == USB_VID_PID]
    return matches[0] if len(matches) == 1 else None


def ping(address):
    try:
        result = subprocess.run(
            ["ping", "-n", "1", "-w", "1500", address],
            capture_output=True, timeout=3, check=False,
        )
        return "ok" if result.returncode == 0 else f"fail exit={result.returncode}"
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"error {type(exc).__name__}"


def probe_health(url):
    start = time.monotonic()
    try:
        with urllib.request.urlopen(url, timeout=6) as response:
            response.read(128)
            status = response.status
        return f"http={status} ms={round((time.monotonic() - start) * 1000)}"
    except Exception as exc:
        return f"error={type(exc).__name__} ms={round((time.monotonic() - start) * 1000)}"


def network_probe(logs, stop, args, board_ip):
    while not stop.is_set():
        start = time.monotonic()
        gateway = ping(args.gateway) if args.gateway else "disabled"
        ip = board_ip[0]
        board = ping(ip) if ip else "unknown"
        health = probe_health(args.health_url)
        logs.write("network", f"gateway={gateway} board_ip={ip or '-'} board={board} server={health}")
        stop.wait(max(0, args.probe_interval - (time.monotonic() - start)))


def write_status(directory, port, board_ip, last_serial_at=None, last_metrics_at=None):
    status = {"updated_at": stamp(), "port": port, "board_ip": board_ip,
              "last_serial_at": last_serial_at, "last_metrics_at": last_metrics_at}
    temporary = directory / "status.tmp"
    temporary.write_text(json.dumps(status, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(directory / "status.json")


def run(args):
    directory = args.output or Path("telemetry") / "long-monitor" / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "session.json").write_text(json.dumps({
        "started_at": stamp(), "requested_port": args.port,
        "gateway": args.gateway, "board_ip": args.board_ip, "health_url": args.health_url,
        "metrics_interval_sec": args.metrics_interval,
        "probe_interval_sec": args.probe_interval,
    }, indent=2) + "\n", encoding="utf-8")
    logs = Logs(directory)
    stop = threading.Event()
    board_ip = [args.board_ip]
    last_serial_at = None
    last_metrics_at = None
    probe = threading.Thread(target=network_probe, args=(logs, stop, args, board_ip), daemon=True)
    probe.start()
    deadline = time.monotonic() + args.duration_hours * 3600 if args.duration_hours else None
    last_port_state = None
    try:
        while not stop.is_set() and (deadline is None or time.monotonic() < deadline):
            port = find_port(args.port)
            if port is None:
                if last_port_state != "absent":
                    logs.write("events", "USB serial port absent or ambiguous")
                    last_port_state = "absent"
                write_status(directory, None, board_ip[0], last_serial_at, last_metrics_at)
                stop.wait(3)
                continue
            try:
                device = serial.Serial()
                device.port = port
                device.baudrate = 115200
                device.timeout = 0.5
                device.dtr = False
                device.rts = False
                device.open()
                logs.write("events", f"serial opened port={port} dtr=0 rts=0")
                last_port_state = "open"
                pending = bytearray()
                next_metrics = time.monotonic() + 2 if args.metrics_interval else float("inf")
                next_status = 0
                awaiting_metrics = None
                try:
                    while not stop.is_set() and (deadline is None or time.monotonic() < deadline):
                        now = time.monotonic()
                        if now >= next_metrics:
                            if awaiting_metrics is not None:
                                logs.write("events", f"metrics response missing for {round(now - awaiting_metrics)}s")
                            device.write(b"CMD:METRICS\n")
                            logs.write("events", "sent CMD:METRICS")
                            awaiting_metrics = now
                            next_metrics = now + args.metrics_interval
                        if now >= next_status:
                            write_status(directory, port, board_ip[0], last_serial_at, last_metrics_at)
                            next_status = now + 30
                        pending.extend(device.read(4096))
                        while b"\n" in pending:
                            line, _, rest = pending.partition(b"\n")
                            pending = bytearray(rest)
                            decoded = line.rstrip(b"\r").decode("utf-8", errors="replace")
                            logs.write("serial", decoded)
                            last_serial_at = stamp()
                            if "rock-serial: metrics heap=" in decoded:
                                last_metrics_at = last_serial_at
                                awaiting_metrics = None
                            match = IP_RE.search(decoded)
                            if match:
                                board_ip[0] = match.group(1)
                        if len(pending) > 16384:
                            logs.write("serial", pending.decode("utf-8", errors="replace"))
                            pending.clear()
                finally:
                    if pending:
                        logs.write("serial", pending.decode("utf-8", errors="replace"))
                    device.close()
                    logs.write("events", f"serial closed port={port}")
            except (OSError, serial.SerialException) as exc:
                logs.write("events", f"serial error port={port} type={type(exc).__name__} detail={exc}")
                last_port_state = "error"
                stop.wait(3)
    except KeyboardInterrupt:
        logs.write("events", "stopped by keyboard")
    finally:
        stop.set()
        probe.join(timeout=8)
        write_status(directory, None, board_ip[0], last_serial_at, last_metrics_at)
        logs.write("events", "monitor stopped")
        logs.close()
    print(directory.resolve())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="auto", help="COM port or auto for one Espressif USB device")
    parser.add_argument("--output", type=Path, help="run directory (default: timestamped telemetry/long-monitor)")
    parser.add_argument("--gateway", default="192.168.31.1", help="router IP for PC-side ping; empty disables")
    parser.add_argument("--board-ip", help="known board IP; updated when a new IP appears in serial logs")
    parser.add_argument("--health-url", default="https://rockplatform.win/health/ready")
    parser.add_argument("--metrics-interval", type=float, default=30, help="seconds between CMD:METRICS; 0 disables commands")
    parser.add_argument("--probe-interval", type=float, default=30, help="seconds between PC-side network probes")
    parser.add_argument("--duration-hours", type=float, default=0, help="0 runs until Ctrl+C")
    options = parser.parse_args()
    if (options.metrics_interval != 0 and options.metrics_interval < 5) or options.probe_interval < 5 or options.duration_hours < 0:
        parser.error("intervals must be >= 5 seconds (or metrics 0) and duration must be >= 0")
    run(options)
