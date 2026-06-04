# Vendored Racket Dependencies

This directory vendors the patched Racket dependencies needed by `make spec`.

- `forge/` is based on `https://github.com/tnelson/forge`, upstream `v5.2`
  plus local patches for XML export and Sterling/run options.
- `something-src/` is based on
  `https://git.leastfixedpoint.com/tonyg/racket-something`, upstream `main`
  plus a reader tokenization patch needed by `#lang rdf-forge`.

The repo still uses the normal Racket package catalog for Forge's ordinary
third-party dependencies. On a fresh machine, run:

```sh
make setup-racket
make spec
```

`make spec` prepends this directory to `PLTCOLLECTS`, so the checked-in copies
win over any user-level `forge` or `something` package links.
