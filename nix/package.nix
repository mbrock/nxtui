# The C++ libraries, CLI tools, and demos, built with the same meson
# setup used outside Nix. Only the files meson reads are in the source,
# so editing docs or the Racket model does not trigger a rebuild.
{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  boost,
  utf8proc,
  zlib,
  brotli,
  zstd,
  c-ares,
  aws-lc,
  python3,
  bash,
  coreutils,
  procps,
  sdl3 ? null,
  sdl3-ttf ? null,
  dejavu_fonts ? null,
  graphicalUi ? false,
  cryptoLibrary ? aws-lc,
  doCheck ? true,
}:

stdenv.mkDerivation {
  pname = "nxt";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../meson.options
      ../src
      ../test
      ../demo
      ../bench
      ../vendor/hub
      ../vendor/libvterm
      ../vendor/mdspan
    ];
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];

  buildInputs = [
    boost
    utf8proc
    zlib
    brotli
    zstd
    c-ares
    cryptoLibrary
  ]
  ++ lib.optionals graphicalUi [
    sdl3
    sdl3-ttf
  ];

  # libcrypto validates TLS certificates and supplies crypto test fixtures.
  # The default AWS-LC provider also enables the ML-KEM-768 test wrapper.
  nativeCheckInputs = [ python3 ] ++ lib.optionals stdenv.hostPlatform.isLinux [ procps ];

  postPatch = ''
    # These tests exercise explicit executable paths, rather than PATH lookup.
    # Use store paths in the sandbox, where /bin/sleep does not exist.
    substituteInPlace test/process-test.cpp test/uring-wand-test.cpp \
      --replace-fail '"/bin/sh"' '"${bash}/bin/sh"' \
      --replace-fail '"/bin/sleep"' '"${coreutils}/bin/sleep"'
  '';

  mesonBuildType = "release";

  mesonFlags = [
    (lib.mesonBool "dev" false)
    (lib.mesonBool "benchmarks" false)
    (lib.mesonBool "tests" doCheck)
    (lib.mesonEnable "cares" true)
    (lib.mesonEnable "sdl_ui" graphicalUi)
  ];

  preCheck = lib.optionalString graphicalUi ''
    demo/agent_chat/nxtui-graphical-tests \
      ${dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf
    SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
      demo/agent_chat/nxt-agent-chat-demo \
      --font ${dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf --exercise
  '';

  postInstall = lib.optionalString graphicalUi ''
    install -Dm644 ${dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf \
      $out/share/nxtui/DejaVuSans.ttf
  '';

  inherit doCheck;

  meta = {
    description = "C++23 coroutine runtime, terminal toolkit, and LLM tooling";
    homepage = "https://github.com/mbrock/nxtui";
    platforms = lib.platforms.linux ++ lib.platforms.darwin;
    mainProgram = "nxtllm";
  };
}
