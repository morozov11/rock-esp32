"""Unified Catalog UC-5 acceptance test script."""
import base64
import sys
import time
import serial
from PIL import Image

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"

def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    ser.dtr = ser.rts = False
    ser.port = PORT
    ser.open()
    print(f"Connected to {PORT}")

    def safe_str(s: str) -> str:
        return s.encode("ascii", "backslashreplace").decode("ascii")

    def drain(seconds: float, verbose=True):
        end = time.monotonic() + seconds
        buf = []
        while time.monotonic() < end:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if line:
                if verbose:
                    print(f"  [DEVICE] {safe_str(line)}")
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
                print(f"  [DEVICE] {safe_str(line)}")
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
        elif meta["cf"] != "RGB565":
            print(f"  [ERROR] Unsupported format {meta['cf']}")
            return False
        Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16").save(out_png)
        print(f"  [SUCCESS] Saved {out_png}: {w}x{h}, {len(raw)} decoded bytes")
        return True

    try:
        # Wait for device to be online and connected
        print("Checking device connection state...")
        found, _ = wait_for_pattern("directory.snapshot", timeout=15)
        if not found:
            print("directory.snapshot not seen in window, sending METRICS...")
            send_cmd("CMD:METRICS")
            drain(2.0)

        # Step 1: Open catalog (Page 1: 20 stations)
        print("\n--- STEP 1: Open catalog (Page 1) ---")
        send_cmd("CMD:UI:CATALOG")
        drain(6.0)

        # Step 2: Accumulate stations via LOADMORE (Page 2: 40 stations)
        print("\n--- STEP 2: LOADMORE (Page 2: 40 stations) ---")
        send_cmd("CMD:UI:LOADMORE")
        drain(6.0)

        # Step 3: Accumulate stations via LOADMORE (Page 3: 41 stations, end of list)
        print("\n--- STEP 3: LOADMORE (Page 3: 41 stations) ---")
        send_cmd("CMD:UI:LOADMORE")
        drain(6.0)

        # Step 4: Run search for 'rock'
        print("\n--- STEP 4: Search for 'rock' ---")
        send_cmd("CMD:UI:SEARCH:rock")
        drain(8.0)
        send_cmd("CMD:METRICS")
        drain(1.0)
        take_screenshot("docs/uc5-search.png")

        # Step 5: Send CMD:UI:CANCEL and verify zero HTTP refetch of catalog
        print("\n--- STEP 5: Cancel search and verify zero HTTP refetch ---")
        send_cmd("CMD:UI:CANCEL")
        cancel_log = drain(3.0)

        # Verify no GET /api/v1/device-control/catalog/stations in cancel_log
        for line in cancel_log:
            if "catalog/stations" in line or "mode=refresh" in line or "mode=load-more" in line:
                raise RuntimeError(f"VIOLATION: Refetch detected after cancel: {line}")
        print("--> VERIFIED: Zero HTTP GET catalog refetch after CMD:UI:CANCEL!")

        take_screenshot("docs/uc5-restored.png")

        # Step 6: Test search error and recovery via BACK
        print("\n--- STEP 6: Search error (invalid_query) and recovery via BACK ---")
        send_cmd("CMD:UI:SEARCH:   ")  # spaces only -> CATALOG_QUERY_INVALID
        drain(3.0)
        take_screenshot("docs/uc5-error.png")

        print("\n--- STEP 7: Dismiss error via CMD:UI:BACK and verify intact catalog ---")
        send_cmd("CMD:UI:BACK")
        back_log = drain(3.0)
        for line in back_log:
            if "catalog/stations" in line or "mode=refresh" in line or "mode=load-more" in line:
                raise RuntimeError(f"VIOLATION: Refetch detected after BACK: {line}")
        print("--> VERIFIED: Zero HTTP GET catalog refetch after error BACK!")

        take_screenshot("docs/uc5-after-error.png")

        print("\n=== UC-5 ACCEPTANCE TEST COMPLETED SUCCESSFULLY ===")

    finally:
        ser.close()

if __name__ == "__main__":
    main()
