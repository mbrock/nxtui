#!/usr/bin/env python3
"""Local LOAD contracts, source diagnostics, and checkpoints during loading."""
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())


def command(*args, ok=True):
    result = subprocess.run([binary, *map(str, args)], capture_output=True,
                            timeout=20)
    assert (result.returncode == 0) == ok, (args, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="wisp-module-") as directory:
    directory = pathlib.Path(directory)
    modules = directory / "modules"
    (modules / "sub").mkdir(parents=True)
    source = directory / "entry.wisp"
    grants = ["--dir", f"src={modules}"]

    def run(text, *args, ok=True, grant=True):
        source.write_text(text)
        return command("run", source, *(grants if grant else []), *args, ok=ok)

    # Read/eval must interleave: the package and macro do not exist until
    # their defining forms finish. NIL is a form, not EOF. Nested paths
    # are relative to the currently loading file, not the CLI entry or cwd.
    (modules / "sub" / "math.wisp").write_text('''
      nil
      (defpackage math (:use wisp))
      (in-package math)
      (defmacro twice (x) `(+ ,x ,x))
      (defvar answer (twice 21))
      (gc)
    ''')
    (modules / "empty.wisp").write_text("; empty module")
    (modules / "main.wisp").write_text('''
      (load "./sub/math.wisp")
      (print (eq? 'fresh 'math:fresh))
      (in-package wisp)
      (print (load "./sub/../sub/./math.wisp"))
      (print math:answer)
      (load "./empty.wisp")
    ''')
    out = run('(print (load "src/main.wisp"))\n'
              '(print (load "src/./main.wisp"))').stdout
    assert out == b'T\nNIL\n42\n"src/main.wisp"\nNIL\n', out

    # Every read goes through an explicit :host effect, which can be
    # handled by the caller. EVAL retains dynamics but not caller locals.
    out = run('''
      (defparameter *marker* 0)
      (print
        (binding ((*marker* 37))
          (call-with-effect-handler :host
            (fn () (load "virtual/test.wisp"))
            (fn (request resume raise)
              (if (eq? (vector-get request 0) :read-file)
                  (call resume "(defvar observed *marker*)")
                (error 'unexpected-request))))))
      (print (list observed *marker*))
    ''', grant=False).stdout
    assert out == b'"virtual/test.wisp"\n(37 0)\n', out

    # Independent suspended loads are not cycles or a shared promise. Both
    # execute; later calls see the successful cache. This uses guest effects
    # to hold two activations deterministically, without a timing-based server.
    out = run('''
      (defvar pending nil) (defvar executions 0)
      (defun start-load ()
        (call-with-effect-handler :host
          (fn () (load "virtual/overlap.wisp"))
          (fn (request resume raise) (set! pending (cons resume pending)) nil)))
      (start-load) (start-load)
      (for-each pending
        (fn (resume) (call resume "(set! executions (+ executions 1))")))
      (print (list executions (load "virtual/overlap.wisp") *loading-files*))
    ''', grant=False).stdout
    assert out == b'(2 NIL NIL)\n', out

    # Failure is not a transaction: prior effects remain, dependencies
    # stay cached, and a retry reruns the failed file, not its dependencies.
    (modules / "dependency.wisp").write_text('(set! dependencies (+ dependencies 1))')
    (modules / "retry.wisp").write_text('''
      (load "./dependency.wisp")
      (set! attempts (+ attempts 1))
      (when (eq? attempts 1) (error 'first-failure))
      (set! finished t)
    ''')
    out = run('''
      (defvar attempts 0) (defvar dependencies 0) (defvar finished nil)
      (print (try (load "src/retry.wisp") (catch (e k) (type-of e))))
      (print (list attempts dependencies finished *loaded-files* *loading-files*))
      (load "src/retry.wisp")
      (print (list attempts dependencies finished *loading-files*))
    ''').stdout
    assert out == (b'LOAD-ERROR\n(1 1 NIL ("src/dependency.wisp") NIL)\n'
                   b'(2 1 T NIL)\n'), out

    # Both self and indirect cycles use normalized identities; errors and
    # nonlocal escapes leave no stale in-progress record or success cache.
    (modules / "self.wisp").write_text('(load "./sub/../self.wisp")')
    (modules / "a.wisp").write_text('(load "./sub/b.wisp")')
    (modules / "sub" / "b.wisp").write_text('(load "../a.wisp")')
    for filename, chain in (("self", b'("src/self.wisp" "src/self.wisp")'),
                            ("a", b'("src/a.wisp" "src/sub/b.wisp" "src/a.wisp")')):
        result = run(f'(load "src/{filename}.wisp")', ok=False)
        assert b"LOAD-CYCLE" in result.stderr and chain in result.stderr, result.stderr
    (modules / "escape.wisp").write_text('(send! :escape 19) (error \'unreachable)')
    out = run('''
      (print (call-with-prompt :escape
               (fn () (load "src/escape.wisp")) (fn (v k) v)))
      (print (list *loading-files* *loaded-files*))
      (print (try (load "src/self.wisp") (catch (e k) 'caught)))
      (print (list *loading-files* *loaded-files*))
      (print (load "src/empty.wisp"))
    ''').stdout
    assert out == b'19\n(NIL NIL)\nCAUGHT\n(NIL NIL)\n"src/empty.wisp"\n', out

    # The CLI entry's host path grants nothing. No absolute path, URL,
    # ungranted directory, grant-root traversal, symlink, or implicit cwd.
    (modules / "link.wisp").symlink_to("empty.wisp")
    (directory / "outside.wisp").write_text('(error \'escaped)')
    result = run('(load "src/empty.wisp")', grant=False, ok=False)
    assert b"NOT-CAPABLE" in result.stderr, result.stderr
    for path, expected in (
        ("other/empty.wisp", b"NOT-CAPABLE"),
        (str(modules / "empty.wisp"), b"LOAD-PATH-ERROR"),
        ("https://example.org/a.wisp", b"LOAD-PATH-ERROR"),
        ("./modules/empty.wisp", b"relative load needs an active source file"),
        ("src/../outside.wisp", b"cannot cross a grant root"),
        ("src/sub/../../outside.wisp", b"cannot cross a grant root"),
        ("src/link.wisp", b"NOT-CAPABLE"),
        ("src/missing.wisp", b"NOT-FOUND"),
    ):
        result = run(f'(load "{path}")', ok=False)
        assert expected in result.stderr and b"escaped" not in result.stderr, result.stderr

    # Reader positions are absolute, one-based byte columns, even after
    # earlier successful forms and multibyte text. Evaluation context is
    # explicitly the enclosing top-level form, not a fabricated backtrace.
    result = run('nil\n; comment\n"é" )', ok=False)
    assert f"{source}:3:6: unexpected character".encode() in result.stderr, result.stderr
    result = run('nil\n; comment\n  (head 1)', ok=False)
    assert f"{source}:3:3: while evaluating top-level form".encode() in result.stderr, result.stderr
    (modules / "bad.wisp").write_text('nil\n; comment\n  (head 1)\n(error \'later)')
    (modules / "outer.wisp").write_text('nil\n (load "./bad.wisp")')
    result = run('(load "src/outer.wisp")', ok=False)
    for expected in (b"src/outer.wisp:2:2", b"src/bad.wisp:3:3", b"TYPE-MISMATCH"):
        assert expected in result.stderr, result.stderr
    assert b"LATER" not in result.stderr, result.stderr
    (modules / "bad.wisp").write_text('nil\n"é" )')
    result = run('(load "src/bad.wisp")', ok=False)
    assert b"src/bad.wisp:2:6: unexpected character" in result.stderr, result.stderr
    (modules / "bad.wisp").write_bytes(b'nil\n"\xff"')
    result = run('(load "src/bad.wisp")', ok=False)
    assert b"src/bad.wisp:2:2: invalid or truncated UTF-8" in result.stderr, result.stderr

    # A loaded source and its relative-path stack survive a process boundary.
    # Already-read files may disappear; *new* loads still need regranted
    # authority. The cache survives, but does not become an I/O capability.
    tape = directory / "loading.tape"
    (modules / "main.wisp").write_text('''
      (load "./dependency.wisp")
      (load "./sub/paused.wisp")
      (print 'parent-finished)
    ''')
    paused = modules / "sub" / "paused.wisp"
    paused.write_text('''
      (write "before\\n")
      (let ((x 31)) (gc) (sleep-ms 1) (print (+ x 11)))
      (print (load "../dependency.wisp"))
      (load "./late.wisp")
    ''')
    (modules / "sub" / "late.wisp").write_text("(print 'late)")
    stopped = run('(defvar dependencies 0) (load "src/main.wisp")\n'
                  '(print dependencies)', "--checkpoint", tape)
    assert stopped.stdout == b"before\n", stopped.stdout
    source.unlink()
    (modules / "main.wisp").unlink()
    paused.unlink()
    (modules / "dependency.wisp").unlink()
    denied = command("restore", tape, "--effects", ok=False)
    assert denied.stdout == b"42\nNIL\n", denied.stdout
    assert b"src/sub/late.wisp:1:1" in denied.stderr and b"NOT-CAPABLE" in denied.stderr
    resumed = command("restore", tape, "--effects", *grants)
    assert resumed.stdout == b"42\nNIL\nLATE\nPARENT-FINISHED\n1\n", resumed.stdout

    # Error attribution also survives restore without the entry or module.
    paused.write_text('nil\n (do (sleep-ms 1) (head 1))')
    run('(load "src/sub/paused.wisp")', "--checkpoint", tape)
    source.unlink()
    paused.unlink()
    failed = command("restore", tape, "--effects", ok=False)
    assert f"{source}:1:1:".encode() in failed.stderr, failed.stderr
    assert b"src/sub/paused.wisp:2:2" in failed.stderr, failed.stderr

print("Wisp modules: paths, caching, cycles, failures, grants, diagnostics, and loading checkpoints passed")
