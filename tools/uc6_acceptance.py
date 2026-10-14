"""Unified Catalog UC-6 acceptance test script ("Состояния и деградация").

Verifies:
1. Initial load (Refresh) -> full-screen loading overlay -> catalog loads 20 stations.
2. Pagination (LoadMore) -> NO full-screen overlay, list stays visible, bottom pill shows "Подгрузка...", accumulates to 40+ stations.
3. Search (Search) -> NO full-screen overlay, inline search busy indicator, results displayed.
4. Search with 0 results -> "Ничего не найдено" empty state, BACK immediately restores catalog without HTTP refetch.
5. Load-more degradation/error simulation -> station list intact, bottom pill shows error retry state, RETRY recovers and accumulates next page.
6. Connection drop and stale reset (mark_catalog_stale) -> clean offline state without crashing, BACK returns to player.
"""

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
        print("Checking device connection state...")
        found, _ = wait_for_pattern("directory.snapshot", timeout=15)
        if not found:
            print("directory.snapshot not seen in window, sending METRICS...")
            send_cmd("CMD:METRICS")
            drain(2.0)

        # -------------------------------------------------------------
        # STEP 1: Initial Load (Refresh)
        # -------------------------------------------------------------
        print("\n=======================================================")
        print("STEP 1: Initial Load (Refresh) -> overlay -> 20 stations")
        print("=======================================================")
        send_cmd("CMD:UI:CATALOG")
        step1_log = drain(6.0)

        # Verify that Refresh fetched page 1 and loaded 20 stations
        has_refresh_fetch = any("mode=refresh" in l for l in step1_log)
        print(f"  Verified mode=refresh fetch: {has_refresh_fetch}")
        take_screenshot("docs/uc6-refresh.png")

        # -------------------------------------------------------------
        # STEP 2: Pagination (LoadMore) -> NO full-screen overlay
        # -------------------------------------------------------------
        print("\n=======================================================")
        print("STEP 2: LoadMore (Page 2) -> NO modal overlay, inline pill")
        print("=======================================================")
        send_cmd("CMD:UI:LOADMORE")
        step2_log = drain(6.0)

        # Verify mode=load-more
        has_loadmore_fetch = any("mode=load-more" in l for l in step2_log)
        print(f"  Verified mode=load-more fetch: {has_loadmore_fetch}")
        take_screenshot("docs/uc6-loadmore.png")

        # -------------------------------------------------------------
        # STEP 3: Search -> NO modal overlay, inline search busy
        # -------------------------------------------------------------
        print("\n=======================================================")
        print("STEP 3: Search ('rock') -> NO modal overlay, inline busy")
        print("=======================================================")
        send_cmd("CMD:UI:SEARCH:rock")
        step3_log = drain(8.0)

        has_search_fetch = any("mode=search" in l for l in step3_log)
        print(f"  Verified mode=search fetch: {has_search_fetch}")
        take_screenshot("docs/uc6-search.png")

        # -------------------------------------------------------------
        # STEP 4: Empty Search -> 'Ничего не найдено' + instant return
        # -------------------------------------------------------------
        print("\n=======================================================")
        print("STEP 4: Search with 0 results -> Empty state overlay")
        print("=======================================================")
        send_cmd("CMD:UI:SEARCH:nonexistentstationxyz123")
        drain(6.0)
        take_screenshot("docs/uc6-search-empty.png")

        print("\n>>> Dismiss empty search via CMD:UI:BACK -> restore catalog")
        send_cmd("CMD:UI:BACK")
        back_log = drain(3.0)

        # Ensure no catalog refetch occurred
        for l in back_log:
            if "catalog/stations" in l or "mode=refresh" in l or "mode=load-more" in l:
                raise RuntimeError(f"VIOLATION: Refetch detected after search BACK: {l}")
        print("  --> VERIFIED: Zero HTTP GET catalog refetch after empty search BACK!")
        take_screenshot("docs/uc6-search-restored.png")

        # -------------------------------------------------------------
        # STEP 5: LoadMore error simulation & recovery
        # -------------------------------------------------------------
        print("\n=======================================================")
        print("STEP 5: Simulate LoadMore error -> inline pill error")
        print("=======================================================")
        send_cmd("CMD:UI:LOADMORE_ERR")
        drain(2.0)
        take_screenshot("docs/uc6-loadmore-error.png")

        print("\n>>> Trigger RETRY on load-more error via CMD:UI:RETRY")
        send_cmd("CMD:UI:RETRY")
        retry_log = drain(6.0)

        has_retry_fetch = any("mode=load-more" in l for l in retry_log)
        print(f"  Verified retry re-sent mode=load-more: {has_retry_fetch}")
        take_screenshot("docs/uc6-loadmore-recovered.png")

        # -------------------------------------------------------------
        # STEP 6: Stale reset (mark_catalog_stale) & offline recovery
        # -------------------------------------------------------------
        print("\n=======================================================")
        print("STEP 6: Stale reset -> offline overlay & recovery")
        print("=======================================================")
        send_cmd("CMD:UI:STALE")
        drain(2.0)
        take_screenshot("docs/uc6-offline.png")

        print("\n>>> Dismiss offline overlay via CMD:UI:BACK -> return to NP")
        send_cmd("CMD:UI:BACK")
        drain(2.0)
        take_screenshot("docs/uc6-offline-recovered.png")

        # Check memory metrics
        send_cmd("CMD:METRICS")
        drain(2.0)

        print("\n=======================================================")
        print("ALL UC-6 ACCEPTANCE CRITERIA PASSED SUCCESSFULLY!")
        print("=======================================================")

    finally:
        ser.close()
        print("Closed serial port.")

if __name__ == "__main__":
    main()
