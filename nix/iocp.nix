# Cross-built with nixbox's MSVC-ABI UWP SDK, not MinGW.
{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
}:
stdenv.mkDerivation {
  pname = "nxtrt-iocp";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../meson.options
      ../src/nxtrt
      ../test/iocp-wand-test.cpp
    ];
  };
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];
  mesonBuildType = "release";
  mesonFlags = [
    "-Ddefault_wand=iocp"
    "-Dtests=true"
    "-Db_vscrt=mt"
  ];
  # Verify the installed headers and pkg-config link contract, not only
  # Meson's in-tree dependency (which links archives by their full path).
  postInstall = ''
    $CXX -std=c++23 "$src/test/iocp-wand-test.cpp" \
      $(PKG_CONFIG_PATH="$out/lib/pkgconfig" $PKG_CONFIG --cflags --libs nxtrt-iocp) \
      -o "$TMPDIR/iocp-consumer.exe"
  '';
  # The test executable is cross-linked and installed for a Windows runner.
  # Nix cannot execute a UWP binary on the Linux build host.
  doCheck = false;
  meta = {
    description = "Coroutine runtime IOCP backend for Windows UWP/Xbox";
    homepage = "https://github.com/mbrock/nxtui";
    platforms = lib.platforms.windows;
  };
}
