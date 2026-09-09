import sys
import subprocess
import re

print("Python:", sys.version)

try:
    import ssl
    print("SSL module: available")
except Exception as e:
    print("SSL error:", e)

try:
    import cryptography
    print("Cryptography module: available")
except Exception as e:
    print("Cryptography not available:", e)

# Test reading profile without printing password
try:
    out = subprocess.check_output(["netsh", "wlan", "show", "profile", "name=Stranger_n5", "key=clear"])
    for encoding in ["cp866", "utf-8", "cp1251"]:
        try:
            text = out.decode(encoding)
            m = re.search(r"(?:Содержимое ключа|Key Content)\s*:\s*(.+)", text)
            if m:
                print("Wi-Fi password successfully extracted in memory (length:", len(m.group(1).strip()), ")")
                break
        except Exception:
            pass
except Exception as e:
    print("Netsh error:", e)
