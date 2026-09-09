import os
import re
import subprocess
import shutil

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(TOOLS_DIR)
CA_CERT_PATH = os.path.join(TOOLS_DIR, 'test_ca_cert.pem')
SDKCONFIG_PATH = os.path.join(PROJECT_DIR, 'sdkconfig')
CANDIDATE_PATH = os.path.join(TOOLS_DIR, 'rock_candidate_v011.bin')
CMAKELISTS_PATH = os.path.join(PROJECT_DIR, 'CMakeLists.txt')

with open(CA_CERT_PATH, 'r') as f:
    ca_base64 = ''.join([line.strip() for line in f if line.strip() and not line.startswith('-----')])

with open(SDKCONFIG_PATH, 'r', encoding='utf-8', errors='ignore') as f:
    sdk = f.read()

sdk = re.sub(r'^CONFIG_ROCK_OTA_TEST_CERT_PEM=.*$', f'CONFIG_ROCK_OTA_TEST_CERT_PEM="{ca_base64}"', sdk, flags=re.MULTILINE)
sdk = re.sub(r'^CONFIG_ROCK_OTA_URL=.*$', 'CONFIG_ROCK_OTA_URL="https://192.168.137.1:8443/rock_candidate_v011.bin"', sdk, flags=re.MULTILINE)
sdk = re.sub(r'^CONFIG_ROCK_OTA_CHECK_ON_BOOT=.*$', 'CONFIG_ROCK_OTA_CHECK_ON_BOOT=y', sdk, flags=re.MULTILINE)
sdk = re.sub(r'^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=.*$', 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y', sdk, flags=re.MULTILINE)

with open(SDKCONFIG_PATH, 'w', encoding='utf-8') as f:
    f.write(sdk)

# Set version to 0.1.1
with open(CMAKELISTS_PATH, 'r') as f:
    cmake = f.read()
cmake = cmake.replace('set(PROJECT_VER "0.1.0")', 'set(PROJECT_VER "0.1.1")')
with open(CMAKELISTS_PATH, 'w') as f:
    f.write(cmake)

cmd = "powershell -ExecutionPolicy Bypass -Command \"$env:PYTHONUTF8='1'; . 'C:\\Espressif\\tools\\Microsoft.v6.1.PowerShell_profile.ps1'; idf.py build\""
print('Building candidate 0.1.1...')
subprocess.run(cmd, shell=True, check=True, cwd=PROJECT_DIR)

shutil.copyfile(os.path.join(PROJECT_DIR, 'build', 'rock_esp32.bin'), CANDIDATE_PATH)
print(f'Copied candidate to {CANDIDATE_PATH}')

# Restore CMakeLists.txt to 0.1.0
with open(CMAKELISTS_PATH, 'r') as f:
    cmake = f.read()
cmake = cmake.replace('set(PROJECT_VER "0.1.1")', 'set(PROJECT_VER "0.1.0")')
with open(CMAKELISTS_PATH, 'w') as f:
    f.write(cmake)
print('Restored CMakeLists.txt to 0.1.0')

with open(CANDIDATE_PATH, 'rb') as f:
    data = f.read()
print('Candidate has 192.168.137.1:', b'192.168.137.1' in data)
print('Candidate size:', len(data))
