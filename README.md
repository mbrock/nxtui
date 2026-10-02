# nxt

`nxt` is a set of C++23 libraries for coroutine-driven command-line software:
a structured async runtime, a terminal rendering toolkit, and an LLM/tooling
layer — three namespaces that all share **one** coroutine runtime, so a
terminal HUD, an HTTP/TLS request, a subprocess, and a streaming LLM agent are
all just tasks on the same deck.

```cpp
#include <nxtrt/app.hpp>
using namespace std::chrono_literals;

nxtrt::task<void> main_task()
{
    co_await nxtrt::op::timeout::after(30ms);
}

int main()
{
    auto rt = nxtrt::runtime{};
    rt.run(main_task);
}
```

The surface is organized around three root namespaces:

- [`nxtrt`][nxtrt] — the coroutine runtime: tasks, scheduling, structured
  child work, async I/O, byte streams, subprocesses, and terminal app loops.
- [`nxtui`][nxtui] — the terminal/raster/layout toolkit: typed screen geometry,
  styled text, composable layout values, and terminal compositing.
- [`nxtai`][nxtai] — the OpenAI/LLM client and tool-execution layer behind the
  `nxtllm` executable.

## The quest

`nxt` is, honestly, a search more than a library. The code runs, but the real
project is the hunt for *one* model of concurrent, effectful computation that
is at the same time **coherent** (the pieces genuinely fit, instead of sitting
in adjacent layers pretending to), **correct** (you can say what it does and
check it), and **efficient** (the already-done case pays for nothing). Most
systems get one or two of those. I want all three at once, and I'm not
convinced anyone knows how yet — including me.

A symptom you'll notice immediately: nearly everything is named with a short,
plain, slightly-off word. `deck`, `wand`, `firm`, `deed`, `wish`, `urge`,
`need`, `hope`, `feed`, `sink`, `game`, `task`, `exec`, `coin`. Four letters,
chosen for sound and resonance as much as for precision. This is deliberate,
and it is a *technique*, not a bit. Odd names keep the concepts **soft**: a
`wand` doesn't arrive pre-loaded with decades of "Executor" baggage, so I can
keep asking what it really is — and I do, constantly. Maybe wands don't exist.
Maybe the deck is a scam. Maybe the whole thing is secretly just a *rack*. The
words are handles for moving the furniture, not labels bolted to it. Renaming
is a first-class operation here; if you get attached to the vocabulary, the
vocabulary has won and the model stops moving.

What the search keeps converging on is a single instinct: **everything here is
a way of holding work that isn't running yet, and the only real question is
what decides when it comes back.** A scheduler holds resumptions; a backend
holds outstanding requests; a buffered stream holds bytes; the smallest
awaitable holds *either a value or the work to get it*. Same shape, four sizes
— the [holding essay][rt-holding] is the long version, and it ends on the
moment those four collapse into one.

The likeliest **crown jewel** is the [`game`][rt-game]: behavioral programming
(request / wait / block) as a pile of tiny independent tasks coordinating
through events. It may be the coordination semantics the runtime has been
missing — the thing that makes `deck`, `wand`, and `firm` facets of one idea
rather than three good ideas in a trench coat. (More context, plus the
async-`exec` extension I haven't ported yet, lives in `etc/bthreads-ts/`.)

And because I refuse to *only* hand-wave, the model is also written down
formally. `nxtrt/runtime.rkt` is `#lang rdf-forge` — a small homemade language
that is at once an OWL **ontology**, an Alloy-style **relational model**, and a
**temporal** spec, in one file. It describes the runtime's own `deck` / `firm`
/ `task` / `wish` / `exec` and the lifecycle an `exec` moves through (prepared
→ parked → settled → retired), states invariants as predicates, and lets the
checker search bounded **traces** — then renders straight into these docs.
That is a thread of its own: I want domain, ontological, and temporal modeling
to be *one* coherent tool, not Protégé and Alloy and TLA⁺ in three windows
that don't talk to each other.

So the open questions, right now, are roughly:

1. **Zig's buffers in C++ with coroutines.** Mostly cracked: generic *value*
   buffers, with the byte streams as the `<byte>` specialization — feeds,
   sinks, and the `hope<T>` hot path that makes the buffered case free. (See
   `src/nxtrt/value-buffers.hpp` and the [holding essay][rt-holding].)
2. **A coherent unifying theory of `deck` / `wand` / `firm`** — and whether
   the [`game`][rt-game] is the missing piece that fuses them into one.
3. **Modeling without a pile of tools** — domain + ontology + time in a single
   language (`rdf-forge`), pointed back at the runtime it describes.
4. **Much, much more.** This list is not closed, and neither is the vocabulary.

None of this is settled. That's the point — it's a working model, in both
senses. If a name here annoys you, good: hold it loosely, like I'm trying to.

## Start here

If you read nothing else, read these pages, in order. They are the
conceptual spine of the project:

| Page | What it is |
| --- | --- |
| [**Runtime overview**][rt-overview] | The map. Every core type — `task`, `deck`, `firm`, `wish`, `wand` — and how they fit, in one page. Start here. |
| [**A story about holding work**][rt-holding] | The narrative. Why the deck, the wand, the byte streams, and `hope<T>` are all the *same* idea — a holder with a release policy — and the endgame where they merge. |
| [**Runtime RFCs**][runtime-rfcs] | The design notebook. Current and speculative RFCs for firms, wands, feeds, reels, buffer land, and the runtime vocabulary. |
| [**RFC 0001: Reels**][rfc-reels] | The framing note. Reels are frame-shaped projections over `bytefeed` stock: raw bytes becoming marked frames, before anything turns into owned values. |
| [**The game**][rt-game] | The one programming model in the runtime: behavioral programming (request / waitFor / block) as small composable `task`s, built on top of the same machinery. |
| [**Occurrent structure**][rt-occurrents] | The ontology note. Behavioral threads, coroutines, and structured concurrency as process parts, boundaries, and shared happenings. |

## nxtrt — the runtime

[`nxtrt`][nxtrt] is the base layer. Its core is small and explicit:

- [`task<T>`][nxtrt-task] — a lazy coroutine that runs only when a deck resumes
  it.
- [`deck`][nxtrt-deck] — the cooperative scheduler; a queue of resumptions
  pumped one round at a time.
- [`wand`][nxtrt-wand] — the platform backend boundary; concrete `uring` and
  `kqueue` wands stage and complete I/O wishes.
- [`firm`][nxtrt-firm] / [`deed<T>`][nxtrt-deed] — structured concurrency:
  fork child tasks, join them, stop them together, recover their results.
- [`channel<T>`][nxtrt-channel] and [`event`][nxtrt-event] — coordination
  primitives; low-level awaitables live under [`nxtrt::op`][nxtrt-op].
- [`game<Event>`][nxtrt-game] — [behavioral programming][rt-game] over tasks.

Above the scheduler core sit byte streams (Zig-`std.Io`-shaped feeds and sinks
with a `hope<T>` hot path — see [the holding essay][rt-holding]) and protocol
and process helpers:

- [`nxtrt::fs`][nxtrt-fs] — async file helpers
- [`nxtrt::http`][nxtrt-http] / [`nxtrt::tls`][nxtrt-tls] — the HTTP/TLS client
  stack
- [`nxtrt::subprocess`][nxtrt-subprocess] / [`nxtrt::pty`][nxtrt-pty] — child
  process work
- [`nxtrt::terminal_app`][nxtrt-terminal-app] — terminal guest applications

## nxtui — the terminal toolkit

[`nxtui`][nxtui] is the rendering library. Its values are useful on their own —
the UI layer defines the data model and rendering primitives, and a live
terminal app uses `nxtrt` to drive input, refresh, and process output.

The core pieces are typed geometry ([`Size`][nxtui-size], [`Pos`][nxtui-pos]),
color and style ([`Rgba8`][nxtui-rgba]), the raster surfaces
([`Raster`][nxtui-raster], [`RasterView`][nxtui-raster-view]), the
[`GlyphTable`][nxtui-glyph-table], ANSI and input helpers, and composable
layout values under [`nxtui::tui`][nxtui-tui] rendered by a
[`TerminalCompositor`][nxtui-terminal-compositor].

```cpp
#include <nxtui/tui.hpp>
using namespace nxtui::tui;

auto view = column(
    text("build", fg(nxtui::Rgba8::cyan()) | bold),
    row(
        text("compile"),
        progress_bar(64.0 * nxtui::percent),
        text(" 64%")));
```

A common use is a partial terminal HUD: a fixed-height layout lives at the
bottom of the terminal while ordinary process output keeps scrolling above it.

## nxtai — the LLM layer

[`nxtai`][nxtai] builds OpenAI Responses requests
([`openai_responses_request`][nxtai-request]), streams server-sent events over
the `nxtrt` HTTP/TLS stack, runs tools through a
[`tool_registry`][nxtai-tool-registry], and ships the small `nxtllm`
executable. The OpenAI event/data types live under
[`nxtai::openai`][nxtai-openai].

```sh
build/nxtllm --dump-request "hello from nxtrt"
```

The executable currently handles one text response, not a model/tool/model
agent loop. The separate tool library exposes pool-ready call recipes and a
bounded batch collector (four concurrent calls by default), preserving ordered
batch results without firm child records or deeds. See the
[NXTAI status and next steps](docs/ai-overview.md) for what is connected,
ownership/error contracts, and the remaining integration work.

## Wisp — portable Lisp machines and HTTP on NXT

The `wisp` executable runs Wisp's guest evaluator on the same NXT deck, with
console effects, explicit `await`, `sleep-ms` timers, and a loopback HTTP server.
The guest heap holds closures, suspended control state, source position, and
pending requests; native coroutines own temporary waits and sockets.
See [RFC 0018](rfc/new/rfc-0018-portable-wisp-lisp-machines.md)
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

### Structs, records and conditions

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
[`src/wisp/host.wisp`](src/wisp/host.wisp): `http-request`, `http-response`,
`file-status`, `file-body` and `host-error`. The C++ host finds their slots by
name through the descriptors, so field order is declared only there.

### Load local Wisp files with explicit read grants

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
[loading contracts and runnable example](rfc/new/rfc-0018-portable-wisp-lisp-machines.md#local-source-loading-uses-existing-read-grants)
for retries, overlapping loads, and restore authority.

### Serve plain HTTP behind a reverse proxy

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

### Files and capabilities

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
`blocking_pool` started on first use ([`nxtrt::fs::files`](src/nxtrt/fs.hpp)).
On macOS, listing reads names and attributes in `getattrlistbulk` batches.

A response body may be `(make-file-body path)` instead of a string. The native server
opens it when the handler finishes and streams it with `Content-Length`,
outside the heap and past the 8 MiB string-body limit. `serve-file` checks the
path first and answers 404 for anything but a regular file. Each streamed chunk
gets a fresh write deadline, so long downloads only need to keep making
progress.

The reusable C++ API is [`nxtrt::http::serve`](src/nxtrt/http-server.hpp), with a
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

### Make HTTP requests from Wisp

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

### How the web server fits into NXT

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
│ nxtrt: deck, firms, tasks, I/O operations  │
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

- [`src/wisp/host.wisp`](src/wisp/host.wisp): the small Lisp-facing interface
  and effect/dynamic-binding policy, evaluated into the executable's boot tape.
- [`src/wisp/main.cpp`](src/wisp/main.cpp): host effects, rooting,
  request conversion, native awaits and response/error delivery.
- [`src/wisp/nxt.hpp`](src/wisp/nxt.hpp): optional `drive` adapter for bounded
  evaluator turns; the executable host runs guest activations to explicit await.
- [`src/nxtrt/http-server.hpp`](src/nxtrt/http-server.hpp): native server contract
  and limits; its implementation uses Boost.Beast for request parsing.
- [`src/nxtrt/http.hpp`](src/nxtrt/http.hpp),
  [`net_dns.hpp`](src/nxtrt/net_dns.hpp), and [`tls.hpp`](src/nxtrt/tls.hpp): the
  client serializer, response/body readers, connection and TLS layers also
  composed by [`demo/http_client_demo.cpp`](demo/http_client_demo.cpp).
- [`src/nxt`](src/nxt): scheduler-independent protocol/crypto utilities;
  `nxtrt` adds asynchronous runtime integration, and `nxtui` builds UI on top.

## Repository map

- `src/nxtrt` — the structured coroutine runtime.
- `src/nxtui` — terminal, raster, compositor, input, and layout code.
- `src/nxtai` — the LLM/OpenAI client and `nxtllm`.
- `src/wisp` — the guest Lisp machine, moving heap, tapes, and executable host.
- `src/nxt` — shared protocol and utility code (crypto, TLS, JSON, PNG,
  stacktraces) not tied to one root namespace.
- `demo` — small runtime, terminal, HTTP, SSE, and shell demos.
- `test` — the nested `_group`/`_test` suites (`build/nxt-tests --slow`
  adds the slow integration tier).
- `docs` — the API documentation source, including the conceptual pages above.

## Building

This repo builds with Meson 1.3+ and does not require Nix. Building requires a
C++23 compiler with `#embed` support (GCC 15+ or Clang 19+). `#embed` is
standard in C23/C++26 and supported as an extension in C++23 mode; the
project still uses `-std=c++23`. Meson checks support at configure time.

```sh
meson setup build
meson compile -C build
build/nxt-tests
```

New build directories default to `debugoptimized`: optimization and debug
symbols, with assertions enabled. Use `meson setup build --buildtype=debug`
for an unoptimized build, or `meson configure build -Dbuildtype=debugoptimized`
to update an existing debug build.

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

The default build produces `nxt-tests`, `nxtllm`, `nxtmt`, `wisp`, the shared
`libnxt-core`, the demo programs, and the `nxt-dev` developer command bundle.
Try the small TUI demo with:

```sh
build/demo/nxt-tui-demo
```

`meson install` gives a self-contained install: the libraries, tools, demos,
public headers (with the vendored libvterm/mdspan/hub headers under
`include/nxt-vendor`), and an `nxt.pc` for pkg-config. Consumers also need
Boost headers and `-std=c++23`.

`libcrypto` is required for certificate verification; AWS-LC is provided by
Nix, and OpenSSL is also supported for X.509 verification. The crypto
cross-check tests (RSA/ECDSA fixtures and ML-KEM-768) expect AWS-LC's headers;
with OpenSSL those optional cross-checks are skipped at configure time.

Default firms acquire frame land lazily in non-relocating chunks rather than
reserving 4 MiB per scope. Chunks are reused until the firm is destroyed;
explicit static or borrowed frame land remains bounded. Callers can still
preallocate an `owned_frame_storage` rack and lend its view to a firm.

Meson keeps allocator poisoning enabled for everyday tests and the repeated
HTTP stress tier. To run the stress cases with poisoning explicitly:

```sh
MALLOC_PERTURB_=17 build/nxt-tests --only-slow
```

### Continuous integration

[GitHub Actions](https://github.com/mbrock/nxtui/actions/workflows/tests.yml)
runs on pushes and pull requests with the locked Nix development toolchain:

| Runner | Application Wand |
| --- | --- |
| Ubuntu 24.04 | io_uring |
| Ubuntu 24.04 | epoll |
| macOS 15 (Apple Silicon) | kqueue |

Each job runs `meson test`: the `nxt-tests` binary, its slow tier
(`nxt-slow-tests`), Wisp allocation-failure tests, and the Wisp host and HTTP
integration tests (also in the `slow` suite). Linux also includes the direct
epoll and io_uring suites; macOS includes the kqueue suite. Meson logs are
uploaded even on failure. Builds use `debugoptimized` (optimization plus
debug symbols, with assertions still enabled); the test harness's per-test
deadlines remain intact. Backends are not silently skipped or substituted.

To reproduce a matrix leg inside `nix develop`:

```sh
meson setup build/ci --buildtype=debugoptimized -Ddefault_wand=epoll -Ddemo=false -Ddev=false -Dllm_tool=false -Dcares=enabled
meson compile -C build/ci -j 2 nxt-tests wisp-alloc-tests wisp-root-link
meson test -C build/ci --print-errorlogs --timeout-multiplier 3
```

Use `uring` on Linux or `kqueue` on BSD/macOS instead of `epoll`. The default
`auto` retains io_uring on Linux and kqueue on BSD/macOS. The selected default
is propagated to library consumers through the Meson dependency and
pkg-config flags, since the application runtime contains a concrete Wand.

### Wisp coverage (GCC)

Use a separate instrumented build, inside `nix develop` or with a matching
GCC/Meson toolchain. This does not change the normal build's flags:

```sh
meson setup build/coverage -Db_coverage=true -Ddemo=false -Ddev=false -Dllm_tool=false
meson compile -C build/coverage nxt-tests wisp-alloc-tests wisp-root-link
find build/coverage -name '*.gcda' -delete  # reset counters before each comparison
build/coverage/test/nxt-tests 14 15 16 17 18 19 20 21
build/coverage/test/wisp-alloc-tests
gcov --json-format -b -c --stdout build/coverage/src/libnxt-core.so.p/wisp_*.gcno \
  build/coverage/test/wisp-alloc-tests.p/.._src_wisp_heap.cpp.gcno \
  > build/coverage/wisp-coverage.jsonl
```

The selectors above cover the Wisp suites in the nested test report. The
JSON-lines report includes line counts and branch outcomes for the core, not
the console host; add `meson test -C build/coverage wisp-host-tests wisp-http-tests`
to exercise host integration as well. Include both heap objects: allocation
fault injection runs in its own executable. Template instantiations and
separate objects can repeat source lines, so sum their execution counts by file
and line before computing line coverage. Uncovered exception paths and
compiler-generated branches are not necessarily missing language tests: use
the report to find contracts worth testing, not as a percentage target. Compare
the same workload with fresh counters, and assert results and snapshot
isolation, not just successful execution.

### With Nix

The flake is optional and wraps the same Meson build:

```sh
nix build            # ./result: libnxt-core, headers, nxt.pc, nxtllm, demos
nix flake check      # the package (with tests) plus a pkg-config consumer build
nix develop          # compilers, Meson, AWS-LC, clangd, docs tools
nix develop .#spec   # optional Racket + JDK environment for the runtime model
```

Inside `nix develop`, the plain C++ build/test and docs commands above work
as-is. The independent `spec` shell shares the same `flake.lock` but is not
part of the default development environment.

### In Amp orbs

`.agents/setup` installs Nix, realizes the development shell from `flake.lock`,
caches Poxy, and configures Meson. It does not download Racket or Java, install
Racket packages, or compile the model. Amp snapshots the base dependencies for
reuse by fresh orbs. Repeated setup runs reuse installed packages;
`.agents/resume` does not install anything.

Setup adds a repository-scoped login-shell hook so agents can run `make build`,
`make test`, and `make docs` directly from the repository root, without manually
entering `nix develop`. Model work is opt-in with
`nix develop .#spec -c make spec`. No API credentials are needed for these local
workflows.

The separate Bun graph-documentation workflow requires the sibling checkout
at `../src/forge-graph/packages/forge-graph` specified in `package.json`.
Setup installs its locked dependencies when that checkout exists and reports
the omission otherwise; this does not affect the C++ build, spec, or Poxy docs.

The orb's host kernel also matters. Subprocess waits use `io_uring` to poll
pidfds, then reap ready children with ordinary `waitid(P_PIDFD)`. This works
on Linux 6.1 without `IORING_OP_WAITID` (which requires Linux 6.7); the pidfd
wait mechanism itself requires Linux 5.4 or later. The `io_uring` backend
still requires io_uring to be enabled and permitted by the host; there is no
automatic switch to epoll if ring creation is unavailable.

On macOS and the BSDs, the kqueue backend runs the same process wishes
without pidfds. A child's handle is its pid, kept reserved by reading exit
status with `waitid(WNOWAIT)` and reaping only when the handle is destroyed,
so a signal can never reach a reused pid. Waits register `EVFILT_PROC`
`NOTE_EXIT`; a process caught mid-exit refuses that registration (`ESRCH`),
and the wait retries on a short timer. One difference remains: waiting twice
reports the same status again, where Linux reports `ECHILD`. On macOS,
`posix_spawn` uses `POSIX_SPAWN_CLOEXEC_DEFAULT`, so children inherit only
their standard streams. All wands share the spawn code in
[`nxtrt/spawn.hpp`](src/nxtrt/spawn.hpp).

### The runtime spec

`make spec` checks the Racket model (`nxtrt/runtime.rkt` and friends): its
example scenarios must be satisfiable and its lifecycle properties must hold
(for instance, that an exec only retires once its cancel CQE has drained, as
`is_retirable` requires); `make spec-witnesses` prints the example traces. It
needs Racket and a Java runtime (Forge's Pardinus solver runs on the JVM);
enter `nix develop .#spec` or run:

```sh
nix develop .#spec -c make spec
```

Racket, Java, and the compiled Racket libraries all come from Nix.
`nix/racket-sources.json` pins the external package closure to source revisions
and Nix content hashes; `nix/spec-racket.nix` installs and compiles those inputs
offline, including the patched Forge and Something sources in `vendor/racket/`.
The resulting `spec-racket` package is an ordinary cacheable Nix derivation:

```sh
nix build .#spec-racket
```

The dependency inputs are reproducibly locked and the output can be shared
through a normal Nix binary cache. Byte-for-byte rebuild reproducibility is
not guaranteed: `nix build .#spec-racket --rebuild` found differences in
Racket-generated `.zo` files and their dependency hashes, even with serial
compilation. This does not prevent substitution of a cached build.

No catalog resolution or package installation happens when entering the shell
or running `make spec`. Only bytecode for editable model/DSL sources goes into
the version-keyed `.racket/` cache; old local package installs there are ignored.
Editing a model does not rebuild the dependency package. `nix flake check`
also runs the specs in a clean Nix build sandbox.

To deliberately refresh the dependency lock from the Racket catalog (this is
the networked update step, not part of normal builds):

```sh
nix develop .#spec -c racket nix/update-racket-sources.rkt > nix/racket-sources.json.new &&
  mv nix/racket-sources.json.new nix/racket-sources.json
nix build .#checks.x86_64-linux.spec # use your system's check attribute
```

Review and commit the lock changes. The updater derives the closure from the
vendored packages' dependency declarations, excluding libraries already
provided by the Racket distribution pinned in `flake.lock`.

Regenerate local API docs (poxy + Doxygen) with:

```sh
make docs
```

Run the portable test subset on a FreeBSD VM with:

```sh
scripts/freebsd-vm init
scripts/freebsd-vm test
```

The helper uses libvirt, cloud-init, ssh, and rsync. It defaults to the official
FreeBSD 15.0 amd64 `BASIC-CLOUDINIT-ufs.qcow2.xz` image, stores local state
under `.cache/freebsd-vm`, and leaves the VM persistent for fast repeat runs.
The guest installs GCC 15 and uses `gcc15`/`g++15` for the test build. The host
needs `libvirt-daemon-system`, `virtinst`, and `cloud-image-utils`.

<!-- Concept pages -->
[rt-overview]: https://swa.sh/nxt/rt_overview.html
[rt-holding]: https://swa.sh/nxt/rt_holding.html
[rt-game]: https://swa.sh/nxt/rt_game.html
[rt-occurrents]: https://swa.sh/nxt/rt_occurrents.html
[runtime-rfcs]: https://swa.sh/nxt/runtime_rfcs.html
[rfc-reels]: https://swa.sh/nxt/rfc_reels.html

<!-- nxtrt -->
[nxtrt]: https://swa.sh/nxt/namespacenxtrt.html
[nxtrt-task]: https://swa.sh/nxt/classnxtrt_1_1task.html
[nxtrt-deck]: https://swa.sh/nxt/classnxtrt_1_1deck.html
[nxtrt-wand]: https://swa.sh/nxt/classnxtrt_1_1wand.html
[nxtrt-firm]: https://swa.sh/nxt/classnxtrt_1_1firm.html
[nxtrt-deed]: https://swa.sh/nxt/classnxtrt_1_1deed.html
[nxtrt-channel]: https://swa.sh/nxt/classnxtrt_1_1channel.html
[nxtrt-event]: https://swa.sh/nxt/classnxtrt_1_1event.html
[nxtrt-game]: https://swa.sh/nxt/classnxtrt_1_1game.html
[nxtrt-op]: https://swa.sh/nxt/namespacenxtrt_1_1op.html
[nxtrt-fs]: https://swa.sh/nxt/namespacenxtrt_1_1fs.html
[nxtrt-http]: https://swa.sh/nxt/namespacenxtrt_1_1http.html
[nxtrt-tls]: https://swa.sh/nxt/namespacenxtrt_1_1tls.html
[nxtrt-subprocess]: https://swa.sh/nxt/namespacenxtrt_1_1subprocess.html
[nxtrt-pty]: https://swa.sh/nxt/namespacenxtrt_1_1pty.html
[nxtrt-terminal-app]: https://swa.sh/nxt/classnxtrt_1_1terminal__app.html

<!-- nxtui -->
[nxtui]: https://swa.sh/nxt/namespacenxtui.html
[nxtui-size]: https://swa.sh/nxt/structnxtui_1_1_size.html
[nxtui-pos]: https://swa.sh/nxt/structnxtui_1_1_pos.html
[nxtui-rgba]: https://swa.sh/nxt/structnxtui_1_1_rgba8.html
[nxtui-raster]: https://swa.sh/nxt/classnxtui_1_1_raster.html
[nxtui-raster-view]: https://swa.sh/nxt/classnxtui_1_1_raster_view.html
[nxtui-glyph-table]: https://swa.sh/nxt/classnxtui_1_1_glyph_table.html
[nxtui-tui]: https://swa.sh/nxt/namespacenxtui_1_1tui.html
[nxtui-terminal-compositor]: https://swa.sh/nxt/classnxtui_1_1tui_1_1_terminal_compositor.html

<!-- nxtai -->
[nxtai]: https://swa.sh/nxt/namespacenxtai.html
[nxtai-request]: https://swa.sh/nxt/structnxtai_1_1responses_1_1openai__responses__request.html
[nxtai-tool-registry]: https://swa.sh/nxt/structnxtai_1_1tools_1_1tool__registry.html
[nxtai-openai]: https://swa.sh/nxt/namespacenxtai_1_1openai.html
