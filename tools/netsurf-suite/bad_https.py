#!/usr/bin/env python3
"""HTTPS servers with certificates a browser must refuse, for the NetSurf suite.

  bad_https.py --ca ca.pem --ca-key ca.key --log FILE --ports P1,P2,P3,P4

Ports, in order:
  untrusted   right address, signed by a CA the guest does not know
  wrongname   signed by the test CA, for another address and name
  expired     signed by the test CA, right address, not valid any more
  selfsigned  right address, its own issuer

Each connection is logged as "<kind> connect", each HTTP request that gets
through as "<kind> REQUEST <path>": a request means the certificate was
accepted. The test CA is tools/net-suite/make-test-certs.sh's.
"""
import argparse
import datetime
import ipaddress
import os
import socket
import ssl
import sys
import tempfile
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

KINDS = ("untrusted", "wrongname", "expired", "selfsigned")
ADDRESS = "10.0.2.2"


def name(cn):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def make(subject, sans, issuer, issuer_key, key, start, end, ca=False):
    builder = (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(issuer)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(start).not_valid_after(end)
               .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if sans:
        builder = builder.add_extension(x509.SubjectAlternativeName(sans), critical=False)
    return builder.sign(issuer_key, hashes.SHA256())


def certificates(ca_cert, ca_key):
    now = datetime.datetime.now(datetime.timezone.utc)
    day = datetime.timedelta(days=1)
    ok = (now - day, now + 30 * day)
    here = [x509.IPAddress(ipaddress.ip_address(ADDRESS))]
    out = {}
    key = ec.generate_private_key(ec.SECP256R1())
    other_key = ec.generate_private_key(ec.SECP256R1())
    other = make("Some other CA", None, name("Some other CA"), other_key, other_key, *ok, ca=True)
    out["untrusted"] = (make("untrusted.test", here, other.subject, other_key, key, *ok), key)
    key = ec.generate_private_key(ec.SECP256R1())
    out["wrongname"] = (make("wrong.test", [x509.DNSName("wrong.test"),
                                            x509.IPAddress(ipaddress.ip_address("10.9.9.9"))],
                             ca_cert.subject, ca_key, key, *ok), key)
    key = ec.generate_private_key(ec.SECP256R1())
    out["expired"] = (make("expired.test", here, ca_cert.subject, ca_key, key,
                           now - 400 * day, now - 30 * day), key)
    key = ec.generate_private_key(ec.SECP256R1())
    out["selfsigned"] = (make("self.test", here, name("self.test"), key, key, *ok), key)
    return out


def serve(kind, context, port, log):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(8)

    def one(conn):
        log("%s connect" % kind)
        try:
            conn.settimeout(20)
            with context.wrap_socket(conn, server_side=True) as tls:
                line = tls.recv(4096).split(b"\r\n")[0]
                if line:
                    log("%s REQUEST %s" % (kind, line.decode("latin-1")))
                    tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 4\r\n"
                                b"Connection: close\r\n\r\noops")
                else:
                    log("%s closed" % kind)
        except (OSError, ssl.SSLError) as e:
            log("%s refused: %s" % (kind, getattr(e, "reason", None) or e))

    def loop():
        while True:
            conn, _ = listener.accept()
            threading.Thread(target=one, args=(conn,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ca", required=True)
    ap.add_argument("--ca-key", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--ports", required=True)
    args = ap.parse_args()
    with open(args.ca, "rb") as f:
        ca_cert = x509.load_pem_x509_certificate(f.read())
    with open(args.ca_key, "rb") as f:
        ca_key = serialization.load_pem_private_key(f.read(), None)
    lock = threading.Lock()

    def log(text):
        with lock, open(args.log, "a") as f:
            f.write(text + "\n")

    certs = certificates(ca_cert, ca_key)
    for kind, port in zip(KINDS, (int(p) for p in args.ports.split(","))):
        cert, key = certs[kind]
        pem = cert.public_bytes(serialization.Encoding.PEM) + key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption())
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(pem)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(f.name)
        os.unlink(f.name)
        serve(kind, context, port, log)
    print("bad_https: ready", flush=True)
    threading.Event().wait()


if __name__ == "__main__":
    sys.exit(main())
