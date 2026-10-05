#!/usr/bin/env python3
"""Run NXT's portable probe against a disposable, verified TLS 1.3 fixture.

No real credentials or external requests. --wine exercises the installed
MSVC/UWP binary; the same fixture serves Linux and Windows consumers.
"""
import argparse
import gzip
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time


def event(kind, **fields):
    data = json.dumps({"type": kind, **fields}, separators=(",", ":"))
    return f"event: {kind}\ndata: {data}\n\n".encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # Never log requests, authorization headers or request bodies.

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            valid = (request["model"] == "gpt-6-luna" and request["stream"] is True
                     and request["input"] == "fixture"
                     and self.headers["Authorization"] == "Bearer fixture-not-a-secret"
                     and self.headers["Host"] == f"localhost:{self.server.server_port}")
            if not valid:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if self.path == "/status":
                self.send_response(429)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json" if self.path == "/type"
                             else "text/event-stream; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            if self.path == "/stream":
                self.send_header("Content-Encoding", "gzip")
            self.end_headers()
            if self.path == "/stall":
                self.chunk(event("response.output_text.delta", delta="partial"))
                self.connection.settimeout(10)
                # Wait for the cancelling client to close, not an arbitrary delay.
                self.connection.recv(1)
                return
            body = event("response.output_text.delta", delta="alpha\0")
            body += event("response.output_text.delta", delta="omega")
            if self.path != "/truncated":
                body += event("response.completed", response={
                    "id": "fixture-response", "status": "completed", "output": [
                        {"type": "message", "role": "assistant", "content": [
                            {"type": "output_text", "text": "alpha\0omega"}]}]})
            if self.path == "/stream":
                body = gzip.compress(body)
            # Boundaries cut through gzip headers, SSE syntax, and JSON escapes.
            widths = (1, 2, 7, 3, 19)
            offset = 0
            index = 0
            while offset < len(body):
                width = widths[index % len(widths)]
                self.chunk(body[offset:offset + width])
                offset += width
                index += 1
                time.sleep(.001)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError, OSError):
            pass  # Expected when a client rejects the head or cancels the stream.
        finally:
            self.close_connection = True

    def chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()


def certificate(directory, name, hostname):
    # The development shell's AWS-LC CLI supports RSA req and -config, but
    # not OpenSSL's EC -pkeyopt/-addext flags. Exercise both providers alike.
    config = directory / f"{name}.conf"
    config.write_text("[req]\ndistinguished_name=dn\n[dn]\n[ext]\n"
                      f"subjectAltName=DNS:{hostname}\n"
                      "basicConstraints=critical,CA:TRUE\n"
                      "keyUsage=critical,digitalSignature,keyCertSign\n")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                    "-nodes", "-days", "1", "-subj", f"/CN={hostname}",
                    "-config", str(config), "-extensions", "ext",
                    "-keyout", str(directory / f"{name}.key"),
                    "-out", str(directory / f"{name}.pem")],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", help="Wine executable for a Windows consumer")
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nxt-network-") as temp:
        directory = Path(temp)
        certificate(directory, "server", "localhost")
        certificate(directory, "wrong", "unrelated.invalid")
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(directory / "server.pem", directory / "server.key")
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        def client_path(path):
            path = str(path.resolve())
            return "Z:" + path if args.wine else path
        command = ([args.wine] if args.wine else []) + [str(args.probe.resolve()),
            "--fixture", "localhost", str(server.server_port),
            client_path(directory / "server.pem"), client_path(directory / "wrong.pem")]
        try:
            result = subprocess.run(command, timeout=90, env=os.environ)
            return result.returncode
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
