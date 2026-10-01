#!/usr/bin/env python3
"""Real HTTP through the guest host, including moving GC and cancellation."""
import concurrent.futures
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())

with tempfile.TemporaryDirectory(prefix="wisp-http-") as directory:
    directory = pathlib.Path(directory)
    source = directory / "server.wisp"
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    source.write_text('''
      (defvar released nil)
      (defvar waiting nil)
      (defun await-release ()
        (if released nil (do (sleep-ms 1) (await-release))))
      (defun handler ()
        (cond
          ((equal? (request-path) "/wait")
           (do (set! waiting t) (await-release) (gc)
               (set-response-body! (request-path))))
          ((equal? (request-path) "/state")
           (set-response-body! (if waiting "waiting" "starting")))
          ((equal? (request-path) "/release")
           (do (set! released t) (set-response-body! "released")))
          ((equal? (request-path) "/early")
           (do (send! :respond (response 201 nil "early"))
               (error "must not run")))
          ((equal? (request-path) "/ignored")
           (response 418 nil "ordinary return is ignored"))
          ((equal? (request-path) "/error") (error "private failure"))
          ((equal? (request-path) "/injection")
           (add-header! "X-Test" (request-text)))
          ((equal? (request-path) "/read")
           (let ((line (read-line)))
             (set-response-body! (if line line "EOF"))))
          ((equal? (request-path) "/timeout")
           (do (sleep-ms 30500) (write "late side effect")
               (set-response-body! "too late")))
          (t
           (do (sleep-ms 5) (gc)
               (add-header! "Set-Cookie" "a=1")
               (add-header! "Set-Cookie" "b=2")
               (set-response-body!
                 (string-append (request-method) "|" (request-path) "|"
                   (request-query-string) "|" (or (request-header "x-MiXeD") "")
                   "|" (request-text)))))))
      (serve-http PORT #'handler)
    '''.replace("PORT", str(port)))
    server = subprocess.Popen([binary, "run", str(source)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 20
        while True:
            assert server.poll() is None, server.communicate()
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    break
            except OSError:
                assert time.monotonic() < deadline, "server did not start"
                time.sleep(.02)

        def request(path, method="GET", body=None, headers=None, chunked=False):
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=40)
            try:
                client.request(method, path, body=body, headers=headers or {},
                               encode_chunked=chunked)
                response = client.getresponse()
                return response.status, response.getheaders(), response.read()
            finally:
                client.close()

        # This tests actual overlap, without relying on fast wall-clock timing.
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            pending = pool.submit(request, "/wait")
            deadline = time.monotonic() + 10
            while request("/state")[2] != b"waiting":
                assert time.monotonic() < deadline
            released = request("/release")
            assert released[2] == b"released", released
            assert pending.result(timeout=10)[2] == b"/wait"

        def echo(i):
            payload = b"body:" + bytes([0, 255, 65 + i])
            status, headers, body = request(f"/item-{i}?n={i}", "POST", payload,
                                            {"X-MIXED": f"header-{i}"})
            assert status == 200, (status, body)
            assert body == f"POST|/item-{i}|n={i}|header-{i}|".encode() + payload, body
            assert [v for k, v in headers if k.lower() == "set-cookie"] == ["a=1", "b=2"]

        with concurrent.futures.ThreadPoolExecutor(12) as pool:
            list(pool.map(echo, range(24)))
        status, _, body = request("/chunks", "POST", [b"abc", b"\0\xff"], chunked=True)
        assert status == 200 and body == b"POST|/chunks|||abc\0\xff", (status, body)
        assert request("/early")[::2] == (201, b"early")
        assert request("/ignored")[::2] == (200, b"")
        assert request("/error")[::2] == (500, b"Internal Server Error\n")
        assert request("/injection", "POST", b"ok\r\nInjected: yes")[0] == 500
        assert request("/head", "HEAD")[2] == b""

        # The C++ server's 30s handler deadline must cancel both guest
        # timers and blocked console I/O, including its stream ownership.
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            blocked = pool.submit(request, "/read")
            assert request("/timeout")[0] == 504
            assert blocked.result(timeout=5)[0] == 504
        time.sleep(.8)
        server.stdin.write(b"available\n")
        server.stdin.flush()
        assert request("/read")[::2] == (200, b"available")
        echo(7)
        assert server.poll() is None
    finally:
        server.terminate()
        try:
            stdout, stderr = server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            stdout, stderr = server.communicate()
        assert stdout == b"" and stderr == b"", (stdout, stderr)

    # Opening network authority is explicitly incompatible with checkpoint
    # mode. The listener must not appear in a supposedly replayable image.
    source.write_text(f"(join (serve-http {port} (fn () nil)))")
    tape = directory / "network.tape"
    rejected = subprocess.run([binary, "run", str(source), "--checkpoint", str(tape)],
                              capture_output=True, timeout=20)
    assert rejected.returncode != 0 and b"NOT-REPLAYABLE" in rejected.stderr, rejected.stderr
    assert not tape.exists()

print("Wisp HTTP: overlapping jobs, request isolation, GC, binary bodies, response policy, timeout and checkpoint rejection passed")
