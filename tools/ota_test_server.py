import os
import sys
import time
import ssl
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
import datetime
import ipaddress

from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CA_CERT_PATH = os.path.join(TOOLS_DIR, "test_ca_cert.pem")
CA_KEY_PATH = os.path.join(TOOLS_DIR, "test_ca_key.pem")
SRV_CERT_PATH = os.path.join(TOOLS_DIR, "test_server_cert.pem")
SRV_KEY_PATH = os.path.join(TOOLS_DIR, "test_server_key.pem")

def generate_certs(host_ip="192.168.31.133"):
    # 1. Generate Root CA
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_subject = ca_issuer = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "RockCast Lab"),
        x509.NameAttribute(NameOID.COMMON_NAME, "RockCast Test CA"),
    ])
    ca_cert = (
        x509.CertificateBuilder()
        .subject_name(ca_subject)
        .issuer_name(ca_issuer)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.now(datetime.timezone.utc))
        .not_valid_after(datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=30))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(ca_key, hashes.SHA256())
    )

    with open(CA_CERT_PATH, "wb") as f:
        f.write(ca_cert.public_bytes(serialization.Encoding.PEM))
    with open(CA_KEY_PATH, "wb") as f:
        f.write(ca_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        ))

    # 2. Generate Server Cert signed by CA
    srv_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    srv_subject = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "RockCast Lab"),
        x509.NameAttribute(NameOID.COMMON_NAME, host_ip),
    ])
    srv_cert = (
        x509.CertificateBuilder()
        .subject_name(srv_subject)
        .issuer_name(ca_subject)
        .public_key(srv_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.now(datetime.timezone.utc))
        .not_valid_after(datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=30))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.SubjectAlternativeName([
                x509.IPAddress(ipaddress.IPv4Address("192.168.137.1")),
                x509.IPAddress(ipaddress.IPv4Address("192.168.31.133")),
                x509.DNSName("localhost"),
            ]),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )

    with open(SRV_CERT_PATH, "wb") as f:
        f.write(srv_cert.public_bytes(serialization.Encoding.PEM))
    with open(SRV_KEY_PATH, "wb") as f:
        f.write(srv_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        ))

    print(f"Generated test certificates in {TOOLS_DIR}")
    return ca_cert.public_bytes(serialization.Encoding.PEM).decode("ascii")

class OtaHandler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        # Serve binaries from tools directory
        clean_path = path.lstrip("/").split("?")[0]
        return os.path.join(TOOLS_DIR, clean_path)

    def log_message(self, format, *args):
        print(f"[HTTPS Server] {args[0]} - {args[1]} - {args[2]}")

def run_server(port=8443):
    server_address = ("0.0.0.0", port)
    httpd = HTTPServer(server_address, OtaHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=SRV_CERT_PATH, keyfile=SRV_KEY_PATH)
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    print(f"HTTPS OTA Server listening on https://0.0.0.0:{port}/")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "gen":
        ip = sys.argv[2] if len(sys.argv) > 2 else "192.168.31.133"
        ca_pem = generate_certs(ip)
        print("CA PEM:")
        print(ca_pem)
    elif len(sys.argv) > 1 and sys.argv[1] == "serve":
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 8443
        run_server(port)
    else:
        print("Usage: python ota_test_server.py [gen <ip> | serve <port>]")
