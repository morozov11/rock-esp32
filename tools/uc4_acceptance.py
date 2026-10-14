"""Unified Catalog UC-4 acceptance test script."""
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

    def drain(seconds: float, verbose=True):
        end = time.monotonic() + seconds
        buf = []
        while time.monotonic() < end:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if line:
                if verbose:
                    print(f"  [DEVICE] {line}")
                buf.append(line)
        return buf

    def send_cmd(cmd: str):
        print(f"\n>>> SEND: {cmd}")
        ser.write((cmd + "\n").encode())
        ser.flush()

    def wait_for_pattern(pattern: str, timeout=30):
        print(f"Waiting for pattern: '{pattern}' (timeout {timeout}s)...")
        deadline = time.monotonic() + timeout
        buf = []
        while time.monotonic() < deadline:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if line:
                print(f"  [DEVICE] {line}")
                buf.append(line)
                if pattern in line:
                    print(f"--> Found pattern: '{pattern}'")
                    return True, buf
        return False, buf

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
                except ValueError as exc:
                    print(f"  [WARN] Interleaved data: {text[:80]}")
                    continue
                lines.append(text)
        if not (meta and finished):
            print("  [ERROR] Incomplete screenshot stream")
            return False
        raw = base64.b64decode("".join(lines), validate=True)
        w, h = int(meta["w"]), int(meta["h"])
        if meta["cf"] == "RGB565_RLE":
            if len(raw) % 3:
                print("  [ERROR] Truncated RLE triplet")
                return False
            data = bytearray()
            for i in range(0, len(raw), 3):
                data.extend(raw[i+1:i+3] * raw[i])
            raw = bytes(data)
        elif meta["cf"] != "RGB565":
            print(f"  [ERROR] Unsupported format {meta['cf']}")
            return False
        if len(raw) != w * h * 2:
            print(f"  [ERROR] Incorrect size {len(raw)} != {w*h*2}")
            return False
        Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16").save(out_png)
        print(f"  [SUCCESS] Saved {out_png}: {w}x{h}, {len(raw)} decoded bytes")
        return True

    try:
        # Check if already booted or booting
        print("Checking device connection state...")
        found, _ = wait_for_pattern("directory.snapshot", timeout=25)
        if not found:
            print("Did not see directory.snapshot within timeout, checking metrics...")
            send_cmd("CMD:METRICS")
            drain(2.0)

        # Baseline metrics
        print("\n--- BASELINE METRICS ---")
        send_cmd("CMD:METRICS")
        drain(2.0)

        # 1. Open catalog (Page 1: 1..20)
        print("\n--- STEP 1: Open catalog (Page 1) ---")
        send_cmd("CMD:UI:CATALOG")
        drain(7.0)
        send_cmd("CMD:METRICS")
        drain(2.0)
        take_screenshot("docs/uc4-page1.png")

        # 2. Load more (Page 2: 21..40)
        print("\n--- STEP 2: Load more (Page 2: 21..40) ---")
        send_cmd("CMD:UI:LOADMORE")
        drain(7.0)
        send_cmd("CMD:METRICS")
        drain(2.0)
        take_screenshot("docs/uc4-page2.png")

        # 3. Load more (Page 3: 41..41 - End of list)
        print("\n--- STEP 3: Load more (Page 3: 41..41) ---")
        send_cmd("CMD:UI:LOADMORE")
        drain(7.0)
        send_cmd("CMD:METRICS")
        drain(2.0)
        take_screenshot("docs/uc4-page3.png")

        # 4. Repeated LOADMORE (dedup / end of list)
        print("\n--- STEP 4: Repeated LOADMORE when exhausted ---")
        send_cmd("CMD:UI:LOADMORE")
        drain(1.0)
        send_cmd("CMD:UI:LOADMORE")
        drain(2.0)

        # 5. Final metrics check
        print("\n--- STEP 5: Final metrics & heap comparison ---")
        send_cmd("CMD:METRICS")
        drain(2.0)

        print("\n=== ACCEPTANCE TEST COMPLETED ===")

    finally:
        ser.close()

if __name__ == "__main__":
    main()
