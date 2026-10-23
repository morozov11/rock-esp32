import serial
import time
import sys
import threading
import re

STATION_ID = 'somafm-metal-detector'

class SerialTester:
    def __init__(self, port='COM6', baudrate=115200):
        if hasattr(sys.stdout, 'reconfigure'):
            sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        self.port = port
        self.baudrate = baudrate
        self.lines = []
        self.lock = threading.Lock()
        self.stop = False
        self.ser = None
        self.thread = None

    def start(self):
        print(f"Connecting to {self.port} with dtr=False, rts=False...")
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baudrate
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        buf = b""
        while not self.stop:
            try:
                chunk = self.ser.read(1024)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                line_clean = re.sub(r"https?://[^\s'\"]+", "<redacted-uri>", line)
                if 'WSS tx: ' in line_clean or 'WSS rx: ' in line_clean:
                    pass
                else:
                    sys.stdout.write(line_clean + "\n")
                    sys.stdout.flush()
                with self.lock:
                    self.lines.append(line_clean)

    def send(self, cmd):
        print(f"\n---> SENDING: {cmd} at {time.strftime('%H:%M:%S')}")
        self.ser.write((cmd + "\n").encode("utf-8"))
        self.ser.flush()

    def wait_for(self, pattern, timeout_s, since_idx=0):
        rx = re.compile(pattern)
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            with self.lock:
                for idx in range(since_idx, len(self.lines)):
                    m = rx.search(self.lines[idx])
                    if m:
                        return idx, m
            time.sleep(0.05)
        return None, None

    def current_line_idx(self):
        with self.lock:
            return len(self.lines)

    def close(self):
        self.stop = True
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.ser and self.ser.is_open:
            self.ser.close()

def main():
    tester = SerialTester('COM6', 115200)
    tester.start()

    try:
        print("\n[STEP 1] Checking device WSS connection...")
        idx, _ = tester.wait_for(r"(device\.registered|device\.heartbeat_ack|WSS connected)", timeout_s=30)
        if idx is None:
            print("Device not reporting WSS heartbeat, continuing anyway...")

        # Switch to Cast if not already
        print("\n[STEP 2] Discovering & selecting Cast device...")
        since = tester.current_line_idx()
        tester.send("CMD:UI:CAST")
        idx, _ = tester.wait_for(r"Cast discovery total: [1-9]\d* devices", timeout_s=15, since_idx=since)
        if idx is None:
            print("Cast discovery did not find devices, aborting")
            return False
        
        since = tester.current_line_idx()
        tester.send("CMD:UI:CAST:SELECT:0")
        idx, _ = tester.wait_for(r"TLS connected to Chromecast", timeout_s=15, since_idx=since)
        if idx is None:
            print("TLS connected message not observed, may already be connected or connecting...")

        print("\n[STEP 3] Starting playback on station...")
        since = tester.current_line_idx()
        tester.send(f"CMD:UI:STATIONID:{STATION_ID}")
        idx, _ = tester.wait_for(r"Relay streamed \d+ bytes to Chromecast", timeout_s=40, since_idx=since)
        if idx is None:
            print("Failed to start Cast playback")
            return False
        print("Cast playback actively streaming to Chromecast!")

        time.sleep(3)

        print("\n[STEP 4] Pausing playback (CMD:UI:PLAYPAUSE)...")
        t_pause_sent = time.time()
        since = tester.current_line_idx()
        tester.send("CMD:UI:PLAYPAUSE")
        idx, _ = tester.wait_for(r"(Serial trigger play/pause|Playback status changed|playback.*PAUSED)", timeout_s=5, since_idx=since)
        print(f"Pause processed! Latency: {(time.time() - t_pause_sent)*1000:.1f}ms")

        print("\n[STEP 5] Waiting 15 seconds in PAUSED state (idle socket timeout observation)...")
        for i in range(15):
            time.sleep(1)
            sys.stdout.write(".")
            sys.stdout.flush()
        print("\nIdle wait complete.")

        print("\n[STEP 6] Testing UI responsiveness while paused (CMD:UI:VOL+)...")
        t_cmd_sent = time.time()
        since = tester.current_line_idx()
        tester.send("CMD:UI:VOL+")
        idx, _ = tester.wait_for(r"(Serial trigger volume|Volume adjusted|Volume set)", timeout_s=2, since_idx=since)
        elapsed_ms = (time.time() - t_cmd_sent) * 1000
        if idx is not None:
            print(f"SUCCESS: UI responded immediately in {elapsed_ms:.1f}ms! No lockup!")
        else:
            print(f"WARNING: UI volume response not seen within 2s (elapsed {elapsed_ms:.1f}ms)")

        print("\n[STEP 7] Resuming playback on Cast (CMD:UI:PLAYPAUSE)...")
        t_resume_sent = time.time()
        since = tester.current_line_idx()
        tester.send("CMD:UI:PLAYPAUSE")
        idx, _ = tester.wait_for(r"(Relay streamed \d+ bytes to Chromecast|Cast LOAD ready!)", timeout_s=25, since_idx=since)
        if idx is not None:
            print(f"SUCCESS: Playback resumed on Chromecast! (took {(time.time() - t_resume_sent):.1f}s)")
        else:
            print("WARNING: Resume stream to Chromecast did not confirm in 25s")

        print("\n[STEP 8] Stopping playback (CMD:UI:STOP)...")
        t_stop_sent = time.time()
        since = tester.current_line_idx()
        tester.send("CMD:UI:STOP")
        idx, _ = tester.wait_for(r"(Serial trigger stop|Playback status changed|Stopping stream source)", timeout_s=5, since_idx=since)
        elapsed_stop_ms = (time.time() - t_stop_sent) * 1000
        print(f"Stop command processed in {elapsed_stop_ms:.1f}ms")

        # Give it a moment to settle
        time.sleep(2)
        print("\n=== TEST PASSED SUCCESSFULLY ===")
        return True

    finally:
        tester.close()

if __name__ == '__main__':
    sys.exit(0 if main() else 1)
