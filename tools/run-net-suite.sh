#!/usr/bin/env bash
# Headless network suite: boots an ISO whose /bin/login is
# tools/net-suite/netsuite.c, with virtio-net on QEMU user networking, against
# tools/net-suite/host_server.py on this host (the guest sees it at 10.0.2.2).
#
#   NET_INTERNET=1   also test the real internet (DNS, http://example.com, and
#                    the mlibc dns-smoke/http-smoke apps when mlibc is built)
#   QEMU_SMP=N       CPU count (default 1)
#   QEMU_NIC=...     NIC device (default modern virtio-net with MSI-X);
#                    virtio-net-pci,vectors=0 tests INTx, e1000 a polled NIC
#   SKIP_CMAKE=1     only repack the test program, skip the kernel build
#   NETSUITE_CFLAGS  more -D flags for netsuite.c
#   NET_LINK=0       NIC straight on slirp, without host_server.py --link in
#                    between: no fake host 10.0.2.77, no loss/delay tests
#   QEMU=...         another QEMU, e.g. ~/qemu-rtl/bin/qemu-system-x86_64 from
#                    tools/qemu-rtl8168/build-qemu.sh for QEMU_NIC=rtl8168
#
# The host also pulls the guest's cable over QMP (set_link) to test link loss.
#
# With curl built against mbedTLS the suite also tests https:// against a
# certificate chain made here (build/net-suite/tls: a test CA and a server
# certificate for 10.0.2.2 and web.test), plus the system CA bundle
# (tools/fetch-cacert.sh) with NET_INTERNET=1.
#
# Exits 0 only when the suite reaches [netsuite] END with fail=0. "GAP:" lines
# are known shortcomings recorded for later steps; they do not fail the run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/net-suite}"
NET_INTERNET="${NET_INTERNET:-0}"
TCP_ECHO_PORT="${TCP_ECHO_PORT:-18007}"
UDP_ECHO_PORT="${UDP_ECHO_PORT:-18008}"
HTTP_PORT="${HTTP_PORT:-18080}"
CLOSED_PORT="${CLOSED_PORT:-18009}"
SINK_PORT="${SINK_PORT:-18006}"
HTTPS_PORT="${HTTPS_PORT:-18443}"
NET_LINK="${NET_LINK:-1}"
LINK_PORT="${LINK_PORT:-18010}"   # 18010-18013: the NIC and slirp ends of the link
mkdir -p "$OUT_DIR"

EXTRA=()
NET_TOOLS=0
MLIBC_LIB="$BUILD_DIR/mlibc-root/lib"
if [ -f "$MLIBC_LIB/libc.so" ] && [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target net-tools >/dev/null
fi
if [ -f "$MLIBC_LIB/libc.so" ]; then
  EXTRA+=("lib/mlibc/ld-instantos.so:$MLIBC_LIB/ld-instantos.so"
          "lib/mlibc/libc.so:$MLIBC_LIB/libc.so"
          "lib/mlibc/libdl.so:$MLIBC_LIB/libdl.so")
  if [ "$NET_INTERNET" = "1" ]; then
    for app in dns-smoke http-smoke; do
      SOURCE="$ROOT/outside/iUserApps/$app/src/main.c" \
      START_SOURCE="$ROOT/outside/iUserApps/$app/src/start.S" \
      OUTPUT="$OUT_DIR/$app" "$ROOT/tools/build-mlibc-hello.sh" >/dev/null
      EXTRA+=("bin/$app:$OUT_DIR/$app")
    done
  fi
  # The network tools run under bash (CMake builds them with mlibc; curl
  # comes from tools/build-curl.sh).
  if [ -x "$BUILD_DIR/bash" ] && [ -x "$BUILD_DIR/net-tools/ping" ]; then
    NET_TOOLS=1
    EXTRA+=("bin/bash:$BUILD_DIR/bash")
    for tool in ping nslookup netcfg sntp dmesg; do EXTRA+=("bin/$tool:$BUILD_DIR/net-tools/$tool"); done
    EXTRA+=("bin/host:$BUILD_DIR/net-tools/nslookup")
    [ -x "$BUILD_DIR/curl/curl" ] && EXTRA+=("bin/curl:$BUILD_DIR/curl/curl")
  fi
fi
HAVE_CURL=0
[ "$NET_TOOLS" = "1" ] && [ -x "$BUILD_DIR/curl/curl" ] && HAVE_CURL=1
HAVE_TLS=0
HAVE_CA_BUNDLE=0
HTTPS_ARGS=()
if [ "$HAVE_CURL" = "1" ] && grep -qa "mbedTLS" "$BUILD_DIR/curl/curl"; then
  HAVE_TLS=1
  TLS_DIR="$OUT_DIR/tls"
  "$ROOT/tools/net-suite/make-test-certs.sh" "$TLS_DIR"
  EXTRA+=("ssl/test-ca.pem:$TLS_DIR/ca.pem")
  HTTPS_ARGS=(--https "$HTTPS_PORT" --cert "$TLS_DIR/server.pem" --key "$TLS_DIR/server.key")
  if [ -f "$BUILD_DIR/cacert/cacert.pem" ]; then
    HAVE_CA_BUNDLE=1
    EXTRA+=("ssl/certs/ca-certificates.crt:$BUILD_DIR/cacert/cacert.pem")
  fi
fi

if ss -ltn 2>/dev/null | grep -qE ":($TCP_ECHO_PORT|$HTTP_PORT|$CLOSED_PORT|$SINK_PORT|$HTTPS_PORT)\b"; then
  echo "netsuite: a test port is already in use on this host" >&2; exit 2
fi
LINK_ARGS=()
if [ "$NET_LINK" = "1" ]; then
  LINK_ARGS=(--link "$LINK_PORT")
  # NIC = socket netdev to host_server.py, which relays to a second socket
  # netdev sharing hub 0 with slirp.
  export QEMU_NETDEV="-netdev socket,id=net0,udp=127.0.0.1:$LINK_PORT,localaddr=127.0.0.1:$((LINK_PORT + 2)) -netdev user,id=slirp0 -netdev hubport,id=hub_slirp,hubid=0,netdev=slirp0 -netdev socket,id=link0,udp=127.0.0.1:$((LINK_PORT + 1)),localaddr=127.0.0.1:$((LINK_PORT + 3)) -netdev hubport,id=hub_link,hubid=0,netdev=link0"
fi
# QMP lets host_server.py pull the guest's cable (GET /linkflap). Unix sockets
# cannot live on a Windows (drvfs) mount such as /mnt/d.
QMP_DIR="$(mktemp -d)"
QMP_SOCK="$QMP_DIR/qmp.sock"
export QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-} -qmp unix:$QMP_SOCK,server=on,wait=off"
HOST_LOG="$OUT_DIR/host_server.log"
python3 "$ROOT/tools/net-suite/host_server.py" \
  --tcp-echo "$TCP_ECHO_PORT" --udp-echo "$UDP_ECHO_PORT" --http "$HTTP_PORT" --sink "$SINK_PORT" --qmp "$QMP_SOCK" "${LINK_ARGS[@]}" "${HTTPS_ARGS[@]}" > "$HOST_LOG" 2>&1 &
host_pid=$!
trap 'kill "$host_pid" 2>/dev/null || true; rm -rf "$QMP_DIR"' EXIT
for _ in $(seq 1 50); do grep -q ready "$HOST_LOG" 2>/dev/null && break; sleep 0.1; done
grep -q ready "$HOST_LOG" || { echo "netsuite: host server did not start" >&2; cat "$HOST_LOG" >&2; exit 2; }

status=0
SUITE_TAG=netsuite OUT_DIR="$OUT_DIR" QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}" \
RUNNER_SRC="$ROOT/tools/net-suite/netsuite.c" \
RUNNER_EXTRA_CFLAGS="-DHAVE_QMP=1 -DNET_INTERNET=$NET_INTERNET -DNET_LINK=$NET_LINK -DNET_TOOLS=$NET_TOOLS -DHAVE_CURL=$HAVE_CURL -DHAVE_TLS=$HAVE_TLS -DHAVE_CA_BUNDLE=$HAVE_CA_BUNDLE -DHTTPS_PORT=$HTTPS_PORT -DSINK_PORT=$SINK_PORT -DTCP_ECHO_PORT=$TCP_ECHO_PORT -DUDP_ECHO_PORT=$UDP_ECHO_PORT -DHTTP_PORT=$HTTP_PORT -DCLOSED_PORT=$CLOSED_PORT ${NETSUITE_CFLAGS:-}" \
EXTRA_INITRD="${EXTRA[*]:-}" \
  "$ROOT/tools/run-os-testsuite.sh" || status=$?

LOG="$OUT_DIR/netsuite.smp${QEMU_SMP:-1}.serial.log"
for marker in "mlibc-dns: serial-done" "http: serial-done"; do
  if [ "$NET_INTERNET" = "1" ] && [ -f "$MLIBC_LIB/libc.so" ] && ! grep -qa "$marker" "$LOG"; then
    echo "netsuite: missing '$marker' from the mlibc apps" >&2; status=1
  fi
done
# The default NIC must come up interrupt-driven, not fall back to polling.
if [ -z "${QEMU_NIC:-}" ] && ! grep -qa "virtio-net: MSI-X receive interrupt" "$LOG"; then
  echo "netsuite: virtio-net did not get its MSI-X receive interrupt" >&2; status=1
fi
exit "$status"
