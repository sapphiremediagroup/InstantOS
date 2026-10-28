#!/usr/bin/env python3
"""Host-side peer for the InstantOS network suite (tools/run-net-suite.sh).

QEMU user networking (slirp) maps the guest's 10.0.2.2 to the host's
127.0.0.1, so everything here listens on loopback:

  TCP echo   --tcp-echo PORT   echoes every byte back until the guest closes
  TCP sink   --sink PORT       reads until EOF, checking pattern(i); the result
                               is GET /sinkstat
  UDP echo   --udp-echo PORT   sends each datagram back to its sender
  HTTP       --http PORT       GET /hello        -> "hello from the host\\n"
                               GET /pattern/N    -> N bytes of pattern(i)
                               GET /sinkstat     -> "n=K bytes=B ok=0|1 ms=T"
                                                    for the K-th finished upload
                               GET /time         -> host Unix time, "S.US"
                               GET /linkflap/MS  -> "ok", then (with --qmp)
                                                    the guest NIC's link goes
                                                    down for MS ms
  HTTPS      --https PORT --cert PEM --key PEM
                               the same pages over TLS
             --pages DIR       GET /ns/NAME serves DIR/NAME (html, css), and
                               /ns/logo.png a 160x100 PNG of #e76f51
             --request-log F   appends "http|https PATH TIME" for every GET
             --qmp SOCKET      QEMU's QMP socket, for /linkflap (set_link on
                               the netdev "net0")

pattern(i) = (i*7 + (i >> 8)) & 0xff, the same function the guest checks
against, so a body can be verified byte by byte without a copy on either side.

  link       --link BASE   the guest's Ethernet. QEMU's NIC is a socket netdev
             on BASE (QEMU's end BASE+2), slirp sits on a hub with another
             socket netdev on BASE+1 (QEMU's end BASE+3); each UDP datagram is
             one frame and this relays them. On the way it can drop, duplicate,
             reorder and delay TCP frames, and it plays a second machine,
             10.0.2.77, which the guest drives over UDP:
               ARP and ping        answered
               UDP to most ports   ICMP port unreachable
               TCP SYN             ICMP host unreachable; to port 7792 a RST,
                                   after noting the SYN's MSS and window scale
               UDP 7777            pings the guest, probes guest port 9 by UDP
                                   and TCP
               UDP 7778            replies "echo=E unreach=U rst=R syn_mss=M
                                   syn_ws=W dropped=D"
               UDP 7779            "impair loss=P dup=P reorder=P delay=MS" (per
                                   cent, ms one way) or "impair off"; "ok"
               UDP 53              DNS for DNS_ZONE
               UDP 123             NTP (SNTPv4 server, stratum 2, host time)

Runs until killed; prints "host_server: ready" once every socket is listening.
"""
import argparse
import heapq
import http.server
import random
import socket
import socketserver
import ssl
import struct
import sys
import threading
import time
import zlib


def pattern(start, n):
    return bytes(((i * 7 + (i >> 8)) & 0xFF) for i in range(start, start + n))


class TcpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data:
                return
            self.request.sendall(data)


SINK = {"n": 0, "bytes": 0, "ok": 0, "ms": 0}
SINK_LOCK = threading.Lock()


class TcpSink(socketserver.BaseRequestHandler):
    def handle(self):
        start = time.monotonic()
        total, good = 0, True
        while True:
            try:
                data = self.request.recv(65536)
            except OSError:
                good = False
                break
            if not data:
                break
            if good and data != pattern(total, len(data)):
                good = False
            total += len(data)
        with SINK_LOCK:
            SINK["n"] += 1
            SINK["bytes"] = total
            SINK["ok"] = int(good)
            SINK["ms"] = int((time.monotonic() - start) * 1000)


def solid_png(width, height, rgb):
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


PAGES = {"dir": None, "log": None}
QMP = {"socket": None}


def qmp_commands(path, commands):
    """Runs QMP commands (dicts) on a fresh connection; returns the replies."""
    import json
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    reader = sock.makefile("r")
    reader.readline()   # greeting
    replies = []
    for command in [{"execute": "qmp_capabilities"}] + commands:
        sock.sendall((json.dumps(command) + "\n").encode())
        while True:
            reply = json.loads(reader.readline())
            if "return" in reply or "error" in reply:
                replies.append(reply)
                break
    sock.close()
    return replies[1:]


def link_flap(down_ms):
    time.sleep(0.3)   # let the HTTP reply out first
    qmp_commands(QMP["socket"], [{"execute": "set_link", "arguments": {"name": "net0", "up": False}}])
    time.sleep(down_ms / 1000.0)
    qmp_commands(QMP["socket"], [{"execute": "set_link", "arguments": {"name": "net0", "up": True}}])
PAGE_TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css", ".png": "image/png"}
REQUEST_LOG_LOCK = threading.Lock()


class Http(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    content_type = "application/octet-stream"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if PAGES["log"]:
            scheme = "https" if isinstance(self.server, ThreadedHttps) else "http"
            with REQUEST_LOG_LOCK, open(PAGES["log"], "a") as log:
                log.write("%s %s %.3f\n" % (scheme, self.path, time.time()))
        if self.path.startswith("/ns/") and PAGES["dir"]:
            name = self.path[len("/ns/"):].split("?")[0]
            ext = name[name.rfind("."):] if "." in name else ""
            body = None
            if name == "logo.png":
                body = solid_png(160, 100, (0xe7, 0x6f, 0x51))
            elif "/" not in name and ".." not in name and ext in PAGE_TYPES:
                try:
                    with open(PAGES["dir"] + "/" + name, "rb") as f:
                        body = f.read()
                except OSError:
                    body = None
            if body is not None:
                self.content_type = PAGE_TYPES.get(ext, "application/octet-stream")
                self.reply(200, len(body), [body])
                return
        if self.path == "/hello":
            body = b"hello from the host\n"
            self.reply(200, len(body), [body])
            return
        if self.path == "/time":
            body = b"%.6f\n" % time.time()
            self.reply(200, len(body), [body])
            return
        if self.path.startswith("/linkflap/") and QMP["socket"]:
            try:
                down_ms = int(self.path[len("/linkflap/"):])
            except ValueError:
                down_ms = -1
            if 0 < down_ms <= 60000:
                threading.Thread(target=link_flap, args=(down_ms,), daemon=True).start()
                body = b"ok\n"
                self.reply(200, len(body), [body])
                return
        if self.path == "/sinkstat":
            with SINK_LOCK:
                body = b"n=%d bytes=%d ok=%d ms=%d\n" % (SINK["n"], SINK["bytes"], SINK["ok"], SINK["ms"])
            self.reply(200, len(body), [body])
            return
        if self.path.startswith("/pattern/"):
            try:
                n = int(self.path[len("/pattern/"):])
            except ValueError:
                n = -1
            if 0 <= n <= 64 * 1024 * 1024:
                chunks = (pattern(off, min(65536, n - off)) for off in range(0, n, 65536))
                self.reply(200, n, chunks)
                return
        self.reply(404, 0, [])

    def reply(self, code, length, chunks):
        self.send_response(code)
        self.send_header("Content-Type", self.content_type)
        self.send_header("Content-Length", str(length))
        self.send_header("Connection", "close")
        self.end_headers()
        for chunk in chunks:
            self.wfile.write(chunk)
        self.close_connection = True


class ThreadedTcp(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class ThreadedHttp(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


class ThreadedHttps(ThreadedHttp):
    """HTTP over TLS; the handshake runs in the connection's thread."""

    def __init__(self, address, handler, context):
        self.context = context
        super().__init__(address, handler)

    def get_request(self):
        sock, peer = self.socket.accept()
        return self.context.wrap_socket(sock, server_side=True, do_handshake_on_connect=False), peer

    def finish_request(self, request, client_address):
        try:
            request.settimeout(30)
            request.do_handshake()
        except (ssl.SSLError, OSError):
            return   # the guest rejected our certificate, or went away
        super().finish_request(request, client_address)


NTP_UNIX_OFFSET = 2208988800


def ntp_time(t):
    seconds = int(t)
    return struct.pack("!II", (seconds + NTP_UNIX_OFFSET) & 0xFFFFFFFF, int((t - seconds) * 2 ** 32))


def ntp_answer(request):
    """An SNTPv4 server reply to a client request, else None."""
    if len(request) < 48 or request[0] & 7 != 3:
        return None
    received = time.time()
    version = (request[0] >> 3) & 7
    return (struct.pack("!BBbbII4s", (version << 3) | 4, 2, request[2], -20, 0, 0, b"HOST") +
            ntp_time(received - 1) + request[40:48] + ntp_time(received) + ntp_time(time.time()))


def udp_echo(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    return s


FAKE_IP = bytes([10, 0, 2, 77])
FAKE_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x77])
PROBE_PORT, STATUS_PORT, IMPAIR_PORT, SYN_PORT = 7777, 7778, 7779, 7792
PROBE_TCP_SPORT, PROBE_UDP_SPORT, GUEST_CLOSED_PORT = 7790, 7791, 9


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(src, dst, proto, payload):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0x7777, 0, 64, proto, 0, src, dst)
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    return hdr + payload


def tcp_segment(src_ip, dst_ip, sport, dport, seq, ack, flags):
    hdr = struct.pack("!HHIIBBHHH", sport, dport, seq, ack, 5 << 4, flags, 8192, 0, 0)
    pseudo = src_ip + dst_ip + struct.pack("!BBH", 0, 6, len(hdr))
    return hdr[:16] + struct.pack("!H", csum(pseudo + hdr)) + hdr[18:]


def dns_name(labels):
    return b"".join(bytes([len(l)]) + l for l in labels) + b"\0"


# The fake host's DNS zone: name -> list of (type, rdata).
DNS_ZONE = {
    "web.test": [(1, bytes([10, 0, 2, 2]))],
    "alias.test": [(5, dns_name([b"web", b"test"]))],
    "mail.test": [(15, b"\x00\x0a" + dns_name([b"web", b"test"]))],
    "txt.test": [(16, b"\x0bhello world")],
    "2.2.0.10.in-addr.arpa": [(12, dns_name([b"web", b"test"]))],
}


def dns_answer(query):
    """Answer a query from DNS_ZONE (following a CNAME), else NXDOMAIN."""
    if len(query) < 17:
        return None
    labels, off = [], 12
    while off < len(query) and query[off]:
        n = query[off]
        labels.append(query[off + 1:off + 1 + n])
        off += 1 + n
    off += 1
    if off + 4 > len(query):
        return None
    qtype = struct.unpack("!H", query[off:off + 2])[0]
    question = query[12:off + 4]
    name = b".".join(labels).decode(errors="replace").lower()
    answers = []
    owner = 12   # compression pointer to the question's name
    seen = 0
    while name in DNS_ZONE and seen < 4:
        seen += 1
        records = DNS_ZONE[name]
        cname = [r for r in records if r[0] == 5]
        for rtype, rdata in records:
            if rtype == qtype or rtype == 5 or qtype == 255:
                answers.append((owner, rtype, rdata))
        if not cname or qtype == 5:
            break
        name = ".".join(l.decode() for l in parse_labels(cname[0][1]))
        owner = None   # spelled out below
    flags = 0x8580 if name in DNS_ZONE else 0x8583   # QR AA RD RA, NXDOMAIN when unknown
    body = b""
    for o, rtype, rdata in answers:
        owner_bytes = struct.pack("!H", 0xC000 | o) if o is not None else dns_name([l.encode() for l in name.split(".")])
        body += owner_bytes + struct.pack("!HHIH", rtype, 1, 60, len(rdata)) + rdata
    header = query[:2] + struct.pack("!HHHHH", flags, 1, len(answers), 0, 0)
    return header + question + body


def parse_labels(encoded):
    labels, i = [], 0
    while i < len(encoded) and encoded[i]:
        labels.append(encoded[i + 1:i + 1 + encoded[i]])
        i += 1 + encoded[i]
    return labels


def is_tcp(frame):
    return len(frame) >= 34 and frame[12:14] == b"\x08\x00" and frame[23] == 6


class FakeHost:
    """10.0.2.77, a second machine on the guest's Ethernet."""

    def __init__(self, link):
        self.link = link
        self.echo = self.unreach = self.rst = 0
        self.syn_mss = self.syn_ws = -1

    def send(self, dst_mac, ethertype, payload):
        frame = dst_mac + FAKE_MAC + struct.pack("!H", ethertype) + payload
        self.link.to_guest(frame.ljust(60, b"\0"))

    def send_ip(self, mac, dst_ip, proto, payload):
        self.send(mac, 0x0800, ipv4(FAKE_IP, dst_ip, proto, payload))

    def icmp(self, mac, dst_ip, typ, code, rest, body):
        msg = struct.pack("!BBH", typ, code, 0) + rest + body
        msg = msg[:2] + struct.pack("!H", csum(msg)) + msg[4:]
        self.send_ip(mac, dst_ip, 1, msg)

    def udp(self, mac, dst_ip, sport, dport, data):
        self.send_ip(mac, dst_ip, 17, struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data)

    def wants(self, frame):
        """True for the frames addressed to this host."""
        if len(frame) < 42:
            return False
        ethertype = frame[12:14]
        if ethertype == b"\x08\x06":
            return frame[38:42] == FAKE_IP
        return ethertype == b"\x08\x00" and frame[30:34] == FAKE_IP

    def handle(self, frame):
        try:
            self._handle(frame)
        except (struct.error, IndexError, ValueError):
            pass

    def _handle(self, frame):
        src_mac = frame[6:12]
        if frame[12:14] == b"\x08\x06":
            if struct.unpack("!H", frame[20:22])[0] == 1:
                reply = frame[14:20] + struct.pack("!H", 2) + FAKE_MAC + FAKE_IP + frame[22:28] + frame[28:32]
                self.send(src_mac, 0x0806, reply)
            return
        ihl = (frame[14] & 0x0F) * 4
        ip = frame[14:]
        total = struct.unpack("!H", ip[2:4])[0]
        ip = ip[:total]
        proto, guest_ip, l4 = ip[9], ip[12:16], ip[ihl:]
        quote = ip[:ihl + 8]
        if proto == 1:
            typ, code = l4[0], l4[1]
            if typ == 8:
                self.icmp(src_mac, guest_ip, 0, 0, l4[4:8], l4[8:])
            elif typ == 0 and struct.unpack("!H", l4[4:6])[0] == 0x7777:
                self.echo = 1
            elif typ == 3 and code == 3 and len(l4) >= 8 + 20 + 4:
                inner = l4[8:]
                inner_ihl = (inner[0] & 0x0F) * 4
                if inner[9] == 17 and struct.unpack("!H", inner[inner_ihl + 2:inner_ihl + 4])[0] == GUEST_CLOSED_PORT:
                    self.unreach = 1
            return
        if proto == 6:
            sport, dport, seq = struct.unpack("!HHI", l4[:8])
            flags = l4[13]
            if flags & 0x04:
                if dport == PROBE_TCP_SPORT and sport == GUEST_CLOSED_PORT:
                    self.rst = 1
            elif flags & 0x02 and dport == SYN_PORT:
                self.note_syn_options(l4)
                rst = tcp_segment(FAKE_IP, guest_ip, dport, sport, 0, seq + 1, 0x14)
                self.send_ip(src_mac, guest_ip, 6, rst)
            elif flags & 0x02:
                self.icmp(src_mac, guest_ip, 3, 1, b"\0" * 4, quote)   # host unreachable
            return
        if proto == 17:
            sport, dport = struct.unpack("!HH", l4[:4])
            data = l4[8:]
            if dport == PROBE_PORT:
                self.echo = self.unreach = self.rst = 0
                self.icmp(src_mac, guest_ip, 8, 0, struct.pack("!HH", 0x7777, 1), b"fake host ping")
                self.udp(src_mac, guest_ip, PROBE_UDP_SPORT, GUEST_CLOSED_PORT, b"anyone?")
                syn = tcp_segment(FAKE_IP, guest_ip, PROBE_TCP_SPORT, GUEST_CLOSED_PORT, 1000, 0, 0x02)
                self.send_ip(src_mac, guest_ip, 6, syn)
            elif dport == STATUS_PORT:
                status = b"echo=%d unreach=%d rst=%d syn_mss=%d syn_ws=%d dropped=%d" % (
                    self.echo, self.unreach, self.rst, self.syn_mss, self.syn_ws, self.link.dropped)
                self.udp(src_mac, guest_ip, STATUS_PORT, sport, status)
            elif dport == 53:
                answer = dns_answer(data)
                if answer:
                    self.udp(src_mac, guest_ip, 53, sport, answer)
            elif dport == 123:
                answer = ntp_answer(data)
                if answer:
                    self.udp(src_mac, guest_ip, 123, sport, answer)
            elif dport == IMPAIR_PORT:
                self.link.configure(data.decode(errors="replace"))
                self.udp(src_mac, guest_ip, IMPAIR_PORT, sport, b"ok")
            else:
                self.icmp(src_mac, guest_ip, 3, 3, b"\0" * 4, quote)   # port unreachable

    def note_syn_options(self, l4):
        self.syn_mss = self.syn_ws = -1
        opts = l4[20:(l4[12] >> 4) * 4]
        i = 0
        while i < len(opts) and opts[i] != 0:
            if opts[i] == 1:
                i += 1
                continue
            if i + 1 >= len(opts) or opts[i + 1] < 2:
                break
            kind, length = opts[i], opts[i + 1]
            if kind == 2 and length == 4:
                self.syn_mss = struct.unpack("!H", opts[i + 2:i + 4])[0]
            elif kind == 3 and length == 3:
                self.syn_ws = opts[i + 2]
            i += length


class Link:
    """Relays the guest's frames to slirp and back, impairing TCP on request."""

    def __init__(self, base):
        self.guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.guest.bind(("127.0.0.1", base))
        self.guest_peer = ("127.0.0.1", base + 2)
        self.slirp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.slirp.bind(("127.0.0.1", base + 1))
        self.slirp_peer = ("127.0.0.1", base + 3)
        for s in (self.guest, self.slirp):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.fake = FakeHost(self)
        self.loss = self.dup = self.reorder = 0.0
        self.delay = 0.0
        self.rng = random.Random(1)
        self.dropped = 0
        self.cond = threading.Condition()
        self.pending = []
        self.counter = 0

    def configure(self, text):
        words = text.split()
        with self.cond:
            self.loss = self.dup = self.reorder = 0.0
            self.delay = 0.0
            self.rng = random.Random(1)
            for w in words[1:]:
                key, _, value = w.partition("=")
                if key in ("loss", "dup", "reorder"):
                    setattr(self, key, float(value) / 100.0)
                elif key == "delay":
                    self.delay = float(value) / 1000.0
        print("link: %s" % text, flush=True)

    def to_guest(self, frame):
        self.guest.sendto(frame, self.guest_peer)

    def forward(self, sock, peer, frame):
        with self.cond:
            impaired = is_tcp(frame) and (self.loss or self.dup or self.reorder or self.delay)
            if not impaired:
                if not self.pending:
                    sock.sendto(frame, peer)
                    return
                due = time.monotonic()   # keep order behind delayed frames
            else:
                if self.rng.random() < self.loss:
                    self.dropped += 1
                    return
                due = time.monotonic() + self.delay
                if self.rng.random() < self.reorder:
                    due += 0.002 + self.rng.random() * 0.010
                copies = 2 if self.rng.random() < self.dup else 1
            for _ in range(copies if impaired else 1):
                self.counter += 1
                heapq.heappush(self.pending, (due, self.counter, sock, peer, frame))
            self.cond.notify()

    def scheduler(self):
        while True:
            with self.cond:
                while not self.pending:
                    self.cond.wait()
                due, _, sock, peer, frame = self.pending[0]
                wait = due - time.monotonic()
                if wait > 0:
                    self.cond.wait(wait)
                    continue
                heapq.heappop(self.pending)
            sock.sendto(frame, peer)

    def from_guest(self):
        while True:
            frame, _ = self.guest.recvfrom(65536)
            if self.fake.wants(frame):
                self.fake.handle(frame)
            else:
                self.forward(self.slirp, self.slirp_peer, frame)

    def from_slirp(self):
        while True:
            frame, _ = self.slirp.recvfrom(65536)
            self.forward(self.guest, self.guest_peer, frame)

    def start(self):
        for target in (self.scheduler, self.from_guest, self.from_slirp):
            threading.Thread(target=target, daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp-echo", type=int, required=True)
    ap.add_argument("--udp-echo", type=int, required=True)
    ap.add_argument("--http", type=int, required=True)
    ap.add_argument("--sink", type=int)
    ap.add_argument("--https", type=int)
    ap.add_argument("--cert", help="PEM certificate chain for --https")
    ap.add_argument("--key", help="PEM private key for --https")
    ap.add_argument("--pages", help="directory served at /ns/")
    ap.add_argument("--request-log", help="file that gets one line per GET")
    ap.add_argument("--link", type=int, help="first of the four UDP ports joining the guest's NIC to slirp")
    ap.add_argument("--qmp", help="QEMU's QMP socket, for GET /linkflap")
    args = ap.parse_args()

    QMP["socket"] = args.qmp
    PAGES["dir"] = args.pages
    PAGES["log"] = args.request_log
    if args.link:
        Link(args.link).start()
    servers = [ThreadedTcp(("127.0.0.1", args.tcp_echo), TcpEcho),
               ThreadedHttp(("127.0.0.1", args.http), Http)]
    if args.sink:
        servers.append(ThreadedTcp(("127.0.0.1", args.sink), TcpSink))
    if args.https:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        servers.append(ThreadedHttps(("127.0.0.1", args.https), Http, context))
    udp = udp_echo(args.udp_echo)
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    print("host_server: ready", flush=True)
    while True:
        data, peer = udp.recvfrom(65536)
        udp.sendto(data, peer)


if __name__ == "__main__":
    sys.exit(main())
