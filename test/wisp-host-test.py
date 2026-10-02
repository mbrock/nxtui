#!/usr/bin/env python3
"""Process boundaries, real pipes/timers, and host checkpoint policy."""
import pathlib
import os
import re
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
    source = directory / "job.wisp"
    tape = directory / "job.tape"

    repl = command("repl", input=b"(+ 17 25)\nunbound\n(+ 19 4)\nnil\n")
    assert repl.stdout == b"42\n23\nNIL\n", repl.stdout
    assert b"UNBOUND-VARIABLE" in repl.stderr, repl.stderr

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

    source.write_text(f'''
      (let ((a (spawn (fn () (write "{'A' * 8192}"))))
            (b (spawn (fn () (write "{'B' * 8192}")))))
        (join a) (join b))
    ''')
    writes = command("run", source).stdout
    assert writes in (b"A" * 8192 + b"B" * 8192, b"B" * 8192 + b"A" * 8192)
    source.write_text('''
      (let ((a (spawn (fn () (read-line))))
            (b (spawn (fn () (read-line)))))
        (print (join a)) (print (join b)))
    ''')
    lines = command("run", source, input=b"A" * 3000 + b"\n" + b"B" * 6000 + b"\n")
    assert sorted(lines.stdout.splitlines()) == [b'"' + b"A" * 3000 + b'"', b'"' + b"B" * 6000 + b'"']

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

    # A sequential host deadlocks here. The second job must run while the
    # first waits; collection must preserve its lexical state and result.
    source.write_text('''
      (defvar ready nil)
      (defun await-ready () (if ready 37 (do (sleep-ms 1) (await-ready))))
      (let* ((a (spawn (fn () (vector (await-ready) "alpha"))))
             (b (spawn (fn () (gc) (set! ready t) 19))))
        (print (join a)) (print (join b)) (print (join a)))
    ''')
    assert command("run", source).stdout == b'#<37 "alpha">\n19\n#<37 "alpha">\n'

    source.write_text('''
      (let ((job nil))
        (set! job (spawn (fn () (sleep-ms 1)
          (try (join job) (error (why k) (vector-get (head why) 2))))))
        (print (join job)))
      (let ((a nil) (b nil))
        (set! a (spawn (fn () (sleep-ms 1) (join b))))
        (set! b (spawn (fn () (sleep-ms 1)
          (try (join a) (error (why k) (vector-get (head why) 2))))))
        (print (try (join a) (error (why k) (vector-get (head why) 2))))
        (print (try (join b) (error (why k) (vector-get (head why) 2)))))
      (let ((bad (spawn (fn () (error "child failed")))))
        (sleep-ms 5)
        (print (try (join bad) (error (why k) (vector-get (head why) 2)))))
    ''')
    cycles = command("run", source).stdout.splitlines()
    assert cycles[0] == b":JOIN-CYCLE" and cycles[-1] == b":JOB-FAILED", cycles
    assert all(x in (b":JOIN-CYCLE", b":JOB-FAILED") for x in cycles[1:3]), cycles
    source.write_text('(spawn (fn () (error "unobserved failure")))')
    assert b"unjoined Wisp job failed" in command("run", source, ok=False).stderr

    # More jobs than one firm's deed capacity. Slots and per-job scopes
    # must recycle; completed handles must not pin the whole job history.
    source.write_text('''
      (defun repeat-jobs (n)
        (if (eq? n 0) 'finished
          (do (join (spawn (fn () (+ n 17)))) (repeat-jobs (- n 1)))))
      (print (repeat-jobs 4100))
    ''')
    assert command("run", source, timeout=90).stdout == b"FINISHED\n"

    source.write_text('''
      (let* ((a (spawn (fn () (sleep-ms 30) (gc) 37)))
             (b (spawn (fn () (sleep-ms 40) 19))))
        (write "before-join\\n")
        (print (+ (join a) (join b))))
    ''')
    stopped = command("run", source, "--checkpoint", tape)
    assert stopped.stdout == b"before-join\n", stopped.stdout
    inspection = command("inspect", tape).stdout
    assert inspection.count(b"request: #<:TIMER ") == 2, inspection
    source.unlink()
    assert command("restore", tape, "--effects").stdout == b"56\n"
    assert b"effects disabled" in command("restore", tape, ok=False).stderr

    # A pending request is not sufficient for a checkpoint: the native
    # stdin read is still in flight. Refuse, leave the destination intact,
    # and drain that read when the session aborts.
    source.write_text('(spawn (fn () (read-line))) (sleep-ms 1)')
    before = tape.read_bytes()
    blocked = subprocess.Popen([binary, "run", str(source), "--checkpoint", str(tape)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    try:
        blocked.wait(timeout=15)
        stdout, stderr = blocked.communicate(timeout=2)
    finally:
        if blocked.poll() is None:
            blocked.kill()
            blocked.communicate()
    assert blocked.returncode != 0 and b"cannot quiesce" in stderr, stderr
    assert stdout == b"" and tape.read_bytes() == before

    # Admission is bounded rather than blocking all occupied workers.
    source.write_text('''
      (defun fill (n)
        (if (eq? n 0) nil
          (cons (spawn (fn () (sleep-ms 5) n)) (fill (- n 1)))))
      (let ((jobs (fill 63)))
        (print (try (spawn (fn () 7))
                    (error (why k) (vector-get (head why) 2))))
        (for-each jobs (fn (job) (join job))))
    ''')
    # Freeze the timers so the capacity test is independent of CPU speed.
    assert command("run", source, "--checkpoint", tape).stdout == b":CAPACITY\n"
    assert command("restore", tape, "--effects").returncode == 0

print("Wisp host: console, restart, checkpoints, concurrent jobs, GC, joins, cycles, and admission passed")
