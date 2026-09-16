"""Capture the device's LVGL snapshot; reject truncated or interleaved serial data."""
import base64
import sys
import time
import serial
from PIL import Image


def capture(port="COM6", out_png="screen.png", cmd="CMD:SCREENSHOT", timeout=25):
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    ser.dtr = ser.rts = False
    ser.port = port
    ser.open()
    try:
        # Opening the ESP32-P4 USB-Serial/JTAG port resets this board.  Let the
        # display, GUI task, and onboarding decision settle before requesting a
        # snapshot so boot logs cannot be interleaved with the base64 payload.
        time.sleep(6)
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode())
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
                    raise ValueError(f"Interleaved serial data: {text[:120]!r}") from exc
                lines.append(text)
        if not (meta and finished):
            raise ValueError("Incomplete screenshot stream")
        raw = base64.b64decode("".join(lines), validate=True)
        w, h = int(meta["w"]), int(meta["h"])
        if meta["cf"] == "RGB565_RLE":
            if len(raw) % 3: raise ValueError("Truncated RLE triplet")
            data = bytearray()
            for i in range(0, len(raw), 3):
                data.extend(raw[i+1:i+3] * raw[i])
                if len(data) > w*h*2: raise ValueError("RLE exceeds image bounds")
            raw = bytes(data)
        elif meta["cf"] != "RGB565":
            raise ValueError("Unsupported pixel format")
        if len(raw) != w*h*2: raise ValueError("Incorrect decoded image size")
        Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16").save(out_png)
        print(f"Saved {out_png}: {w}x{h}, {len(raw)} decoded bytes")
        return True
    finally:
        ser.close()


if __name__ == "__main__":
    capture(*(sys.argv[1:4]))
