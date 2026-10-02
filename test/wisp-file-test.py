#!/usr/bin/env python3
"""--dir grants, file effects confined beneath them, and streamed file bodies."""
import http.client
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())


def run(source, *args, ok=True, cwd=None):
    result = subprocess.run([binary, "run", str(source), *args],
                            capture_output=True, timeout=20, cwd=cwd)
    assert (result.returncode == 0) == ok, result.stderr
    return result


with tempfile.TemporaryDirectory(prefix="wisp-file-") as directory:
    directory = pathlib.Path(directory)
    site = directory / "public"
    (site / "sub").mkdir(parents=True)
    (directory / "secret.txt").write_text("outside")
    (site / "a.txt").write_bytes(b"hello\0\xff")
    (site / "sub" / "index.html").write_text("<p>x")
    (site / "sub" / "link").symlink_to("../a.txt")
    (site / "up").symlink_to("..")
    os.mkfifo(site / "fifo")
    big = os.urandom(9 * 1024 * 1024)  # past the 8 MiB string body limit
    (site / "big.bin").write_bytes(big)
    source = directory / "files.wisp"

    source.write_text('''
      (defun code (thunk)
        (try (call thunk) (catch (e k) (vector-get (head e) 2))))
      (write (read-file "site/a.txt") "|")
      (print (vector-get (file-status "site/a.txt") 0))
      (print (vector-get (file-status "site/a.txt") 1))
      (print (vector-get (file-status "site") 0))
      (print (vector-get (file-status "site/up") 0))
      (print (file-status "site/missing"))
      (print (list-directory "site"))
      (print (list-directory "site/sub"))
      (print (list (code (fn () (read-file "site/sub/link")))
                   (code (fn () (read-file "site/up/secret.txt")))
                   (code (fn () (read-file "site/../secret.txt")))
                   (code (fn () (read-file "/etc/passwd")))
                   (code (fn () (read-file "other/a.txt")))
                   (code (fn () (read-file "site/missing")))
                   (code (fn () (read-file "site/fifo")))
                   (code (fn () (read-file "site/sub")))
                   (code (fn () (list-directory "site/a.txt")))))
      (print (list (content-type "site/sub/index.html")
                   (content-type "x/y.tar.wasm") (content-type "x/README")))
    ''')
    raw = run(source, "--dir", f"site={site}").stdout
    assert raw.startswith(b"hello\0\xff|:FILE\n"), raw[:40]
    out = raw.split(b"\n", 1)[1].decode().split("\n")
    assert out[0:7] == [
        '"7"', ":DIRECTORY", ":SYMLINK", "NIL",
        '("a.txt" "big.bin" "fifo" "sub" "up")', '("index.html" "link")',
        "(:NOT-CAPABLE :NOT-CAPABLE :INVALID-ARGUMENT :NOT-CAPABLE"
        " :NOT-CAPABLE :NOT-FOUND :INVALID-ARGUMENT :INVALID-ARGUMENT"
        " :NOT-FOUND)"], out
    assert out[7] == ('("text/html; charset=utf-8" "application/wasm"'
                      ' "application/octet-stream")'), out[7]

    # Without a grant there is no filesystem at all; --dir PATH names itself.
    source.write_text('(write (try (read-file "public/a.txt")'
                      ' (catch (e k) "denied")))')
    assert run(source).stdout == b"denied"
    assert run(source, "--dir", "public", cwd=directory).stdout == b"hello\0\xff"
    for bad in (["--dir", "a/b"], ["--dir", "x=/nonexistent"],
                ["--dir", f"x={site}", "--dir", f"x={site}"]):
        assert b"--dir" in run(source, *bad, ok=False).stderr, bad

    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    source.write_text(f'''
      (defroute ("GET" "static" &rest path)
        (serve-file (join-strings "/" (cons "site" path))))
      (defroute ("GET" "missing")
        (set-response-body! (vector :file "site/missing")))
      (serve-http {port} #'route-request)
    ''')
    server = subprocess.Popen([binary, "run", str(source), "--dir", f"site={site}"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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

        def request(path, method="GET"):
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
            try:
                client.putrequest(method, path, skip_accept_encoding=True)
                client.endheaders()
                response = client.getresponse()
                return (response.status, response.getheader("Content-Type"),
                        response.getheader("Content-Length"), response.read())
            finally:
                client.close()

        assert request("/static/sub/index.html") == (
            200, "text/html; charset=utf-8", "4", b"<p>x")
        assert request("/static/big.bin") == (
            200, "application/octet-stream", str(len(big)), big)
        assert request("/static/big.bin", "HEAD")[2:] == (str(len(big)), b"")
        for path in ("/static/sub", "/static/sub/link", "/static/up/secret.txt",
                     "/static/fifo", "/static/..", "/static/../secret.txt",
                     "/static/nope"):
            assert request(path)[0] == 404, path
        assert request("/missing")[0] == 500
        assert server.poll() is None
    finally:
        server.terminate()
        server.communicate(timeout=5)

print("Wisp files: --dir grants, confinement, symlinks, FIFOs, status, listing, streamed bodies passed")
