#!/usr/bin/env python3
"""Wisp outbound HTTP using independent HTTP/TLS peers, real waits and GC."""
import gzip
import http.server
import os
import pathlib
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading

binary = str(pathlib.Path(sys.argv[1]).resolve())
payload = b"decoded:\x00\xff\x80:end"
timed_out = threading.Event()
closed = threading.Event()
requests = []


class Peer(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_HEAD(self):
        # Keep the socket open, with a representation length but no body.
        self.send_response_only(200)
        self.send_header("Content-Length", "123")
        self.end_headers()

    def do_GET(self):
        self.do_POST()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        requests.append((self.path, self.command, self.headers, body))
        if self.path == "/stall":
            timed_out.set()
            # A timeout must close the connection after draining native I/O.
            assert self.rfile.read(1) == b""
            closed.set()
            return
        if self.path == "/interim":
            self.wfile.write(b"HTTP/1.1 103 Early Hints\r\nLink: </asset>\r\n\r\n")
        if self.path == "/chunked":
            packed = gzip.compress(payload)
            self.send_response_only(207)
            self.send_header("Set-Cookie", "a=1")
            self.send_header("Set-Cookie", "b=2")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for chunk in (packed[:7], packed[7:]):
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return
        if self.path in ("/limit", "/oversized"):
            # Bound the decoded size, not just the small compressed input.
            body = gzip.compress(b"x" * (8 * 1024 * 1024 + (self.path == "/oversized")))
        elif self.path == "/interim":
            body = payload
        elif self.path == "/redirect":
            body = b"redirect not followed"
        elif self.path != "/echo":
            body = b""
        self.send_response_only(302 if self.path == "/redirect" else
                                404 if self.path == "/missing" else
                                204 if self.path == "/empty" else 200)
        if self.path in ("/limit", "/oversized"):
            self.send_header("Content-Encoding", "gzip")
        if self.path == "/redirect":
            self.send_header("Location", "/echo")
        if self.path != "/empty":
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.path != "/empty":
            self.wfile.write(body)


with tempfile.TemporaryDirectory(prefix="wisp-http-client-") as directory:
    directory = pathlib.Path(directory)
    source = directory / "client.wisp"
    peer = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    thread = threading.Thread(target=peer.serve_forever, daemon=True)
    thread.start()
    url = f"http://localhost:{peer.server_port}"
    client_env = os.environ.copy()

    def run(program, input=b"", ok=True, timeout=20, checkpoint=False):
        source.write_text(program.replace("URL", url))
        args = [binary, "run", str(source)]
        if checkpoint:
            args += ["--checkpoint", str(directory / "network.tape")]
        result = subprocess.run(args, input=input, capture_output=True,
                                timeout=timeout, env=client_env)
        assert (result.returncode == 0) == ok, (result.returncode, result.stderr)
        if ok:
            assert result.stderr == b"", result.stderr
        return result

    try:
        decoded = run('''
          (let ((r (fetch-http "URL/chunked")))
            (gc)
            (print (vector-get r 0)) (print (vector-get r 1))
            (write (vector-get r 2)))
        ''')
        assert decoded.stdout == (
            b'207\n(#<"Set-Cookie" "a=1"> #<"Set-Cookie" "b=2"> '
            b'#<"Content-Encoding" "gzip"> #<"Transfer-Encoding" "chunked">)\n'
            + payload), decoded.stdout
        direct = run('''
          (let ((r (await (vector :HTTP-FETCH
                         (vector "URL/echo" "POST" nil "direct-await")))))
            (write (vector-get r 2)))
        ''')
        assert direct.stdout == b"direct-await", direct.stdout
        echoed = run('''
          (write (vector-get (fetch-http "URL/echo" "POST"
            (list (vector "X-Test" "first")
                  (vector "X-Test" "second")) (read-bytes 9)) 2))
        ''', input=b"before\0\xff!")
        assert echoed.stdout == b"before\0\xff!", echoed.stdout
        sent = next(r for r in requests if r[0] == "/echo" and r[3] == b"before\0\xff!")
        assert sent[1] == "POST" and sent[3] == echoed.stdout, sent
        assert sent[2].get_all("X-Test") == ["first", "second"]
        assert sent[2]["Connection"] == "close"
        assert sent[2]["Host"] == f"localhost:{peer.server_port}"
        assert sent[2]["Accept-Encoding"] == "gzip, deflate"

        semantics = run('''
          (print (vector-get (fetch-http "URL/echo" "HEAD") 2))
          (print (vector-get (fetch-http "URL/empty") 2))
          (print (vector-get (fetch-http "URL/missing") 0))
          (print (vector-get (fetch-http "URL/redirect") 0))
          (write (vector-get (fetch-http "URL/interim") 2))
        ''')
        assert semantics.stdout == b'""\n""\n404\n302\n' + payload, semantics.stdout

        limit = run('(write (vector-get (fetch-http "URL/limit") 2))')
        assert limit.stdout == b"x" * (8 * 1024 * 1024), len(limit.stdout)
        rejected = run('''
          (defun code (thunk)
            (try (call thunk) (error (why k) (vector-get (head why) 2))))
          (print (code (fn () (fetch-http "URL/echo" (read-bytes 13)))))
          (print (code (fn () (fetch-http "URL/echo" "GET"
                              (list (vector "X-Test" (read-bytes 17)))))))
          (print (code (fn () (fetch-http "URL/echo" "POST"
                              (list (vector "content-length" "999")) "body"))))
          (print (code (fn () (fetch-http "URL/echo" "GET" (list 42)))))
          (print (code (fn () (fetch-http "URL/echo" "GET"
                              (list (vector "X-Test" 42))))))
          (print (code (fn () (fetch-http "URL/oversized"))))
        ''', input=b"GET\r\nInjectedok\r\nInjected: yes")
        assert rejected.stdout == b":INVALID-ARGUMENT\n" * 5 + b":IO\n", rejected.stdout
        checkpoint = run('(fetch-http "URL/echo")', ok=False, checkpoint=True)
        assert b"NOT-REPLAYABLE" in checkpoint.stderr, checkpoint.stderr
        assert not (directory / "network.tape").exists()

        # Independent TLS peer also exercises requests spanning TLS records.
        openssl = shutil.which("openssl")
        if openssl:
            key, cert = directory / "key.pem", directory / "cert.pem"
            ca_key, ca = directory / "ca-key.pem", directory / "ca.pem"
            csr = directory / "request.pem"
            config = directory / "cert.cnf"
            config.write_text('''
              [req]
              distinguished_name = subject
              x509_extensions = extensions
              [subject]
              [extensions]
              basicConstraints = critical,CA:FALSE
              keyUsage = critical,digitalSignature
              extendedKeyUsage = serverAuth
              subjectAltName = DNS:localhost
              [ca]
              basicConstraints = critical,CA:TRUE
              keyUsage = critical,keyCertSign,cRLSign
            ''')
            subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048",
                            "-nodes", "-keyout", str(ca_key),
                            "-out", str(ca), "-subj", "/CN=Test Root", "-days", "1",
                            "-config", str(config), "-extensions", "ca"],
                           check=True, capture_output=True, timeout=20)
            subprocess.run([openssl, "req", "-new", "-newkey", "rsa:2048",
                            "-nodes", "-keyout", str(key),
                            "-out", str(csr), "-subj", "/CN=localhost",
                            "-config", str(config)],
                           check=True, capture_output=True, timeout=20)
            subprocess.run([openssl, "x509", "-req", "-in", str(csr),
                            "-CA", str(ca), "-CAkey", str(ca_key),
                            "-out", str(cert), "-days", "1",
                            "-extfile", str(config), "-extensions", "extensions"],
                           check=True, capture_output=True, timeout=20)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_3
            context.load_cert_chain(cert, key)
            server_names = []
            context.set_servername_callback(lambda socket, name, ctx: server_names.append(name))
            peer.socket = context.wrap_socket(peer.socket, server_side=True)
            url = f"https://localhost:{peer.server_port}"
            rejected_program = '''
              (print (try (fetch-http "URL/echo")
                          (error (why k) (vector-get (head why) 2))))
            '''
            request_count = len(requests)
            untrusted = run(rejected_program)
            assert untrusted.stdout == b":IO\n", untrusted.stdout
            assert len(requests) == request_count, "sent HTTP before authenticating peer"
            # Explicitly trust the test CA, not certificates supplied by peers.
            client_env["SSL_CERT_FILE"] = str(ca)
            large = b"TLS:\0\xff" * 5000
            secure = run('''
              (write (vector-get (fetch-http "URL/echo" "POST" nil
                                    (read-bytes 30000)) 2))
            ''', input=large)
            assert secure.stdout == large, (len(secure.stdout), secure.stderr)
            assert server_names[-1] == "localhost", server_names
            # Connecting to the same trusted peer by IP must not use a DNS SAN.
            url = f"https://127.0.0.1:{peer.server_port}"
            request_count = len(requests)
            mismatch = run(rejected_program)
            assert mismatch.stdout == b":IO\n", mismatch.stdout
            assert len(requests) == request_count, "sent HTTP to wrong virtual host"
            assert server_names[-1] is None, "IP literal was sent as DNS SNI"
            url = f"https://localhost:{peer.server_port}"
            print("Wisp HTTP client TLS: trusted 30 KB POST, SNI, untrusted and wrong-host rejection passed")
        else:
            print("Wisp HTTP client TLS: skipped (openssl fixture tool unavailable)")

        # The outbound deadline raises a guest condition and the host remains
        # usable. The peer observes EOF, rather than an abandoned socket.
        timeout = run('''
          (print (try (fetch-http "URL/stall")
                      (error (why k) (vector-get (head why) 2))))
          (print (vector-get (fetch-http "URL/echo") 0))
        ''', timeout=40)
        assert timed_out.is_set() and timeout.stdout == b":TIMEOUT\n200\n", timeout.stdout
        assert closed.wait(5), "timed-out request left its socket open"
    finally:
        peer.shutdown()
        peer.server_close()
        thread.join(timeout=5)

print("Wisp HTTP client: composite await, binary POST, chunked gzip, duplicate headers, GC, HEAD/204, errors, limits, timeout and checkpoint rejection passed")
