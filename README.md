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
    rt.run(main_task());
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

## Repository map

- `src/nxtrt` — the structured coroutine runtime.
- `src/nxtui` — terminal, raster, compositor, input, and layout code.
- `src/nxtai` — the LLM/OpenAI client and `nxtllm`.
- `src/nxt` — shared protocol and utility code (crypto, TLS, JSON, PNG,
  stacktraces) not tied to one root namespace.
- `demo` — small runtime, terminal, HTTP, SSE, and shell demos.
- `test` — the nested `_test` suites.
- `docs` — the API documentation source, including the conceptual pages above.

## Building

This repo builds with Meson and does not require Nix:

```sh
meson setup build
meson compile -C build
build/nxt-tests
```

The default build produces `nxt-tests`, `nxtllm`, `libnxt-core.a`, and the demo
programs. Try the small TUI demo with:

```sh
build/demo/nxt-tui-demo
```

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
