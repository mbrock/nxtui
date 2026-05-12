# How this happened

It started with the question "do you see the color scheme in
`~/.emacs.d` called baltic birch and baltic church?" Yes, both
present.

We replayed an agent trace and noticed the ANSI got stripped through
the pipe. Grepped the source. Found a quietly-implemented
`nxt::ansi::Mode::debug` that emits `⟨CSI:0m⟩` instead of `\e[0m` — a
mode only the tests had ever used. Wired up `NXT_ANSI=disabled|debug|
enabled`, made `debug` the non-TTY default so we'd never lose
visibility into a piped session again.

That somehow led to talking about tool-call rendering. The realization
that a tool call isn't a row in a catalogue — it's a *cassette*. A
small designed artifact with a case, a spine label, a J-card, a window
onto the tape, printed metadata, affordances. The kind of thing you
recognize across a room. The kind of thing whose visual language
should live in *one* file, not in 759 lines of C++ doing string
splitting next to elaborate JSON ellipsization next to lifecycle
runners next to per-tool color tables.

We then remembered xtc — the obscure Zig terminal renderer with a real
DOM, miniflex layout, and Tailwind class resolver implemented through
OKLCH color conversion. Which suggested the obvious move:

```
agent trace
  →  XML document
  →  XSLT cassette stylesheet  (baltic-birch-v1.xsl)
  →  xtc markup
  →  ANSI
```

Each layer a flat file. Each transformation transparent. XSLT — the
1999 W3C technology that everyone politely left for dead — turned out
to be exactly the right tool for "match this element pattern, emit
this Tailwind layout fragment." `<xsl:template match="result[@kind=
'matches']">` is the same shape as `case kind of Matches -> ...` in
a typed visitor, just declarative.

Then DuckDB's `nanoarrow` and `webbed` community extensions showed up
to handle the data side: compact 31 per-run `.arrow` files into one
zstd Parquet (170 MB → 3 MB), then emit trace XML directly via one
SQL query using `html_escape`, `string_agg`, and `json_extract_string`.
The whole pipeline became:

```bash
make traces                              # nanoarrow → traces/all.parquet
./cassette/trace-render 1VGR6SG2         # parquet → xml → xsl → xtc → ansi
```

A real agent session, queried in place, transformed via XSLT, rendered
through xtc, emitted as Baltic-Birch-styled ANSI cassettes in the
terminal — with thought blocks, tool-call cards by kind (rg_search
amber, read_file emerald, bash terracotta, web_fetch violet), preview
lines in dim slate, and a header band one shade above the body so the
chrome reads as its own register.

We started with two emacs theme files.

We ended with a hypermedia rendering pipeline for AI agent traces,
assembled from:

  - A W3C standard from 1999 (XSLT).
  - A Zig binary nobody else uses (xtc).
  - A community DuckDB extension called `webbed`.
  - Another community extension called `nanoarrow`.
  - A bash-and-perl WebDriver wrapper (`wd`).
  - Lightpanda's accessibility tree dump.
  - The OKLCH color space.
  - Tailwind's shade taxonomy.

Each piece doing exactly its job. None designed to meet. The seams all
line up.
