#!/usr/bin/env python3
"""Process boundaries, real pipes/timers, and host checkpoint policy."""
import pathlib
import os
import re
import resource
import stat
import struct
import subprocess
import sys
import tempfile
import time
import zlib

binary = str(pathlib.Path(sys.argv[1]).resolve())


def command(*args, input=b"", ok=True, timeout=20):
    result = subprocess.run(
        [binary, *map(str, args)], input=input, capture_output=True, timeout=timeout
    )
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="wisp-host-") as directory:
    directory = pathlib.Path(directory)
    source = directory / "checkpoint.wisp"
    tape = directory / "timer.tape"

    repl = command("repl", input=b"(+ 17 25)\nunbound\n(+ 19 4)\nnil\n")
    assert repl.stdout == b"42\n23\nNIL\n", repl.stdout
    assert b"UNBOUND-VARIABLE" in repl.stderr, repl.stderr

    # One synchronous form discards over 312 MiB of strings. There is no
    # await or guest GC until PRINT at the end: boundary-only collection
    # cannot pass the Linux address-space cap. This is not an RSS target.
    # Keep lexical data, partially evaluated arguments, and a multi-shot
    # continuation alive across the automatic collections.
    source.write_text('''
      (defun churn (n payload)
        (if (eq? n 0) 'finished
            (do (string-append payload payload)
                (churn (- n 1) payload))))
      (let ((kept (vector (string-append "keep" "-me") 19))
            (resume (call-with-prompt 'save
                      (fn () (list 11 (send! 'save nil) 31))
                      (fn (value k) k))))
        (print (list (list 'before kept)
                     (churn 20000 "PAYLOAD") kept
                     (call resume 7) (call resume 23))))
    '''.replace("PAYLOAD", "x" * 8192))

    def limit_memory():
        # RLIMIT_AS is enforced on Linux; elsewhere still check the roots
        # and continuation results without imposing a nonportable limit.
        if sys.platform.startswith("linux"):
            limit = 256 * 1024 * 1024
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

    transient = subprocess.run([binary, "run", str(source)],
                               capture_output=True, timeout=30,
                               preexec_fn=limit_memory)
    assert transient.returncode == 0, transient.stderr
    assert transient.stderr == b"", transient.stderr
    assert transient.stdout == (
        b'((BEFORE #<"keep-me" 19>) FINISHED #<"keep-me" 19> '
        b'(11 7 31) (11 23 31))\n'
    ), transient.stdout

    source.write_text('(print (await (vector :timer 1)))')
    assert command("run", source).stdout == b"NIL\n"

    source.write_text('''
      (write "before\\n")
      (let ((x 35)) (sleep-ms 4000) (print (+ x 7)))
      (defpackage after (:use wisp))
      (in-package after)
      (print 'finished)
    ''')
    stopped = command("run", source, "--checkpoint", tape)
    assert stopped.stdout == b"before\n" and stopped.stderr == b"", stopped
    assert stat.S_IMODE(tape.stat().st_mode) == 0o600
    original = tape.read_bytes()
    assert original[:8] == b"NXWISP\r\n", "checkpoint writes must remain raw"
    info = command("inspect", tape)
    assert b"request: #<:TIMER 4000>" in info.stdout, info.stdout
    assert re.search(rb'request-id: "[1-9][0-9]*"', info.stdout), info.stdout
    deadline = int(re.search(rb'deadline-unix-ms: "([0-9]+)"', info.stdout)[1])
    assert deadline > time.time() * 1000, "fixture timer already expired"
    disabled = command("restore", tape, ok=False)
    assert disabled.stdout == b"" and b"effects disabled" in disabled.stderr
    assert tape.read_bytes() == original

    # Restore never reloads this file. Both the source and its byte cursor
    # must be in the entry, not in a C++ reader or an external path.
    source.unlink()
    resumed = command("restore", tape, "--effects")
    assert resumed.stdout == b"42\nFINISHED\n", resumed.stdout
    assert resumed.stderr == b"", resumed.stderr
    assert time.time() * 1000 >= deadline - 25, "timer fired before deadline"
    # The same image is now an expired fork. A mistaken relative-delay
    # restore would wait four seconds and exceed this deadline.
    expired = command("restore", tape, "--effects", timeout=2)
    assert expired.stdout == resumed.stdout
    assert tape.read_bytes() == original, "restores must not rewrite input"

    # A compressed copy restores the same suspended guest state. Construct
    # it with Python's zlib, independently of the C++ tape encoder.
    compressed = directory / "compressed.tape"
    packed = b"NXWISPZ\n" + struct.pack("<I", len(original)) + zlib.compress(original)
    compressed.write_bytes(packed)
    assert command("inspect", compressed).stdout == info.stdout
    disabled = command("restore", compressed, ok=False)
    assert disabled.stdout == b"" and b"effects disabled" in disabled.stderr
    restored = command("restore", compressed, "--effects", timeout=2)
    assert restored.stdout == resumed.stdout and restored.stderr == b""
    assert compressed.read_bytes() == packed, "restores must not rewrite input"

    # Chained checkpoints must consume the restored timer and stop at a
    # newly issued one, even when both effects are inside the same form.
    source.write_text('''
      (print (let ((x 31)) (sleep-ms 1) (write "between\\n")
                          (sleep-ms 1) (+ x 11)))
    ''')
    command("run", source, "--checkpoint", tape)
    first = command("inspect", tape).stdout
    next_tape = directory / "next.tape"
    progress = command("restore", tape, "--effects", "--checkpoint", next_tape)
    assert progress.stdout == b"between\n", progress.stdout
    second = command("inspect", next_tape).stdout
    assert re.search(rb"request-id: (.+)", first)[1] != re.search(rb"request-id: (.+)", second)[1]
    assert command("restore", next_tape, "--effects").stdout == b"42\n"
    repl = command("repl", input=b"(do (sleep-ms 1) (+ 19 4))\n")
    assert repl.stdout == b"23\n", repl.stdout

    source.write_text('''
      (print (try (do (sleep-ms 10000) 'wrong)
                  (error (reason continuation) (vector-get (head reason) 2))))
      (print 19)
    ''')
    command("run", source, "--checkpoint", tape)
    cancelled = command("restore", tape, "--effects", "--cancel", timeout=2)
    assert cancelled.stdout == b":CANCELLED\n19\n", cancelled.stdout

    source.write_text('''
      (write-error "diagnostic\\n")
      (print (read-line))
      (write (read-bytes 3))
      (print (read-line))
      (print (try (sleep-ms (- 0 1)) (error (why k) 'bad-delay)))
      (print (try (send! :host (vector :unknown nil))
                  (error (why k) 'unknown-effect)))
      (print (try (write "prefix" 17) (error (why k) 'bad-write)))
      (sleep-ms 1)
      (gc)
      (print 73)
    ''')
    console = command("run", source, input=b"line\na\0b")
    assert console.stdout == b'"line"\na\0bNIL\nBAD-DELAY\nUNKNOWN-EFFECT\nBAD-WRITE\n73\n', console.stdout
    assert console.stderr == b"diagnostic\n", console.stderr

    source.write_text('''
      (try (write "lost")
           (error (reason continuation)
             (write-error (print-to-string (vector-get (head reason) 2)))))
    ''')
    read, write = os.pipe()
    os.close(read)
    try:
        broken = subprocess.run([binary, "run", str(source)], stdout=write,
                                stderr=subprocess.PIPE, timeout=20)
    finally:
        os.close(write)
    assert broken.returncode == 0 and broken.stderr == b":IO", broken.stderr

    source.write_text('(error "unhandled") (sleep-ms 1)')
    absent = directory / "error.tape"
    unhandled = command("run", source, "--checkpoint", absent, ok=False)
    assert b"unhandled" in unhandled.stderr and not absent.exists()

    source.write_text('(sleep-ms 1) (write "must not run")')
    destination = directory / "existing-directory"
    destination.mkdir()
    failed = command("run", source, "--checkpoint", destination, ok=False)
    assert b"checkpoint rename" in failed.stderr and failed.stdout == b"", failed
    assert destination.is_dir()
    assert not list(directory.glob("existing-directory.*")), "temporary tape leaked"

    source.write_text('(print 7)')
    missing = directory / "missing.tape"
    no_timer = command("run", source, "--checkpoint", missing, ok=False)
    assert b"no checkpoint written" in no_timer.stderr and not missing.exists()
    assert command("inspect", source, ok=False).stdout == b""

print("Wisp host: console, restart, checkpoints, direct timer await, and effect handling passed")
