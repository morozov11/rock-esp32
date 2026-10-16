"""RE-18 serial timing check. Records only sanitized timing/status lines.

Run with ESP-IDF Python: python tools/re18_acceptance.py COM6
"""

import re
import statistics
import sys
import time

import serial


PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
QUICK = "--quick" in sys.argv[2:]
SERIES = "--resumption-series" in sys.argv[2:]
WAIT_TOKEN = "--wait-token" in sys.argv[2:]
REST = re.compile(
    r"REST (GET|POST) status=(\d+) err=([A-Z_]+) len=(\d+) "
    r"stale_retry=(\d) reused_handle=(\d) connect_ms=(-?\d+) "
    r"wait_headers_ms=(-?\d+) total_ms=(\d+)"
)
PRESENTED = re.compile(r"catalog presented: mode=([a-z-]+) elapsed_ms=(\d+)")

ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = PORT, 115200, 0.2
ser.dtr = ser.rts = False
ser.open()


def lines_until(deadline):
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", "replace")
        if line:
            yield line


def request(label, command, timeout=30):
    ser.write((command + "\n").encode())
    ser.flush()
    get = None
    ack = False
    lines_seen = 0
    for line in lines_until(time.monotonic() + timeout):
        lines_seen += 1
        if "Serial command received:" in line:
            ack = True
        match = REST.search(line)
        if match and match.group(1) == "GET":
            get = tuple(match.groups())
            if match.group(2) == "401":
                print(f"{label}: GET 401 observed", flush=True)
        shown = PRESENTED.search(line)
        if shown:
            print(f"{label}: rest={get} mode={shown.group(1)} ui_ms={shown.group(2)}", flush=True)
            return get, int(shown.group(2))
    print(f"{label}: no catalog presentation; ack={ack} lines={lines_seen} last_rest={get}", flush=True)
    return get, None


try:
    print("Waiting for control session (up to 30 s)", flush=True)
    boot_lines = 0
    for line in lines_until(time.monotonic() + 30):
        boot_lines += 1
        match = REST.search(line)
        if match:
            print(f"boot REST: {match.groups()}", flush=True)
        if "WSS connected" in line:
            break
    print(f"Boot lines observed: {boot_lines}", flush=True)

    if WAIT_TOKEN:
        print("Waiting up to 12 min for scheduled REST POST", flush=True)
        refreshed = False
        for line in lines_until(time.monotonic() + 720):
            match = REST.search(line)
            if match and match.group(1) == "POST":
                print(f"Scheduled REST POST: {match.groups()}", flush=True)
                refreshed = match.group(2) == "200"
                break
        print(f"Token request observed: {refreshed}", flush=True)
        request("after-token", "CMD:UI:CATALOG")
        sys.exit(0)

    if SERIES:
        request("warmup", "CMD:UI:CATALOG")
        resumed = []
        for i in range(1, 6):
            idle_s = 300 if i == 1 else 90
            print(f"Idle {i}/5 for {idle_s} s", flush=True)
            for line in lines_until(time.monotonic() + idle_s):
                match = REST.search(line)
                if match and match.group(1) == "POST":
                    print(f"Idle REST POST: {match.groups()}", flush=True)
            rest, ui_ms = request(f"resumed-{i}", "CMD:UI:CATALOG")
            if rest and rest[1] == "200" and rest[5] == "1" and int(rest[6]) >= 0 and ui_ms is not None:
                resumed.append(ui_ms)
        print(f"Resumed UI samples: {resumed}; median: {statistics.median(resumed) if resumed else 'n/a'} ms", flush=True)
        sys.exit(0)

    request("first", "CMD:UI:CATALOG")
    repeat = [request(f"repeat-{i}", "CMD:UI:CATALOG") for i in range(1, 6)]
    request("search", "CMD:UI:SEARCH:rock")
    request("browse", "CMD:UI:CATALOG")
    request("pagination", "CMD:UI:LOADMORE", timeout=15)

    if not QUICK:
        print("Idling for 300 s", flush=True)
        until = time.monotonic() + 300
        last = 0
        for line in lines_until(until):
            remaining = int(until - time.monotonic())
            minute = remaining // 60
            if minute != last:
                last = minute
                print(f"Idle remaining about {remaining} s", flush=True)
        request("after-idle", "CMD:UI:CATALOG")

    values = [item[1] for item in repeat if item[1] is not None]
    if values:
        print(f"Repeat UI median: {statistics.median(values)} ms", flush=True)
finally:
    ser.close()
