#!/usr/bin/env python3
"""DEFROUTE matching, precedence and fallbacks, in-process and over HTTP."""
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())

ROUTES = '''
  (defroute ("GET" "")
    (set-response-body! "root"))
  (defroute ("GET" "git" repo "info" "refs")
    (set-response-body! (string-append "refs:" repo)))
  (defroute ("POST" "git" repo "git-upload-pack")
    (set-response-body! (string-append "upload:" repo ":" (request-text))))
  (defroute ("GET" "git" repo ref)
    (set-response-body! (string-append "ref:" repo ":" ref)))
  (defroute ("GET" "skip" _ last)
    (set-response-body! last))
  (defroute ("GET" "static" &rest path)
    (set-response-body! (join-strings "," (cons "static" path))))
  (defroute (method "any" x)
    (set-response-body! (string-append method ":" x)))
  (defroute ("OPTIONS" "*")
    (set-response-status! 204))
  (defroute ("GET" "early")
    (send! :respond (response 201 nil "early"))
    (error "must not run"))
  ;; Redefinition replaces the handler but keeps the original position,
  ;; ahead of the later, more general GIT REPO REF route.
  (defroute ("GET" "git" repo "info" "refs")
    (set-response-body! (string-append "refs2:" repo)))
  (defroute ("HEAD" "only-head")
    (add-header! "X-Head" "yes"))
'''

with tempfile.TemporaryDirectory(prefix="wisp-router-") as directory:
    directory = pathlib.Path(directory)
    source = directory / "router.wisp"
    cases = [
        ("GET", "/", "200 root"),
        ("GET", "/git/abc/info/refs", "200 refs2:abc"),
        ("POST", "/git/abc/git-upload-pack", "200 upload:abc:body"),
        ("GET", "/git/abc/HEAD", "200 ref:abc:HEAD"),
        ("GET", "/git/abc", "404 Not Found\n"),
        ("GET", "/git/abc/info/refs/more", "404 Not Found\n"),
        ("GET", "/skip/one/two", "200 two"),
        ("GET", "/static", "200 static"),
        ("GET", "/static/", "200 static,"),
        ("GET", "/static/a/b%20c", "200 static,a,b%20c"),
        ("DELETE", "/any/1", "200 DELETE:1"),
        ("OPTIONS", "*", "204 "),
        ("GET", "/early", "201 early"),
        ("HEAD", "/git/abc/HEAD", "200 ref:abc:HEAD"),
        ("HEAD", "/only-head", "200 X-Head=yes"),
        ("DELETE", "/git/abc/info/refs", "405 Allow=GET, HEAD"),
        ("GET", "/git/abc/git-upload-pack", "200 ref:abc:git-upload-pack"),
        ("PUT", "/git/abc/git-upload-pack", "405 Allow=POST, GET, HEAD"),
        ("GET", "/only-head", "405 Allow=HEAD"),
    ]
    # The host splits and decodes segments; these paths need no decoding.
    def segments(path):
        parts = path[1:].split("/") if path.startswith("/") else [path]
        return "(list " + " ".join(f'"{x}"' for x in parts) + ")"
    checks = "\n".join(
        f'(show (%nxt-http-handle #\'route-request (make-http-request '
        f'"{m}" "{p}" "" nil "body" {segments(p)})))'
        for m, p, _ in cases)
    source.write_text(ROUTES + '''
      (defun show (r)
        (write (print-to-string (http-response-status r)) " "
          (if (http-response-headers r)
              (join-strings ","
                (map (fn (h) (string-append (vector-get h 0) "="
                                            (vector-get h 1)))
                     (http-response-headers r)))
            (or (http-response-body r) ""))
          "|"))
    ''' + checks)
    result = subprocess.run([binary, "run", str(source)],
                            capture_output=True, timeout=20)
    assert result.returncode == 0 and result.stderr == b"", result.stderr
    got = result.stdout.decode().split("|")[:-1]
    for (method, path, expected), actual in zip(cases, got, strict=True):
        assert actual == expected, (method, path, expected, actual)

    # The 405 Allow header must pass the native response validation.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    source.write_text(ROUTES + f"(serve-http {port} #'route-request)")
    server = subprocess.Popen([binary, "run", str(source)],
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

        def request(method, path, body=None):
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
            try:
                client.request(method, path, body=body)
                response = client.getresponse()
                return response.status, response.getheader("Allow"), response.read()
            finally:
                client.close()

        assert request("POST", "/git/r/git-upload-pack", b"x") == (
            200, None, b"upload:r:x")
        assert request("DELETE", "/git/r/info/refs") == (
            405, "GET, HEAD", b"Method Not Allowed\n")
        assert request("GET", "/nope") == (404, None, b"Not Found\n")
        # The host percent-decodes segments after splitting at "/".
        assert request("GET", "/static/a/b%20c") == (200, None, b"static,a,b c")
        assert request("GET", "/static/a%2Fb") == (200, None, b"static,a/b")
        assert request("GET", "/hello%zz")[0] == 400
        assert request("GET", "/static/%4")[0] == 400
    finally:
        server.terminate()
        server.communicate(timeout=5)

print("Wisp HTTP router: patterns, precedence, redefinition, HEAD, 404/405, percent-decoding passed")
