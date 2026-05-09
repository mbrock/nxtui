# mdspan

This directory vendors the header-only Kokkos `mdspan` implementation used by
`nxt`.

- Upstream: https://github.com/kokkos/mdspan
- Version: `mdspan-0.6.0`
- License: Apache-2.0 WITH LLVM-exception, copied in `LICENSE`

Only the installed `include/experimental` header tree is vendored. `nxt` uses it
through `#include <experimental/mdspan>`.
