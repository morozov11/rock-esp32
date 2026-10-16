"""Follow-up hardware checks: mDNS during playback (3.1) and boot warmup line.

1. Starts a station, opens the Cast menu mid-playback (real discovery runs
   with or without receivers) and watches for ESP_MP3_DEC errors plus the
   new discovery heap logs.
2. Resets the board via the USB-JTAG RTS line and captures the boot log,
   asserting 'mDNS warmed up before playback' appears right after Wi-Fi.

Usage: python tools/uc_audio_boot_check.py [port] [log_path]
"""
import re
import sys
import time
import datetime

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
LOG = sys.argv[2] if len(sys.argv) > 2 else "telemetry/uc_audio_boot_check.log"
TS = re.compile(rb"I \((\d+)\)")


def open_port(reset=False):
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    if reset:  # USB-JTAG: pulsing RTS resets the chip into normal boot
        ser.rts = True
        time.sleep(0.2)
        ser.rts = False
    return ser


def run():
    log = open(LOG, "w", encoding="utf-8", errors="replace")
    t0 = time.time()
    state = {"mp3": 0, "warm": False, "heap": 0, "preroll": False, "cast_menu": False}

    def pump(ser, secs):
        deadline = time.time() + secs
        while time.time() < deadline:
            for line in ser.read(4096).split(b"\n"):
                clean = line.rstrip(b"\r")
                if not clean:
                    continue
                now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                entry = f"[{now} +{time.time()-t0:7.2f}s] ".encode() + clean
                log.write(entry.decode("utf-8", "replace") + "\n")
                print(entry.decode("utf-8", "replace"), flush=True)
                if b"ESP_MP3_DEC" in clean:
                    state["mp3"] += 1
                if b"mDNS warmed up before playback" in clean:
                    state["warm"] = True
                if b"mDNS discovery heap" in clean:
                    state["heap"] += 1
                if b"Pre-roll reached" in clean:
                    state["preroll"] = True
                if b"cast" in clean.lower() and b"menu" in clean.lower():
                    state["cast_menu"] = True
            time.sleep(0.05)
        log.flush()

    # --- check 1: cast menu during playback ---
    ser = open_port()
    print("=== CHECK 1: cast discovery during playback ===", flush=True)
    ser.write(b"CMD:UI:STATION:0\n"); ser.flush()
    pump(ser, 12)  # wait for playback
    if not state["preroll"]:
        print("!! playback did not start; continuing anyway", flush=True)
    mp3_before = state["mp3"]
    print(">> opening Cast menu (discovery runs)", flush=True)
    ser.write(b"CMD:UI:CAST\n"); ser.flush()
    pump(ser, 12)  # discovery window + margin
    print(f"CHECK1: preroll={state['preroll']} mp3_err_during_discovery="
          f"{state['mp3'] - mp3_before} heap_logs={state['heap']}", flush=True)
    ser.write(b"CMD:UI:CAST:LOCAL\n"); ser.flush()
    pump(ser, 3)
    ser.close()

    # --- check 2: boot log with mdns warmup ---
    print("\n=== CHECK 2: reboot, boot log ===", flush=True)
    state.update(mp3=0, preroll=False)
    ser = open_port(reset=True)
    pump(ser, 75)  # boot + wifi + mdns warmup window
    print(f"CHECK2: mdns_warmed={'YES' if state['warm'] else 'NO'}", flush=True)
    ser.close()
    log.close()


if __name__ == "__main__":
    run()
