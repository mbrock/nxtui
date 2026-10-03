# Wisp: portable Lisp machines and HTTP on NXT {#wisp}

The `wisp` executable runs Wisp's guest evaluator on the same NXT deck, with
console effects, explicit `await`, `sleep-ms` timers, and a loopback HTTP server.
The guest heap holds closures, suspended control state, source position, and
pending requests; native coroutines own temporary waits and sockets.
See [RFC 0018](https://github.com/mbrock/nxtui/blob/main/rfc/new/rfc-0018-portable-wisp-lisp-machines.md)
for the language, GC, and portable tape contracts.

```sh
nix develop -c meson compile -C build wisp-root-link
build/wisp repl
build/wisp run demo/wisp-timer.wisp --checkpoint build/timer.tape
build/wisp inspect build/timer.tape
build/wisp restore build/timer.tape --effects
```

`--checkpoint` synchronously saves at the next newly issued timer, then exits.
Earlier console awaits have already finished; no live native wait is saved.
HTTP listeners are rejected in checkpoint mode. Restore uses
saved absolute deadlines; elapsed timers fire immediately. The source is
inside the tape, so restore needs no source file and does not repeat earlier
forms. Checkpoints replace the selected file atomically, with file and directory
fsync. Only load trusted tapes: validation is not a security sandbox.

Restores have **effects disabled** unless given `--effects`. `inspect` executes
no guest code. Add `--cancel` to restore to deliver a `:CANCELLED` host condition
through the saved pending request's guest error handler instead of performing
it. Restoring again is a fork, not an exactly-once guarantee: enabling both forks can
repeat effects. The input tape is never updated implicitly.

Host conditions are `host-error` structs with `operation`, `code` and
`message` slots, so `(host-error-code e)` gives codes such as
`:INVALID-ARGUMENT`, `:UNSUPPORTED-OPERATION`, `:IO`, and `:CANCELLED`.
`restore --effects --checkpoint OUT` does not save merely because a timer was
restored: only a newly issued timer requests another checkpoint.

`(await task-description)` sends a `:host` effect and returns the selected
native operation's result. Descriptors have the form `[operation argument]`;
for example, `(await (vector :timer 5))` awaits a five-millisecond timer.
Low-level wishes and composite native tasks use this same bridge. `sleep-ms`
and `fetch-http` are guest wrappers around `await`; there is no Wisp spawn/join
API or guest worker/job model. Console operations remain serialized per stream.
Guest evaluation runs uninterrupted until return or explicit await, including
across GC; CPU-bound code can block the event loop, just as in Node/browser JS.
Wisp structured concurrency is deliberately deferred.

The REPL accepts complete forms on one line and preserves definitions. Console
hooks include `write`, `print`, `write-error`, `read-line`, and `read-bytes`.
The executable host uses **image schema `NXT-WISP-4`**:
`[tag [source path form-start] byte-offset run pending last-result request-serial]`. Old host
schemas are intentionally rejected, as are portable tapes before version 4,
which added records. Disable the tool with `-Dwisp_tool=false`.

## Structs, records and conditions

`(defstruct point x (y 0))` defines `make-point` (positional; a slot written
`(name default)` and all later slots are optional, and NIL means the default),
`point?`, accessors `point-x`/`point-y`, setters `set-point-x!`/`set-point-y!`,
and the descriptor `<point>`. Instances print as `#S(POINT :X 1 :Y 0)`, and
`type-of` says `POINT`. Redefining a struct with the same slots keeps its
descriptor, so instances made before a reload still belong to it.

Underneath is a `record` heap type borrowed from Emacs Lisp: a word vector whose
first word is its type. `(record 'pair 1 2)` prints as `#S(PAIR 1 2)`; a struct
descriptor is itself a record `#S(STRUCT-TYPE POINT (X Y))`, which supplies the
type name and slot names. `record?`, `record-type`, `record-length`,
`record-get` and `record-set!` are the primitives (slot indices exclude the
type). `equal?` compares records by identity, as it does vectors.

Conditions are records named by their type, so a handler asks `(type-of e)`:
the evaluator signals `#S(TYPE-MISMATCH CONS 1)`, `(error 'name details...)`
signals `#S(NAME details...)`, `(error some-struct)` signals that instance, and
`(error "text")` signals `#S(ERROR "text")`. Host records are structs from
[`src/wisp/host.wisp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/host.wisp): `http-request`, `http-response`,
`file-status`, `file-body` and `host-error`. The C++ host finds their slots by
name through the descriptors, so field order is declared only there.

## Load local Wisp files with explicit read grants

With `build/wisp run entry.wisp --dir src=./project`, the entry can call
`(load "src/main.wisp")`. Loaded files may use `(load "./lib/helper.wisp")` or
`(load "../shared.wisp")`; relative paths follow the innermost active load,
not the host working directory. The CLI entry grants no filesystem authority.
Paths cannot escape their grant root or follow symlinks.

`load` reads and evaluates one form at a time, so macros and package changes
affect later forms. Success caches the normalized path; repeat loads return
NIL, and cycles raise `LOAD-CYCLE`. Failed loads retain earlier effects but
are not cached. Package selection is shared, not restored after loading.
Reader errors carry `path:line:column` (one-based byte columns); evaluation
errors identify the enclosing top-level form. Timers inside loaded code can
be checkpointed without retaining native loader state. See the
[loading contracts and runnable example](https://github.com/mbrock/nxtui/blob/main/rfc/new/rfc-0018-portable-wisp-lisp-machines.md#local-source-loading-uses-existing-read-grants)
for retries, overlapping loads, and restore authority.

## Serve plain HTTP behind a reverse proxy

```sh
build/wisp run demo/wisp-http.wisp --dir demo
```

The demo binds loopback port 8080 and directly awaits the native serving task.
Each callback calls a zero-argument handler with dynamic `*request*` and
`*response*`, an `http-request` and an `http-response` struct. Use
`request-method`, `request-path`, `request-query-string`, `request-header`
(case-insensitive), and `request-text`; paths and query strings are raw, while
`(http-request-segments *request*)` is the path split at `/` and
percent-decoded. A malformed escape gets 400 before any handler runs. Set response state
with `set-response-status!`, `add-header!`, and `set-response-body!`, or exit early
with `(send! :respond (response 404 nil "Not Found"))`. Ordinary handler return
values are ignored, matching the old Wisp web interface.

The demo dispatches with `defroute` and `route-request`, ported from Zig Wisp:

```lisp
(defroute ("GET" "git" repo "info" "refs") ...) ; REPO binds one segment
(defroute ("GET" "static" &rest path) ...)      ; PATH binds the rest
(defroute ("GET" "skip" _ last) ...)            ; _ matches without binding
(defroute (method "any") ...)                   ; a symbol method binds it
(serve-http 8080 #'route-request)
```

Patterns are a method followed by decoded path segments (`/` is `("")`). Routes
are tried in definition order, and redefining a pattern replaces its handler
in place. HEAD falls back to GET routes; unmatched paths answer 404, and paths
served only under other methods answer 405 with `Allow`.

## Files and capabilities

The guest has no ambient filesystem. Like WASI preopens, the command line
grants directories: `--dir NAME=PATH` (or `--dir PATH` when PATH is a plain
name) makes PATH visible to the guest as `NAME/...`, read-only, for `run`,
`restore` and `repl`. Grants are opened at startup and are not part of a tape,
so a restore must grant them again.

```lisp
(read-file "site/notes.txt")     ; binary-safe string, up to 64 MiB
(file-status "site/index.html")  ; #S(FILE-STATUS :KIND :FILE :SIZE "1234" ...)
(list-directory "site")          ; sorted names
(serve-file "site/app.wasm")     ; streamed, Content-Type from the extension
```

Paths are `/`-separated plain names beneath a grant: empty, `.` and `..`
segments are rejected, and symlinks are never followed, even ones that stay
inside the tree (stricter than WASI). FIFOs and devices are not read. Failures
raise `:NOT-CAPABLE` (ungranted or symlinked), `:NOT-FOUND`,
`:PERMISSION-DENIED`, `:INVALID-ARGUMENT` or `:IO`. `file-status` gives size and
modification time (Unix ms) as decimal strings, since fixnums are 31 bits.

File operations never stall other callbacks. On io_uring, opens and stats are
kernel operations, with `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)`
enforcing the same confinement in the kernel. Elsewhere (epoll, kqueue), and
for listing and reads where the wand would block, they run on a small
`blocking_pool` started on first use ([`nxtrt::fs::files`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/fs.hpp)).
On macOS, listing reads names and attributes in `getattrlistbulk` batches.

A response body may be `(make-file-body path)` instead of a string. The native server
opens it when the handler finishes and streams it with `Content-Length`,
outside the heap and past the 8 MiB string-body limit. `serve-file` checks the
path first and answers 404 for anything but a regular file. Each streamed chunk
gets a fresh write deadline, so long downloads only need to keep making
progress.

## Run granted programs

Programs are capabilities too. `--run NAME` finds NAME on `PATH` at startup
and `--run NAME=PATH` names an executable; the guest can then run only those,
by name, and never searches `PATH` itself:

```lisp
(run-command "git" "status" "--short")
;; #S(PROCESS-RESULT :EXIT-CODE 0 :SIGNAL NIL :OUTPUT " M README.md\n")
```

Stdin is empty, stdout and stderr arrive merged, and a nonzero exit or a
signal is a result rather than an error. Output beyond 8 MiB terminates the
program and raises `:TOO-LARGE`. Cancelling the awaiting activation (such as
an HTTP handler timeout) terminates and reaps the program; it signals only the
program itself, not processes it started. Like HTTP, commands are rejected in
checkpoint mode. A program grant is coarse: the program runs with the host's
full authority, whatever its directory grants say.

The reusable C++ API is [`nxtrt::http::serve`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/http-server.hpp), with a
borrowed listener, `task<response>(request)` handler, and bounded server options.
It supports HTTP/1.1 keep-alive, pipelining, fixed-length/chunked requests, and
binary bodies. Defaults bound headers to 16 KiB, request bodies to 1 MiB,
responses to 8 MiB, and connections to 64. Whole-phase deadlines cover headers
(10s), bodies, handlers, and writes (30s each). Cancellation drains operations
before socket close; timed-out handlers are cancelled and their actual pending
native waits are drained.
An async accept feed supplies connection recipes to the native bounded `pool`;
completed connections return admission capacity when consumed. There are no
permanent connection workers. Pool close cancels and drains both acceptance
and connections while their descriptors, handlers, and storage remain alive.
TLS, access logging, and public exposure belong to your proxy. Forwarded headers
are untrusted; upgrades, CONNECT, and Expect are rejected. Responses can stream
a file (`http::file_body`); other streaming, request-body streaming,
WebSockets, effect-result logging, and external-resource restore remain
future work.

## Make HTTP requests from Wisp

`(fetch-http url &optional method headers body)` uses NXT's existing native
client stack. The method defaults to `"GET"`, headers to `nil`, and body to
empty. It returns an `http-response`, the struct handlers fill in, so a
handler can relay it with `send! :respond`.
Headers are a list of `[name value]` vectors, preserving order and duplicates;
the body is a binary-safe string, already de-chunked and decompressed. HTTP
error statuses such as 404 are normal responses, not guest exceptions.

With the demo running, save these forms as `client.wisp` and run
`build/wisp run client.wisp`. In `build/wisp repl`, enter each complete form on
one line instead:

```lisp
;; Read a page; PRINT shows metadata, WRITE emits the body bytes.
(let ((r (fetch-http "http://127.0.0.1:8080/")))
  (print (http-response-status r))
  (print (http-response-headers r))
  (write (http-response-body r)))

;; Send a body with application headers.
(fetch-http "http://127.0.0.1:8080/echo" "POST"
  (list (vector "Content-Type" "text/plain")) "hello from the client")

;; Await a native composite task directly. Other pending I/O can progress
;; while this call is suspended.
(await (vector :http-fetch
        (vector "http://127.0.0.1:8080/" "GET" nil "")))
```

The demo's `/relay` route also uses `fetch-http` *inside a server handler*,
POSTing its body to `/echo` on the same listener and returning that response
with `send! :respond`. Waiting for network I/O suspends only this guest
activation; other callbacks can accept and handle the nested request. Don't
forward arbitrary client-selected URLs without an explicit access policy.

The initial client interface buffers the whole response, opens one connection
per call, and does not follow redirects, retry, pool connections, stream, or
manage cookies. It uses the existing URL parser: DNS/IPv4 hosts, numeric ports,
and escaped paths; include `/` before a query string. Userinfo, fragments and
IPv6 URL literals are not supported by this binding. `Host`, `Content-Length`,
`Connection`, transfer framing, upgrades and expectations are host-managed;
attempts to set them or inject control characters are rejected. It advertises
gzip/deflate and can also decode zstd/brotli when compiled in. Response headers
remain the original wire metadata: their Content-Length/Content-Encoding may
describe the compressed body, not the returned decoded string.

Request bodies are limited to 1 MiB, request heads to 16 KiB, each response head
to a 16 KiB reader buffer, and decoded response bodies to 8 MiB. A 30-second
whole-request deadline raises a `host-error` with code `:TIMEOUT`.
Other transport/protocol errors raise `:IO`; argument validation raises
`:INVALID-ARGUMENT`. Catch these with Wisp's `try` as with other host conditions.
DNS uses c-ares when available (the libc fallback performs blocking resolution).
Both `serve-http` and `fetch-http` reject `--checkpoint` with `:NOT-REPLAYABLE`:
there is no effect-result log or restoration of live sockets.

**HTTPS authenticates the server.** NXT's handmade TLS 1.3 client uses
libcrypto's X.509 verifier (AWS-LC in Nix) to check chain trust, signatures,
validity dates, CA constraints, critical extensions, and TLS server usage.
The URL host must match a subject alternative name: DNS names allow only
whole-label wildcards; IP literals require an IP SAN. Common-name fallback is
disabled. DNS connections send SNI for virtual-host selection; IP connections
do not. CertificateVerify and Finished are both required and verified before
any HTTP request is sent.

Trust comes from libcrypto's default CA paths, configurable with
`SSL_CERT_FILE` (PEM bundle) and `SSL_CERT_DIR` (hashed CA directory). Native
clients can instead call `handshake(host, ca_file)` to use only an explicit
PEM bundle. For private servers, configure trust in a local CA and use a
server certificate signed by it (AWS-LC does not treat a self-signed non-CA
leaf in a CA bundle as a trust anchor). There is no verification-disable
option. The TLS implementation remains experimental: it does not perform
online revocation (OCSP/CRL) or Certificate Transparency checks and is not a
browser-equivalent or audited TLS stack.

## Boot image and tape compression

Wisp embeds `base.wisp` and `host.wisp` directly with `#embed`, without
generated C++ source headers. The Wisp tool boots those libraries once at
build time, writes a zlib-compressed binary tape, and embeds that tape with
`#embed` too. Fresh runs and REPLs decompress and decode a private copy;
restores still use only the selected checkpoint. Library source, evaluator,
or tape-codec changes regenerate the embedded image automatically. Zlib is
already a required dependency; this does not require zstd or Brotli. Disable
boot compression with `meson configure build -Dwisp_boot_compression=false`.

The tape API also accepts compressed tapes from files or byte buffers.
Pass `wisp::tape::compression::zlib` as the final argument to `encode` or
`write` to opt in; `decode` and `read` detect compression automatically.
Default API writes and CLI checkpoints remain uncompressed, with the same
version-3 bytes as before. Compressed tapes retain the inner SHA-256 and
schema validation; the size limit applies to both compressed and expanded
bytes, and the expanded count is checked before allocating its buffer.

## How the web server fits into NXT

This is not a second runtime or a separate web framework. The HTTP server is
a reusable `nxtrt` service; Wisp supplies application handlers through the
executable host. Native C++ applications can call `nxtrt::http::serve` without
Wisp at all. The same scheduler and byte-stream abstractions also underpin
terminal applications and the OpenAI/SSE client.

```diagram
┌───────────────────────────────────────────┐
│ Wisp: serve-http / fetch-http              │
│ Portable heap, evaluator and effects      │
└─────────────────────┬─────────────────────┘
                      │ :host effect
┌─────────────────────▼─────────────────────┐
│ Host: heap continuations ↔ native tasks    │
└─────────────────────┬─────────────────────┘
                      │ awaits
┌─────────────────────▼─────────────────────┐
│ nxtrt: deck, pools, tasks, I/O operations  │
│ HTTP server / DNS+TCP+TLS client / buffers │
└─────────────────────┬─────────────────────┘
                      │
┌─────────────────────▼─────────────────────┐
│ OS sockets and timers                     │
│ epoll / io_uring / kqueue                  │
└───────────────────────────────────────────┘
```

An incoming request is parsed and bounded by the native server, copied into
the Wisp heap as an `http-request` struct, then run as a rooted heap
activation until it returns or explicitly awaits. `%nxt-http-handle` dynamically
binds `*request*` and `*response*` for that activation and installs the
`:respond` prompt. Ordinary handler returns are discarded;
the mutated response is used unless `send! :respond` exits early. Dynamic
bindings keep overlapping requests isolated, while packages/globals are shared.

For outbound I/O, `fetch-http` calls `await` with a `:HTTP-FETCH` descriptor.
The public `await` sends a `:host` effect; its captured resume/raise
continuations and request are stored in the heap. The host selects the native
implementation, copies its arguments into native storage, and awaits DNS,
connect, optional TLS, and body
decoding. On completion it roots a heap response and resumes the guest; on
failure it raises a host condition. Guest control state stays in the moving
heap, while native coroutine frames temporarily own sockets and buffers. A
server handler deadline cancels and drains the actual pending native wait before
socket teardown. HTTP activations are not guest jobs or handles.

Useful source entry points:

- [`src/wisp/host.wisp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/host.wisp): the small Lisp-facing interface
  and effect/dynamic-binding policy, evaluated into the executable's boot tape.
- [`src/wisp/main.cpp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/main.cpp): host effects, rooting,
  request conversion, native awaits and response/error delivery.
- [`src/wisp/nxt.hpp`](https://github.com/mbrock/nxtui/blob/main/src/wisp/nxt.hpp): optional `drive` adapter for bounded
  evaluator turns; the executable host runs guest activations to explicit await.
- [`src/nxtrt/http-server.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/http-server.hpp): native server contract
  and limits; request parsing and response serialization use local HTTP/1.1
  code over NXT byte feeds and sinks, with no Beast or Asio dependency.
- [`src/nxtrt/http.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/http.hpp),
  [`net_dns.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/net_dns.hpp), and [`tls.hpp`](https://github.com/mbrock/nxtui/blob/main/src/nxtrt/tls.hpp): the
  client serializer, response/body readers, connection and TLS layers also
  composed by [`demo/http_client_demo.cpp`](https://github.com/mbrock/nxtui/blob/main/demo/http_client_demo.cpp).
- [`src/nxt`](https://github.com/mbrock/nxtui/tree/main/src/nxt): scheduler-independent protocol/crypto utilities;
  `nxtrt` adds asynchronous runtime integration, and `nxtui` builds UI on top.
