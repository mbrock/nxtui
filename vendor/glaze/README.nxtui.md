# Vendored Glaze

This directory contains the `include/` tree from:

- Upstream: https://github.com/stephenberry/glaze
- Version: `v7.6.0`
- Release: https://github.com/stephenberry/glaze/releases/tag/v7.6.0

Glaze is header-only for our intended use, so `meson.build` exposes
`vendor/glaze/include` as a `declare_dependency()` without using CMake
FetchContent or a Meson wrap.

To refresh this vendor copy:

```sh
tmpdir=$(mktemp -d)
curl -L --fail https://github.com/stephenberry/glaze/archive/refs/tags/v7.6.0.tar.gz \
  -o "$tmpdir/glaze-v7.6.0.tar.gz"
tar -xzf "$tmpdir/glaze-v7.6.0.tar.gz" -C "$tmpdir"
rm -rf vendor/glaze/include
cp -R "$tmpdir/glaze-7.6.0/include" vendor/glaze/include
rm -rf "$tmpdir"
```

See upstream for license details.
