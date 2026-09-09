import os
import sys
import time
import re
import ssl
import subprocess
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
import serial

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(TOOLS_DIR)
CA_CERT_PATH = os.path.join(TOOLS_DIR, 'test_ca_cert.pem')
SRV_CERT_PATH = os.path.join(TOOLS_DIR, 'test_server_cert.pem')
SRV_KEY_PATH = os.path.join(TOOLS_DIR, 'test_server_key.pem')
CANDIDATE_PATH = os.path.join(TOOLS_DIR, 'rock_candidate_v011.bin')
SDKCONFIG_PATH = os.path.join(PROJECT_DIR, 'sdkconfig')
SDKCONFIG_BAK = os.path.join(PROJECT_DIR, 'sdkconfig.test_bak')

class OtaHandler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        clean_path = path.lstrip('/').split('?')[0]
        return os.path.join(TOOLS_DIR, clean_path)
    def log_message(self, format, *args):
        print(f'[HTTPS Server] {args[0]} - {args[1]} - {args[2]}', flush=True)

def start_server(port=8443):
    server_address = ('0.0.0.0', port)
    httpd = HTTPServer(server_address, OtaHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=SRV_CERT_PATH, keyfile=SRV_KEY_PATH)
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    print(f'HTTPS Server running on port {port}', flush=True)
    return httpd

def get_wifi_creds():
    return 'RockLab-2G', 'rockcast1234'

def get_ca_pem():
    with open(CA_CERT_PATH, 'r') as f:
        lines = [line.strip() for line in f if line.strip() and not line.startswith('-----')]
    return ''.join(lines)

def update_sdkconfig(ssid, password, ota_url, ca_pem):
    with open(SDKCONFIG_PATH, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    with open(SDKCONFIG_BAK, 'w', encoding='utf-8') as f:
        f.write(content)

    keys = {
        'CONFIG_ROCK_WIFI_SSID': f'"{ssid}"',
        'CONFIG_ROCK_WIFI_PASSWORD': f'"{password}"',
        'CONFIG_ROCK_OTA_URL': f'"{ota_url}"',
        'CONFIG_ROCK_OTA_CHECK_ON_BOOT': 'y',
        'CONFIG_ROCK_OTA_TEST_CERT_PEM': f'"{ca_pem}"',
        'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE': 'y',
    }
    
    for k, v in keys.items():
        pattern = re.compile(rf'^{k}=.*$', re.MULTILINE)
        if pattern.search(content):
            content = pattern.sub(f'{k}={v}', content)
        else:
            content += f'\n{k}={v}\n'
            
    with open(SDKCONFIG_PATH, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Updated sdkconfig with in-memory test configuration', flush=True)

def restore_sdkconfig():
    if os.path.exists(SDKCONFIG_BAK):
        with open(SDKCONFIG_BAK, 'r', encoding='utf-8') as f:
            content = f.read()
        with open(SDKCONFIG_PATH, 'w', encoding='utf-8') as f:
            f.write(content)
        os.remove(SDKCONFIG_BAK)
        print('Restored original sdkconfig (credentials removed)', flush=True)

def run_cmd(cmd, check=True):
    full_cmd = f"powershell -ExecutionPolicy Bypass -Command \"$env:PYTHONUTF8='1'; . 'C:\\Espressif\\tools\\Microsoft.v6.1.PowerShell_profile.ps1'; {cmd}\""
    res = subprocess.run(full_cmd, shell=True, capture_output=True, text=True, cwd=PROJECT_DIR)
    if check and res.returncode != 0:
        print('STDOUT:', res.stdout, flush=True)
        print('STDERR:', res.stderr, flush=True)
        raise RuntimeError(f'Command failed: {cmd}')
    return res.stdout

def main():
    if not os.path.exists(CANDIDATE_PATH):
        raise RuntimeError('Candidate binary tools/rock_candidate_v011.bin not found')

    ssid, password = get_wifi_creds()
    ca_pem = get_ca_pem()
    ota_url = 'https://192.168.137.1:8443/rock_candidate_v011.bin'

    try:
        update_sdkconfig(ssid, password, ota_url, ca_pem)
        
        print('Building base image (v0.1.0) with idf.py build...', flush=True)
        run_cmd('idf.py build')

        print('Flashing bootloader, partition table, otadata, and base app to COM6...', flush=True)
        flash_cmd = (
            'python -m esptool -p COM6 --chip esp32p4 --after watchdog-reset write-flash '
            '--flash-mode dio --flash-size 16MB --flash-freq 80m '
            '0x2000 build/bootloader/bootloader.bin '
            '0x8000 build/partition_table/partition-table.bin '
            '0x10000 build/ota_data_initial.bin '
            '0x20000 build/rock_esp32.bin'
        )
        run_cmd(flash_cmd)

        print('Starting HTTPS test server...', flush=True)
        server = start_server(8443)

        print('Connecting to COM6 serial monitor (no DTR/RTS toggling)...', flush=True)
        time.sleep(0.5)
        ser = serial.Serial('COM6', 115200, timeout=1)

        start_time = time.time()
        saw_ota_0 = False
        saw_wifi = False
        saw_ota_start = False
        saw_ota_done = False
        saw_reboot = False
        saw_ota_1 = False
        saw_pending_verify = False
        saw_mark_valid = False
        saw_same_version_abort = False

        print('--- BEGIN DEVICE LOGS ---', flush=True)
        while time.time() - start_time < 180:
            line = ser.readline().decode('utf-8', errors='ignore')
            if line:
                clean_line = line.rstrip()
                # Suppress secrets if present
                if 'password' not in clean_line.lower():
                    print(clean_line, flush=True)
                
                if "Running partition 'ota_0'" in clean_line or 'offset 0x20000' in clean_line:
                    saw_ota_0 = True
                if 'Wi-Fi connection established' in clean_line:
                    saw_wifi = True
                if 'Starting OTA update from URL:' in clean_line:
                    saw_ota_start = True
                if 'OTA update successfully written and verified!' in clean_line:
                    saw_ota_done = True
                if "Running partition 'ota_1'" in clean_line or 'offset 0x810000' in clean_line:
                    saw_reboot = True
                    saw_ota_1 = True
                if 'is PENDING_VERIFY! Validation required' in clean_line:
                    saw_pending_verify = True
                if 'Cancelling rollback; marking running app as VALID' in clean_line:
                    saw_mark_valid = True
                if 'Firmware version matches running image; aborting update' in clean_line:
                    saw_same_version_abort = True
                    break

        ser.close()
        print('--- END DEVICE LOGS ---', flush=True)

        print('\n=== OTA TEST VERIFICATION SUMMARY ===', flush=True)
        print(f'1. Booted initial ota_0 (0.1.0): {saw_ota_0}', flush=True)
        print(f'2. Connected to Wi-Fi: {saw_wifi}', flush=True)
        print(f'3. Initiated OTA download via TLS: {saw_ota_start}', flush=True)
        print(f'4. Downloaded & flashed candidate to ota_1: {saw_ota_done}', flush=True)
        print(f'5. Rebooted into ota_1 (0x810000): {saw_ota_1}', flush=True)
        print(f'6. Booted in PENDING_VERIFY state: {saw_pending_verify}', flush=True)
        print(f'7. Validated & cancelled rollback: {saw_mark_valid}', flush=True)
        print(f'8. Aborted update on matching version: {saw_same_version_abort}', flush=True)

        success = (saw_wifi and saw_ota_start and saw_ota_done and saw_ota_1 and 
                   saw_pending_verify and saw_mark_valid and saw_same_version_abort)
        if success:
            print('\n>>> ALL RE-9 OTA VERIFICATION CHECKS PASSED SUCCESSFULLY! <<<', flush=True)
        else:
            print('\n>>> SOME CHECKS FAILED! Check log details above. <<<', flush=True)

    finally:
        restore_sdkconfig()

if __name__ == '__main__':
    main()
