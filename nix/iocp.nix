# Cross-built with nixbox's MSVC-ABI UWP SDK, not MinGW.
{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  boost,
  zlib,
  cryptoLibrary,
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
      ../src/nxt
      ../src/nxtai
      ../test/iocp-wand-test.cpp
      ../test/network-probe.cpp
    ];
  };
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];
  # Public headers and pkg-config require these in downstream build environments.
  propagatedBuildInputs = [
    boost
    zlib
    cryptoLibrary
  ];
  mesonBuildType = "release";
  mesonFlags = [
    "-Ddefault_wand=iocp"
    "-Dtests=true"
    "-Db_vscrt=mt"
  ];
  # The separate flake iocp-consumer check tests this package in isolation;
  # checking here would inherit private dependency paths and mask omissions.
  # The test executable is cross-linked and installed for a Windows runner.
  # Nix cannot execute a UWP binary on the Linux build host.
  doCheck = false;
  meta = {
    description = "Coroutine runtime IOCP backend for Windows UWP/Xbox";
    homepage = "https://github.com/mbrock/nxtui";
    platforms = lib.platforms.windows;
  };
}
