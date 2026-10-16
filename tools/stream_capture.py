"""Stream raw ESP32 console output with timestamps directly to file and stdout."""
import datetime
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
out_path = sys.argv[2] if len(sys.argv) > 2 else "telemetry/live_gui_capture.log"

print(f"Opening {port} for continuous live capture into {out_path}...", flush=True)

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.5
ser.dtr = False
ser.rts = False

try:
    ser.open()
except Exception as e:
    print(f"Failed to open {port}: {e}", file=sys.stderr, flush=True)
    sys.exit(1)

t0 = time.time()
print(f"Listening on {port} since {datetime.datetime.now().strftime('%H:%M:%S')}. Press Ctrl+C or kill task to stop.", flush=True)

with open(out_path, "w", encoding="utf-8", errors="replace") as out_f:
    out_f.write(f"=== Capture started at {datetime.datetime.now().isoformat()} ===\n")
    out_f.flush()
    try:
        remainder = ""
        while True:
            raw = ser.read(2048)
            if raw:
                text = remainder + raw.decode("utf-8", errors="replace")
                lines = text.split("\n")
                remainder = lines.pop()  # Keep uncompleted line in remainder
                
                now_str = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                elapsed = time.time() - t0
                for line in lines:
                    line_clean = line.rstrip("\r")
                    formatted = f"[{now_str} +{elapsed:6.2f}s] {line_clean}\n"
                    out_f.write(formatted)
                    sys.stdout.write(formatted)
                out_f.flush()
                sys.stdout.flush()
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        if remainder:
            now_str = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            formatted = f"[{now_str}] {remainder.rstrip()}\n"
            out_f.write(formatted)
            sys.stdout.write(formatted)
        ser.close()
        out_f.write(f"=== Capture ended at {datetime.datetime.now().isoformat()} ===\n")
        out_f.flush()
        print("Capture closed.", flush=True)
