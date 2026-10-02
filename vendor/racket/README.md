# Vendored Racket Dependencies

This directory vendors the patched Racket dependencies needed by `make spec`.

- `forge/` is based on `https://github.com/tnelson/forge`, upstream `v5.2`
  plus local patches for XML export, Sterling/run options, and optional Git
  metadata (a solver runtime does not need Git installed).
- `something-src/` is based on
  `https://git.leastfixedpoint.com/tonyg/racket-something`, upstream `main`
  plus a reader tokenization patch needed by `#lang rdf-forge`.

Nix installs and compiles these patched sources together with the external
dependency closure pinned in `nix/racket-sources.json`. On a fresh machine, run:

```sh
nix develop .#spec -c make spec
```

The shell uses the compiled copies in the Nix store, not user-level package
links. Changing these vendored sources rebuilds `nix build .#spec-racket`;
changing the editable models or `rdf-forge` does not. The Racket catalog is
consulted only by the explicit lock updater, `nix/update-racket-sources.rkt`.

Something's experimental shells/examples do not compile in this vendored
snapshot. The Nix package compiles its base/infix/reader modules and their
dependencies, which are the modules used by `rdf-forge`, rather than those
unused experiments.
