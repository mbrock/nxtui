# Wisp {#wisp}

Wisp is a small Lisp whose entire execution state lives in its own heap:
definitions and data, and also the evaluator's control stack, pending
continuations, and the request a program is waiting on. A Wisp program can
therefore be stopped at a wait, saved to a file (a *tape*), and restored,
inspected, or forked later in another process. Nothing about the running
computation lives in native stack frames.

This is a C++ port of the [Zig Wisp](https://github.com/mbrock/wisp), which
remains the reference for language semantics. The `wisp` executable hosts the
evaluator on the same nxt deck as everything else, so a Wisp program gets
timers, console I/O, files, subprocesses, and an HTTP server and client from
the nxt runtime, and through capabilities that the command line grants
explicitly.

The design and the reasons for it are in
[RFC 0018](https://github.com/mbrock/nxtui/blob/main/rfc/new/rfc-0018-portable-wisp-lisp-machines.md).
This page explains how to use the system.

[TOC]

## Quick start {#wisp_quick_start}

```sh
meson compile -C build wisp-root-link   # builds build/wisp
build/wisp repl
build/wisp run demo/wisp-timer.wisp
```

The REPL reads one complete form per line and keeps definitions between
lines. `build/wisp --help` prints the full command line:

```text
wisp run SOURCE   [--checkpoint TAPE] [--dir NAME=PATH]... [--run NAME[=PATH]]...
wisp restore TAPE [--effects] [--cancel] [--checkpoint TAPE] [--dir ...] [--run ...]
wisp inspect TAPE
wisp repl         [--dir NAME=PATH]... [--run NAME[=PATH]]...
```

To leave the tool out of a build, configure with `-Dwisp_tool=false`.

## The language in brief {#wisp_language}

Wisp looks like a small Common Lisp: symbols print in upper case, `nil` is
false and the empty list, `t` is true, and functions are named with `#'`
and invoked with `call`. Numbers are signed 31-bit fixnums; strings are
binary-safe byte strings.

```lisp
(defun fact (n)
  (if (< n 1) 1 (* n (fact (- n 1)))))
(fact 10)                         ; => 3628800

(defmacro twice (form) `(do ,form ,form))
(twice (write "hi\n"))            ; prints hi twice

(let ((xs (list 1 2 3)))
  (map (fn (x) (* x x)) xs))      ; => (1 4 9)
```

`defun`, `defmacro`, `fn`, `let`/`let*`, `if`/`cond`/`when`/`unless`, `do`,
`set!`, backquote, and the usual list and string functions come from the
base library, [`src/wisp/base.wisp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/base.wisp).
Redefining a function takes effect immediately for every caller.

### Structs and records {#wisp_structs}

`(defstruct point x (y 0))` defines:

- `make-point`, positional. A slot written `(name default)` and every slot
  after it are optional, and passing `nil` also selects the default.
- the predicate `point?`, accessors `point-x` and `point-y`, and setters
  `set-point-x!` and `set-point-y!`.
- the descriptor `<point>`.

```lisp
(make-point 1)                    ; => #S(POINT :X 1 :Y 0)
(type-of (make-point 1 2))        ; => POINT
```

Redefining a struct with the same slots keeps its descriptor, so instances
made before a reload still belong to it.

Underneath is a `record` heap type borrowed from Emacs Lisp: a word vector
whose first word is its type. `(record 'pair 1 2)` prints as `#S(PAIR 1 2)`.
A struct descriptor is itself a record, `#S(STRUCT-TYPE POINT (X Y))`, which
supplies the type name and slot names. The primitives are `record?`,
`record-type`, `record-length`, `record-get` and `record-set!` (slot indices
exclude the type). `equal?` compares records by identity, as it does
vectors.

### Conditions {#wisp_conditions}

Errors are records named by their type, so a handler dispatches on
`(type-of e)`:

| Signal | Condition |
| --- | --- |
| evaluator type error | `#S(TYPE-MISMATCH CONS 1)` |
| `(error 'name details...)` | `#S(NAME details...)` |
| `(error some-struct)` | that instance |
| `(error "text")` | `#S(ERROR "text")` |

`try` catches them. The handler receives the condition and the continuation
of the point that signalled it:

```lisp
(try (error 'oops 1 2)
  (catch (e k) e))                ; => #S(OOPS 1 2)

(try (point-x 7)
  (catch (e k) (type-of e)))      ; => TYPE-MISMATCH
```

### Prompts, effects, and continuations {#wisp_effects}

Wisp's control primitive is the delimited continuation. `call-with-prompt`
installs a tagged prompt; `send!` captures the continuation up to the nearest
prompt with that tag and passes it, with a value, to the prompt's handler.
`handle` is the macro form:

```lisp
(handle (+ 1 (send! 'greet 5))
  (greet (value k) (list 'got value)))      ; => (GOT 5)
```

`call-with-effect-handler` adds the usual effect-handler shape: the handler
gets the request plus `resume` and `raise` functions. Resuming reinstalls the
handler, so the body can send again.

```lisp
(call-with-effect-handler 'ask
  (fn () (+ 1 (send! 'ask 20)))
  (fn (request resume raise) (call resume (* 2 request))))   ; => 41
```

Continuations are ordinary heap values. They can be stored, called more than
once, and saved in a tape. `try` is itself a prompt with the tag `error`,
and everything the host does for a program (below) is an effect with the tag
`:host`.

### Dynamic variables and packages {#wisp_dynamic}

`defparameter` declares a dynamically scoped variable and `binding` rebinds
it for the extent of a body, including across awaits:

```lisp
(defparameter *depth* 0)
(binding ((*depth* 1)) (child))
```

Packages work as in the Zig implementation: `defpackage`, `in-package`, and
direct-only (non-transitive) use lists. See RFC 0018 for the details.

### Lowered execution {#wisp_lowered}

Source interpretation remains the default. The boot image also includes
the guest compiler: `analyze`, `ir-check`, and `ir-show` describe semantic
code; `lower` checks that graph and produces compact executable records.
Lowering is opt-in:

```lisp
(lowered-eval '(let ((x 41)) (+ x 1)))     ; => 42
(code-show (lower (analyze '(let ((x 41)) (+ x 1)))))
;; => (:LET (X) ((:CONSTANT 41))
;;      (:CALL + ((:LEXICAL-LOAD 0 0) (:CONSTANT 1))))

(defun add1 (x) (+ x 1))
(lower-function! #'add1)                  ; => T
(add1 41)                               ; => 42
(code #'add1)                            ; => (+ X 1)
```

`lower-function!` preserves closure identity and its captured environment;
`lower-package!` lowers the ordinary functions named in a package and returns
their count. `lower` accepts the same optional scope as `ir-check`. Semantic
records themselves are analysis data, not code: evaluating one signals
`invalid-expression`, and source interpretation is the semantic reference
that lowered execution is tested against.

Lowered code keeps no analysis bindings or owner links. Its operation numbers
and operand kinds come from the native schema exposed by `code-operations`.
It still runs on Wisp's heap control machine: source and lowered functions can
call each other, continuations remain multi-shot, and tapes retain suspended
work. Ordinary vectors remain self-evaluating data, not instructions.

For a pending lowered continuation frame, `(code-frame k)` returns
`(operation-view position callee completed-values)`, or `nil` for a frame
from another execution mode. Positions are zero-based; call frames retain the
callee resolved before the arguments started. `ktx-fun`, `ktx-arg`, and
`ktx-acc` expose the raw node, cursor, and progress. Resumptions copy progress
but share lexical locations, just as source execution does.

Macros expand when code is lowered, and syntax is
snapshotted; changing source conses returned by `code` does not change lowered
instructions. Function cells and global values remain live. `set-code!`
with a source form restores source execution; existing suspended work retains
its original nodes. Code records are immutable: `make-code` is their only
constructor and checks each operation's shape once, `record` refuses a code
opcode, `record-set!` refuses a code record, and tapes check restored code.
See
[RFC 0021](https://github.com/mbrock/nxtui/blob/main/rfc/new/rfc-0021-wisp-lowered-code.md)
for the representation and the deferred flat-code/activation-frame decisions.

## Loading files {#wisp_load}

```sh
build/wisp run entry.wisp --dir src=./project
```

With that grant, `entry.wisp` can call `(load "src/main.wisp")`. A loaded file
may use `(load "./lib/helper.wisp")` or `(load "../shared.wisp")`. Relative
paths follow the innermost active load, not the host's working directory, and
the entry file itself grants nothing: every path goes through a
[directory grant](#wisp_files), cannot escape its grant root, and never
follows symlinks.

`load` reads and evaluates one form at a time, so macros and package changes
affect later forms. A successful load caches the normalized path: loading it
again returns `nil`, and a cycle raises `LOAD-CYCLE`. A failed load keeps the
effects of the forms that already ran but is not cached. Package selection is
shared, not restored after loading.

Reader errors carry `path:line:column` (one-based byte columns); evaluation
errors name the enclosing top-level form. Timers inside loaded code can be
checkpointed: the source, cursor, cache, and continuations are all heap data.
The [loading section of RFC 0018](https://github.com/mbrock/nxtui/blob/main/rfc/new/rfc-0018-portable-wisp-lisp-machines.md#local-source-loading-uses-existing-read-grants)
covers retries, overlapping loads, and restore authority.

## Talking to the host: `await` {#wisp_await}

The portable language has no I/O authority. Everything a program does outside
its heap is a request to the host:

```lisp
(await (vector :timer 5))          ; wait five milliseconds
```

`await` sends a `:host` effect whose value is a descriptor
`[operation argument]`. The host stores the captured `resume` and `raise`
continuations in the heap, runs the matching native nxt task, and then
resumes the program with the result or raises a condition in it. The guest
library wraps each operation in a friendlier function:

| Wrapper | Operation | Needs |
| --- | --- | --- |
| `sleep-ms` | `:timer` | |
| `write`, `print`, `write-error` | `:stdout`, `:stderr` | |
| `read-line`, `read-bytes` | `:read-line`, `:read-bytes` | |
| `read-file`, `file-status`, `list-directory` | `:read-file`, ... | `--dir` |
| `run-command` | `:run-command` | `--run` |
| `fetch-http` | `:http-fetch` | |
| `serve-http` | `:http-serve` | |

A failed request raises a `host-error` struct with `operation`, `code`, and
`message` slots. `(host-error-code e)` gives a keyword such as
`:INVALID-ARGUMENT`, `:UNSUPPORTED-OPERATION`, `:NOT-CAPABLE`, `:NOT-FOUND`,
`:IO`, `:TIMEOUT`, or `:CANCELLED`:

```lisp
(try (read-file "nope/x")
  (catch (e k) (host-error-code e)))   ; => :NOT-CAPABLE
```

Guest evaluation runs uninterrupted until it returns or explicitly awaits,
including across garbage collection. CPU-bound code therefore blocks the event
loop, as it would in Node or a browser. Console operations are serialized per
stream. There is no guest spawn/join API and no guest worker model; structured
concurrency inside Wisp is deliberately deferred. The one source of
concurrency is the HTTP server, which runs each request as its own activation.

## Checkpoints and tapes {#wisp_tapes}

A tape is a complete saved machine: heap, definitions, the source being run,
and the pending request.

```sh
build/wisp run demo/wisp-timer.wisp --checkpoint build/timer.tape
build/wisp inspect build/timer.tape
build/wisp restore build/timer.tape --effects
```

[`demo/wisp-timer.wisp`](https://github.com/mbrock/nxtui/blob/main/demo/wisp-timer.wisp)
prints a line, binds `answer` to 35, sleeps five seconds, and prints
`(+ answer 7)`. With `--checkpoint`, the run saves when the program issues the
timer and exits. `inspect` shows where it stopped, without executing guest
code:

```text
source-location: demo/wisp-timer.wisp:3:1
request: #<:TIMER 5000>
deadline-unix-ms: "1791059626332"
```

`restore --effects` continues from the saved wait in a new process and prints
`42`: the binding of `answer` was in the heap.

The rules:

- **Checkpoints happen at timers.** `--checkpoint` saves synchronously at the
  next newly issued timer, then exits. Console awaits before that point have
  already finished; no live native wait is ever saved. Restoring a timer does
  not itself trigger another checkpoint; only a newly issued timer does, so
  `restore --effects --checkpoint OUT` saves at the *next* timer.
- **Deadlines are absolute.** Restore uses the saved deadline; an elapsed
  timer fires immediately.
- **Effects are off by default.** `restore` without `--effects` refuses to
  perform the pending request. `--cancel` instead delivers a `:CANCELLED`
  host error to the program's handler for that request.
- **Restoring is forking.** The input tape is never updated, and restoring
  it twice with effects can repeat effects. There is no exactly-once
  guarantee and no log of effect results.
- **Live resources are not saved.** HTTP listeners, outbound requests, and
  commands are rejected in checkpoint mode with `:NOT-REPLAYABLE`.
  Directory and program grants are not part of a tape either; pass them again
  on restore.
- **The source is inside the tape**, so restore needs no source file and does
  not repeat earlier forms.
- **Only load trusted tapes.** Validation checks structure and a SHA-256
  digest; it is not a security sandbox.

Checkpoints replace the target file atomically, with file and directory
`fsync`. The executable's image schema is `NXT-WISP-4`:
`[tag [source path form-start] byte-offset run pending last-result request-serial]`.
The portable tape format is version 5, which adds a versioned lowered-operation
manifest. Older host schemas, older portable tapes, and incompatible operation
identities or operand layouts are rejected; there is no automatic migration.

### The tape API {#wisp_tape_api}

[`wisp::tape`](https://github.com/mbrock/nxtui/blob/main/src/wisp/tape.hpp)
has `encode`/`decode` for byte buffers and `write`/`read` for streams.
`decode` returns a separate, address-stable image; it never replaces a running
machine. Pass `wisp::tape::compression::zlib` as the last argument to
`encode` or `write` to compress; `decode` and `read` detect compression. The
size limit applies to both the compressed and the expanded bytes, and the
expanded size is checked before allocation. CLI checkpoints are uncompressed.

The base, compiler, and host libraries are evaluated once at build time into a
zlib-compressed boot tape that is embedded in the executable with `#embed`.
Fresh runs and REPLs decode a private copy of it; restores use only the
selected checkpoint. Changes to the libraries, the evaluator, or the codec
regenerate the boot tape. `-Dwisp_boot_compression=false` embeds it
uncompressed.

## Capabilities {#wisp_capabilities}

A Wisp program starts with no authority beyond the console. Like WASI
preopens, the command line grants what it may touch.

### Directories {#wisp_files}

`--dir NAME=PATH` makes `PATH` visible to the program as `NAME/...`,
read-only, for `run`, `restore`, and `repl`. `--dir PATH` works when `PATH`
is a plain name. Grants are opened at startup.

```lisp
(read-file "site/notes.txt")     ; binary-safe string, up to 64 MiB
(file-status "site/index.html")  ; #S(FILE-STATUS :KIND :FILE :SIZE "1234" ...)
(list-directory "site")          ; sorted names
(serve-file "site/app.wasm")     ; in an HTTP handler: stream the file
```

Paths are `/`-separated plain names beneath a grant. Empty, `.`, and `..`
segments are rejected, and symlinks are never followed, even ones that stay
inside the tree (stricter than WASI). FIFOs and devices are not read. Failures
raise `:NOT-CAPABLE` (not granted, or a symlink), `:NOT-FOUND`,
`:PERMISSION-DENIED`, `:INVALID-ARGUMENT`, or `:IO`. `file-status` reports
size and modification time (Unix milliseconds) as decimal strings, because
fixnums have only 31 bits.

File operations never stall other activations. On %io_uring, opens and stats
are kernel operations, and `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)`
enforces the confinement in the kernel. Elsewhere (epoll, kqueue), and for
listings and reads that would block the wand, they run on a small
`blocking_pool` started on first use
([`nxtrt::fs::files`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/fs.hpp)).
On macOS, listings read names and attributes in `getattrlistbulk` batches.

### Programs {#wisp_programs}

`--run NAME` finds `NAME` on `PATH` at startup; `--run NAME=PATH` names a
specific executable. The program can then run only those, by name, and never
searches `PATH` itself:

```lisp
(run-command "echo" "hi there")
;; => #S(PROCESS-RESULT :EXIT-CODE 0 :SIGNAL NIL :OUTPUT "hi there\n")
```

Standard input is empty, and stdout and stderr arrive merged. A nonzero exit
or a signal is a result, not an error. Output beyond 8 MiB terminates the
program and raises `:TOO-LARGE`. Cancelling the awaiting activation (for
example, an HTTP handler timeout) terminates and reaps the program; it signals
only that program, not processes it started.

A program grant is coarse: the program runs with the host's full authority,
whatever the directory grants say.

## Serving HTTP {#wisp_http_server}

```sh
build/wisp run demo/wisp-http.wisp --dir demo
curl localhost:8080/hello/you
```

[`demo/wisp-http.wisp`](https://github.com/mbrock/nxtui/blob/main/demo/wisp-http.wisp)
binds loopback port 8080. Put a TLS-terminating reverse proxy in front of it
for anything public: TLS, access logging, and exposure policy belong to the
proxy, and forwarded headers are not trusted.

### Routes {#wisp_routes}

`defroute` and `route-request` are ported from Zig Wisp:

```lisp
(defroute ("GET" "hello" name)                  ; NAME binds one segment
  (add-header! "Content-Type" "text/plain; charset=utf-8")
  (set-response-body! (string-append "Hello, " name ".\n")))

(defroute ("GET" "static" &rest path) ...)      ; PATH binds the rest
(defroute ("GET" "skip" _ last) ...)            ; _ matches without binding
(defroute (method "any") ...)                   ; a symbol method binds it

(serve-http 8080 #'route-request)
```

A pattern is a method followed by decoded path segments (`/` is `("")`).
Routes are tried in definition order, and redefining a pattern replaces its
handler in place. HEAD falls back to GET routes. An unmatched path answers
404; a path served only under other methods answers 405 with `Allow`.

### Handlers {#wisp_handlers}

Each request calls a zero-argument handler with `*request*` and `*response*`
dynamically bound to an `http-request` and an `http-response` struct.

- **Reading:** `request-method`, `request-path`, `request-query-string`,
  `request-header` (case-insensitive, first value), and `request-text`. The
  path and query string are raw; `(http-request-segments *request*)` is the
  path split at `/` and percent-decoded. A malformed escape is answered with
  400 before any handler runs.
- **Writing:** `set-response-status!`, `add-header!`, and
  `set-response-body!`. The handler's return value is ignored; the mutated
  `*response*` is sent.
- **Early exit:** `(send! :respond (response 404 nil "Not Found"))` sends
  that response immediately.
- **Files:** a body may be `(make-file-body path)` instead of a string. The
  server opens it after the handler finishes and streams it with
  `Content-Length`, outside the heap and past the 8 MiB string-body limit.
  `serve-file` checks the path first and answers 404 for anything but a
  regular file. Each streamed chunk gets a fresh write deadline, so a long
  download only needs to keep making progress.

Waiting inside a handler, on a timer or an outbound request, suspends only
that request's activation. Other requests keep being accepted and handled; the
demo's `/slow` and `/relay` routes show this. Dynamic bindings keep
overlapping requests isolated, while packages and globals are shared.

### Server limits {#wisp_server_limits}

The server is the reusable C++
[`nxtrt::http::serve`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/http-server.hpp);
Wisp supplies only the handler.

| Limit | Default |
| --- | --- |
| request head | 16 KiB |
| request body | 1 MiB |
| response body (string) | 8 MiB |
| concurrent connections | 64 |
| header deadline | 10 s |
| body, handler, and write deadlines | 30 s each |

It supports HTTP/1.1 keep-alive, pipelining, fixed-length and chunked request
bodies, and binary bodies. Upgrades, `CONNECT`, and `Expect` are rejected.
A timed-out handler is cancelled, and its pending native waits are drained
before the socket closes. Request-body streaming, response streaming other
than files, WebSockets, and effect logging are future work.

## Making HTTP requests {#wisp_http_client}

```lisp
(fetch-http url &optional method headers body)
```

`fetch-http` uses nxt's native client stack. The method defaults to `"GET"`,
headers to `nil`, and the body to empty. It returns an `http-response`, the
same struct handlers fill in, so a handler can relay it with
`(send! :respond ...)`. Headers are a list of `[name value]` vectors that
preserve order and duplicates. The body is a binary-safe string, already
de-chunked and decompressed. HTTP error statuses such as 404 are ordinary
responses, not conditions.

```lisp
(let ((r (fetch-http "http://127.0.0.1:8080/hello/client")))
  (print (http-response-status r))           ; 200
  (write (http-response-body r)))            ; Hello, client.

(fetch-http "http://127.0.0.1:8080/echo" "POST"
  (list (vector "Content-Type" "text/plain")) "hello from the client")
```

The demo's `/relay` route calls `fetch-http` *inside a handler*, POSTing to
`/echo` on the same listener; the nested request is accepted and served while
the outer one waits. Don't forward client-chosen URLs without an access
policy.

The client is deliberately simple. It buffers the whole response, opens one
connection per call, and does not follow redirects, retry, pool connections,
stream, or keep cookies. URLs may use DNS names or IPv4 addresses, numeric
ports, and escaped paths (include `/` before a query string); userinfo,
fragments, and IPv6 literals are not supported. `Host`, `Content-Length`,
`Connection`, transfer framing, upgrades, and expectations are managed by the
host, and attempts to set them or to inject control characters are rejected.
It advertises gzip and deflate and also decodes zstd and brotli when built
with them. Response headers are the original wire headers, so their
`Content-Length` and `Content-Encoding` may describe the compressed body.

| Limit | Value | On failure |
| --- | --- | --- |
| request body | 1 MiB | `:INVALID-ARGUMENT` |
| request head | 16 KiB | `:INVALID-ARGUMENT` |
| each response head | 16 KiB | `:IO` |
| decoded response body | 8 MiB | `:IO` |
| whole request | 30 s | `:TIMEOUT` |

DNS uses c-ares when available; the libc fallback resolves synchronously.

### HTTPS {#wisp_tls}

HTTPS authenticates the server. nxt's TLS 1.3 client uses libcrypto's X.509
verifier (AWS-LC under Nix) to check chain trust, signatures, validity dates,
CA constraints, critical extensions, and TLS server usage. The URL host must
match a subject alternative name: DNS names allow only whole-label wildcards,
IP literals require an IP SAN, and common-name fallback is disabled. DNS
connections send SNI; IP connections do not. CertificateVerify and Finished
are both verified before any HTTP request is sent.

Trust comes from libcrypto's default CA paths, configurable with
`SSL_CERT_FILE` (a PEM bundle) or `SSL_CERT_DIR` (a hashed CA directory).
Native C++ clients can call `handshake(host, ca_file)` to trust only one PEM
bundle. For a private server, create a local CA and sign the server
certificate with it: AWS-LC does not treat a self-signed leaf in a CA bundle
as a trust anchor. There is no option to disable verification.

The TLS stack is handmade and experimental. It does not check revocation
(OCSP, CRL) or Certificate Transparency, and it is not audited.

## How Wisp sits on nxt {#wisp_architecture}

Wisp is not a second runtime. The `wisp` executable is a host program: it
owns an nxt `runtime`, an evaluator, and a moving heap, and translates `:host`
effects into ordinary nxt tasks.

```diagram
┌───────────────────────────────────────────┐
│ Wisp program: serve-http / fetch-http     │
│ heap, evaluator, continuations            │
└─────────────────────┬─────────────────────┘
                      │ :host effect
┌─────────────────────▼─────────────────────┐
│ Host: heap continuations ↔ native tasks   │
└─────────────────────┬─────────────────────┘
                      │ co_await
┌─────────────────────▼─────────────────────┐
│ nxtrt: deck, pools, tasks, I/O wishes     │
│ HTTP server, DNS/TCP/TLS client, streams  │
└─────────────────────┬─────────────────────┘
                      │
┌─────────────────────▼─────────────────────┐
│ OS: io_uring / epoll / kqueue             │
└───────────────────────────────────────────┘
```

The division of labour is strict. **Guest control state stays in the heap**,
which may move during collection; **native coroutine frames own only
temporary waits, sockets, and buffers.** That is what makes a checkpoint
possible: at any await, everything the program needs to continue is heap data.

An outbound request goes like this. `fetch-http` calls `await` with an
`:HTTP-FETCH` descriptor. The `:host` effect captures the continuation; the
host stores the request and its `resume` and `raise` functions in the heap,
copies the arguments into native storage, and awaits DNS, connect, optional
TLS, and body decoding as an nxt task. On completion it roots a heap response
and resumes the guest; on failure it raises a `host-error`.

An incoming request goes the other way. The native server parses and bounds
it, the host copies it into the heap as an `http-request` struct, and
`%nxt-http-handle` binds `*request*` and `*response*`, installs the
`:respond` prompt, and runs the handler as a rooted heap activation until it
returns or awaits. A handler deadline cancels the activation's actual pending
native wait and drains it before the socket is torn down.

Source entry points:

- [`src/wisp/host.wisp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/host.wisp):
  the Lisp side of the host, including `await`, the HTTP and file wrappers,
  and the host structs. The C++ host finds struct slots by name through their
  descriptors, so field order is declared only here.
- [`src/wisp/main.cpp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/main.cpp):
  host effects, rooting, request conversion, native awaits, and delivery of
  results and errors.
- [`src/wisp/nxt.hpp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/nxt.hpp):
  `wisp::drive`, an optional adapter that advances an evaluator in bounded
  turns on a deck, for embedding Wisp in other nxt programs.
- [`src/nxtrt/http-server.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/http-server.hpp),
  [`http.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/http.hpp),
  [`net_dns.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/net_dns.hpp),
  and [`tls.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/tls.hpp):
  the native server and client, written over nxt byte streams with no Asio or
  Beast dependency. Native programs can use them without Wisp.

Coverage instructions for the Wisp test suites are in @ref building.
