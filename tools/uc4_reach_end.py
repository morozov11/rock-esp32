"""Load until the end of the catalog and capture the final page with the pill."""
import base64
import sys
import time
import serial
from PIL import Image

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"

def main():
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    ser.dtr = ser.rts = False
    ser.port = PORT
    ser.open()
    print(f"Connected to {PORT}")

    def drain(seconds: float):
        end = time.monotonic() + seconds
        buf = []
        while time.monotonic() < end:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if line:
                print(f"  [DEVICE] {line}")
                buf.append(line)
        return buf

    def send_cmd(cmd: str):
        print(f"\n>>> SEND: {cmd}")
        ser.write((cmd + "\n").encode())
        ser.flush()

    def take_screenshot(out_png: str, timeout=25):
        print(f"\n>>> CAPTURING SCREENSHOT -> {out_png}")
        ser.reset_input_buffer()
        ser.write(b"CMD:SCREENSHOT\n")
        ser.flush()
        deadline = time.monotonic() + timeout
        meta, lines, finished = None, [], False
        while time.monotonic() < deadline:
            text = ser.readline().decode("ascii", errors="ignore").strip()
            if "===SCREENSHOT_START:" in text:
                raw = text.split("===SCREENSHOT_START:", 1)[1].split("===", 1)[0]
                meta = dict(part.split("=", 1) for part in raw.split(","))
                lines = []
            elif text == "===SCREENSHOT_END===":
                finished = True
                break
            elif meta and text:
                try:
                    base64.b64decode(text, validate=True)
                except ValueError:
                    continue
                lines.append(text)
        if not (meta and finished):
            print("  [ERROR] Incomplete screenshot stream")
            return False
        raw = base64.b64decode("".join(lines), validate=True)
        w, h = int(meta["w"]), int(meta["h"])
        if meta["cf"] == "RGB565_RLE":
            data = bytearray()
            for i in range(0, len(raw), 3):
                data.extend(raw[i+1:i+3] * raw[i])
            raw = bytes(data)
        Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16").save(out_png)
        print(f"  [SUCCESS] Saved {out_png}")
        return True

    try:
        # Loop LOADMORE until cursor is empty
        for i in range(10):
            send_cmd("CMD:UI:LOADMORE")
            out = drain(6.0)
            text = "\n".join(out)
            if "cursor=''" in text or "cursor empty" in text or "total 128" in text:
                print(f"Reached end of catalog at iteration {i}!")
                break
        
        drain(2.0)
        send_cmd("CMD:METRICS")
        drain(2.0)
        take_screenshot("docs/uc4-end.png")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
