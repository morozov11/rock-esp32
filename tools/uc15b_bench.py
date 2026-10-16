"""1.5b bench scenario: 5 station switches + Stop->Play; measures 'Upstream connect' lines.

Usage: python tools/uc15b_bench.py [port] [log_path]
"""
import re
import sys
import time
import datetime

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
LOG = sys.argv[2] if len(sys.argv) > 2 else "telemetry/uc15b_bench.log"
TS = re.compile(rb"I \((\d+)\)")
CONNECT = re.compile(rb"Upstream connect: (\d+) ms, reused_handle=(\d)")


def main():
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = PORT, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    log = open(LOG, "w", encoding="utf-8", errors="replace")
    t0 = time.time()
    buf = b""

    def pump(secs, collect=None):
        nonlocal buf
        deadline = time.time() + secs
        while time.time() < deadline:
            data = ser.read(4096)
            if data:
                text = buf + data
                lines = text.split(b"\n")
                for line in lines[:-1]:
                    clean = line.rstrip(b"\r")
                    now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    log.write(f"[{now} +{time.time()-t0:7.2f}s] " + clean.decode("utf-8", "replace") + "\n")
                    print((f"[{now}] " + clean.decode('utf-8', 'replace'))[:160], flush=True)
                    if collect is not None:
                        m = CONNECT.search(clean)
                        if m:
                            collect.append((int(m.group(1)), int(m.group(2))))
                buf = lines[-1]
            time.sleep(0.05)
        log.flush()

    def send(cmd):
        print(f">> {cmd}", flush=True)
        ser.write(cmd.encode() + b"\n")
        ser.flush()

    print("waiting for session (heartbeats)...", flush=True)
    connects = []
    # boot: wait for welcome/registration or heartbeat activity
    pump(45)

    # Catalog is not auto-loaded at boot: open it via the real button so the
    # STATION:<row> triggers have visible rows to click.
    send("CMD:UI:CATALOG")
    pump(10, connects)

    for i, row in enumerate([0, 1, 2, 3, 0]):
        send(f"CMD:UI:STATION:{row}")
        pump(12, connects)  # connect + preroll
        print(f"-- switch {i+1}/5 done", flush=True)

    # Stop -> Play cycle
    send("CMD:UI:STATION:1")
    pump(12, connects)
    send("CMD:UI:STOP")
    pump(3, connects)
    send("CMD:UI:PLAYPAUSE")
    pump(10, connects)
    send("CMD:METRICS")
    pump(3)

    print("\n=== Upstream connect samples (ms, reused_handle) ===", flush=True)
    for ms, reused in connects:
        print(f"  {ms:5d} ms  reused={reused}", flush=True)
    log.close()


if __name__ == "__main__":
    main()
