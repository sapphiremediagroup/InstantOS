#!/usr/bin/env bash
# Makes the network suites' TLS test chain in DIR (once): ca.pem/ca.key, a
# root CA, and server.pem/server.key, a certificate for 10.0.2.2 and
# web.test it signed. An RSA root signing an ECDSA leaf, so both signature
# checks run. Used by tools/run-net-suite.sh and tools/run-netsurf-suite.sh.
#
#   tools/net-suite/make-test-certs.sh DIR
set -euo pipefail
DIR="$1"
[ -f "$DIR/server.pem" ] && exit 0
mkdir -p "$DIR"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$DIR/ca.key" -out "$DIR/ca.pem" \
  -days 3650 -subj "/CN=InstantOS netsuite test CA" >/dev/null 2>&1
openssl ecparam -name prime256v1 -genkey -noout -out "$DIR/server.key"
openssl req -new -key "$DIR/server.key" -subj "/CN=web.test" -out "$DIR/server.csr"
printf 'subjectAltName=IP:10.0.2.2,DNS:web.test\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$DIR/server.ext"
openssl x509 -req -in "$DIR/server.csr" -CA "$DIR/ca.pem" -CAkey "$DIR/ca.key" \
  -CAcreateserial -days 825 -extfile "$DIR/server.ext" -out "$DIR/server.pem" >/dev/null 2>&1
