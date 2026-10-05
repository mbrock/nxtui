#!/usr/bin/env python3
"""Bounded, deterministic ws/wss fixtures for the portable NXT consumer.

No external services or credentials. --wine uses the installed UWP consumer.
The server independently decodes masks/lengths and checks client payloads.
"""
import argparse
import base64
import hashlib
import ipaddress
import os
from pathlib import Path
import re
import signal
import socket
import socketserver
import ssl
import struct
import subprocess
import tempfile
import threading
import time

from contextlib import nullcontext
from runpy import run_path

certificate = run_path(str(Path(__file__).with_name("network-fixture.py")))["certificate"]


def frame(opcode, payload=b"", fin=True):
    head = bytes([(128 if fin else 0) | opcode])
    n = len(payload)
    head += (bytes([n]) if n < 126 else b"\x7e" + struct.pack("!H", n)
             if n <= 65535 else b"\x7f" + struct.pack("!Q", n))
    return head + payload


class Handler(socketserver.StreamRequestHandler):
    def exact(self, n):
        data = self.rfile.read(n)
        if len(data) != n:
            raise EOFError()
        return data

    def client_frame(self):
        first, second = self.exact(2)
        assert first & 128 and not first & 112, "client flags/FIN"
        assert second & 128, "unmasked client frame"
        n = second & 127
        if n == 126:
            n = struct.unpack("!H", self.exact(2))[0]
            assert n >= 126, "noncanonical client 16-bit length"
        elif n == 127:
            n = struct.unpack("!Q", self.exact(8))[0]
            assert 65536 <= n < 2**63, "noncanonical client 64-bit length"
        assert n <= 1024 * 1024, "fixture client size limit"
        mask = self.exact(4)
        raw = self.exact(n)
        return first & 15, bytes(b ^ mask[i % 4] for i, b in enumerate(raw))

    def handle(self):
        self.connection.settimeout(15)
        self.path = ""
        try:
            self.session()
        except (EOFError, ConnectionError, OSError):
            if self.path in ("/exchange", "/fragments", "/close-mid"):
                self.server.errors.append("premature EOF on " + self.path)
        except Exception as exc:
            self.server.errors.append(repr(exc))

    def session(self):
        request = self.rfile.readline(4096).decode("ascii").strip().split()
        assert len(request) == 3 and request[0] == "GET" and request[2] == "HTTP/1.1"
        path = request[1]
        self.path = path
        headers = {}
        while True:
            line = self.rfile.readline(4096)
            if line == b"\r\n":
                break
            assert line, "truncated request"
            name, value = line.decode("ascii").split(":", 1)
            headers[name.lower()] = value.strip()
        assert headers["upgrade"].lower() == "websocket"
        assert headers["connection"].lower() == "upgrade"
        assert headers["sec-websocket-version"] == "13"
        port = self.server.server_address[1]
        host = self.server.advertise
        if port != (443 if self.server.context else 80):
            host += f":{port}"
        assert headers["host"] == host
        assert len(base64.b64decode(headers["sec-websocket-key"], validate=True)) == 16
        self.server.seen.add(path)
        if path == "/stall-upgrade":
            assert self.connection.recv(1) == b"", "cancelled Upgrade did not close"
            return
        accept = base64.b64encode(hashlib.sha1(
            (headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        status = "HTTP/1.1 101 Switching Protocols\r\n"
        fields = ["Upgrade: WebSocket", "Connection: keep-alive, UpGrAdE",
                  "Sec-WebSocket-Accept: " + accept]
        if path == "/bad-accept":
            fields[2] = "Sec-WebSocket-Accept: wrong"
        elif path == "/bad-missing-accept":
            fields.pop()
        elif path == "/bad-status":
            status = "HTTP/1.1 200 OK\r\n"
        elif path == "/bad-status-digits":
            status = "HTTP/1.1 0101 Switching Protocols\r\n"
        elif path == "/bad-status-control":
            status = "HTTP/1.1 101 reason\0\r\n"
        elif path == "/bad-version":
            status = "HTTP/1.0 101 Switching Protocols\r\n"
        elif path == "/bad-connection":
            fields[1] = "Connection: keep-alive"
        elif path == "/bad-upgrade":
            fields[0] = "Upgrade: h2c"
        elif path == "/bad-duplicate":
            fields.append(fields[2])
        elif path == "/bad-extension":
            fields.append("Sec-WebSocket-Extensions: permessage-deflate")
        elif path == "/bad-subprotocol":
            fields.append("Sec-WebSocket-Protocol: not-offered")
        elif path == "/bad-malformed":
            fields.append("broken-header")
        elif path == "/bad-folded":
            fields.append(" Sec-WebSocket-Accept: " + accept)
        if path == "/stall-send":
            # Lock the receive buffer before the ready barrier; otherwise
            # OS autotuning can absorb much more than this fixture intends.
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            receive_buffer = self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
            started = time.monotonic()
            print(f"STALL-SEND ready time={time.time():.3f} peer={self.client_address} "
                  f"tls={bool(self.server.context)} rcvbuf={receive_buffer}", flush=True)
        # Send the head and a frame together: Upgrade must preserve buffered
        # post-header bytes. Fragment fixtures then split UTF-8 across frames.
        head = (status + "\r\n".join(fields) + "\r\n\r\n").encode()
        if path == "/fragments":
            self.connection.sendall(head + frame(1, b"A\xf0", False)
                                    + frame(9, b"\x13\0ping") + frame(10, b"unsolicited"))
            assert self.client_frame() == (10, b"\x13\0ping"), "automatic pong payload"
            self.connection.sendall(frame(0, b"\x9f\x8c", False) + frame(9)
                                    + frame(0, b"\x99Z") + frame(2, b"\0\xff", False)
                                    + frame(0, b"\x13\0") + frame(8))
            assert self.client_frame() == (10, b""), "empty ping reply"
            assert self.client_frame() == (8, b""), "server Close not echoed"
            return
        self.connection.sendall(head)
        if path == "/exchange":
            while True:
                opcode, payload = self.client_frame()
                assert opcode in (1, 2, 8, 9), "unexpected client opcode"
                if opcode == 1:
                    payload.decode("utf-8", errors="strict")
                self.connection.sendall(frame(10 if opcode == 9 else opcode, payload))
                if opcode == 8:
                    return
        elif path.startswith("/stall-"):
            suffix = {"header": b"\x82", "payload": b"\x82\x05ab",
                      "fragment": frame(1, b"a", False), "overlap": b"", "send": b""}[path[7:]]
            self.connection.sendall(frame(10, b"ready") + suffix)
            if path == "/stall-send":
                # Never read client data. Hold beyond the probe's 15s deadline
                # so a fixture timeout cannot masquerade as cancellation.
                stopped = self.server.finished.wait(30)
                print(f"STALL-SEND end time={time.time():.3f} peer={self.client_address} "
                      f"tls={bool(self.server.context)} elapsed_ms={(time.monotonic() - started) * 1000:.0f} "
                      f"reason={'server-stopping' if stopped else 'hold-expired'}", flush=True)
            else:
                assert self.connection.recv(1) == b"", "cancelled receive did not close"
            return
        else:
            malformed = {
                "rsv": b"\xc1\0", "mask": b"\x81\x80", "opcode": b"\x83\0",
                "continuation": frame(0), "new-message": frame(1, b"a", False) + frame(2),
                "control-fin": frame(9, fin=False), "control-size": b"\x89\x7e",
                "length16": b"\x82\x7e\0\x01x", "length64": b"\x82\x7f" + struct.pack("!Q", 126),
                "high-bit": b"\x82\x7f\x80" + b"\0" * 7,
                "utf8": frame(1, b"\xc0\xaf"), "utf8-surrogate": frame(1, b"\xed\xa0\x80"),
                "utf8-large": frame(1, b"\xf4\x90\x80\x80"), "utf8-partial": frame(1, b"\xe2\x82"),
                "close-one": frame(8, b"x"), "close-code": frame(8, b"\x03\xed"),
                "close-reason": frame(8, b"\x03\xe8\xff"), "eof": b"", "truncated": b"\x82\x05ab",
                "limit": b"\x82\x07", "fragment-limit": frame(1, b"1234", False) + b"\x80\x03",
            }.get(path[5:])
            if malformed is not None:
                self.connection.sendall(malformed)
                return
            if path == "/close-mid":
                self.connection.sendall(frame(1, b"partial", False) + frame(8, b"\x03\xe8bye"))
                assert self.client_frame() == (8, b"\x03\xe8bye")


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, context=None, bind="127.0.0.1", port=0, advertise="localhost"):
        super().__init__((bind, port), Handler)
        self.context = context
        self.advertise = advertise
        self.errors = []
        self.seen = set()
        self.finished = threading.Event()

    def get_request(self):
        connection, address = super().get_request()
        if self.context:
            connection.settimeout(15)
            try:
                connection = self.context.wrap_socket(connection, server_side=True)
            except OSError:
                connection.close()
                raise
        return connection, address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine")
    parser.add_argument("--serve", action="store_true", help="serve until stopped; do not launch a probe")
    parser.add_argument("--directory", type=Path, help="new private directory for persistent fixture keys/PEMs")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--advertise", default="localhost", help="client-reachable DNS name or IPv4; used for SAN and Host")
    parser.add_argument("--ws-port", type=int, default=0)
    parser.add_argument("--wss-port", type=int, default=0)
    parser.add_argument("probe", type=Path, nargs="?")
    args = parser.parse_args()
    if args.serve == bool(args.probe) or (args.serve and not args.directory):
        parser.error("supply a probe OR --serve --directory NEW_DIRECTORY")
    if not re.fullmatch(r"[A-Za-z0-9.-]+", args.advertise):
        parser.error("advertised identity must be a DNS name or IPv4")
    try:
        ipaddress.IPv4Address(args.advertise)
        san_type = "IP"
    except ValueError:
        san_type = "DNS"
    with (nullcontext(args.directory) if args.serve else
          tempfile.TemporaryDirectory(prefix="nxt-websocket-")) as temp:
        directory = Path(temp)
        if args.serve:
            directory.mkdir(parents=True, mode=0o700)  # Never overwrite existing keys.
        certificate(directory, "server", args.advertise, san_type)
        certificate(directory, "wrong", "unrelated.invalid")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(directory / "server.pem", directory / "server.key")
        servers = [Server(bind=args.bind, port=args.ws_port, advertise=args.advertise),
                   Server(context, args.bind, args.wss_port, args.advertise)]
        threads = [threading.Thread(target=s.serve_forever) for s in servers]
        for thread in threads:
            thread.start()
        def client_path(path):
            return ("Z:" if args.wine else "") + str(path.resolve())
        ws = f"ws://{args.advertise}:{servers[0].server_address[1]}"
        wss = f"wss://{args.advertise}:{servers[1].server_address[1]}"
        result = 0
        try:
            if args.serve:
                print(f"WS_BASE={ws}\nWSS_BASE={wss}\nCA_FILE={directory.resolve() / 'server.pem'}\n"
                      f"WRONG_CA_FILE={directory.resolve() / 'wrong.pem'}", flush=True)
                stop = threading.Event()
                signal.signal(signal.SIGTERM, lambda *_: stop.set())
                try:
                    stop.wait()
                except KeyboardInterrupt:
                    pass
            else:
                command = ([args.wine] if args.wine else []) + [str(args.probe.resolve()),
                    ws, wss, client_path(directory / "server.pem"), client_path(directory / "wrong.pem")]
                result = subprocess.run(command, timeout=180, env=os.environ).returncode
        finally:
            for server in servers:
                server.finished.set()
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()
        if not args.serve:
            for server in servers:
                for stage in ("upgrade", "header", "payload", "fragment", "send", "overlap"):
                    if "/stall-" + stage not in server.seen:
                        server.errors.append("cancellation stage not reached: " + stage)
        errors = [error for server in servers for error in server.errors]
        for error in errors:
            print("FIXTURE FAIL:", error)
        print(f"FIXTURE: {len(errors)} server assertion failures")
        return result or bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
