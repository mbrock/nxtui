# Pinned, compiled Forge dependencies. Model/DSL sources deliberately stay in
# the checkout, so editing a spec does not rebuild the dependency environment.
{
  lib,
  stdenvNoCC,
  fetchzip,
  racket,
  jdk21_headless,
  makeWrapper,
}:

let
  sources = lib.importJSON ./racket-sources.json;
  packages = lib.mergeAttrsList (
    map (
      source:
      let
        src = fetchzip { inherit (source) url hash; };
      in
      lib.mapAttrs (_: subdir: "${src}/${subdir}") source.packages
    ) sources
  );
  vendored = lib.fileset.toSource {
    root = ../vendor/racket;
    fileset = lib.fileset.unions [
      ../vendor/racket/forge
      ../vendor/racket/something-src
    ];
  };
in
stdenvNoCC.mkDerivation {
  pname = "nxt-spec-racket";
  version = "1";
  dontUnpack = true;

  nativeBuildInputs = [
    racket
    makeWrapper
  ];

  buildPhase = ''
    runHook preBuild
    export HOME="$TMPDIR/home"
    export PLTADDONDIR="$out/share/racket"
    mkdir -p "$HOME" "$out/sources"

    # Copy to stable output paths before installing/compiling: package links
    # and bytecode must not refer to the disposable build directory.
    ${lib.concatStringsSep "\n" (
      lib.mapAttrsToList (name: src: ''
        cp -R ${src} "$out/sources/${name}"
      '') packages
    )}
    cp -R ${vendored}/forge "$out/sources/forge"
    cp -R ${vendored}/something-src "$out/sources/something"
    chmod -R u+w "$out/sources"

    # All dependencies are explicit local inputs. Missing dependencies fail
    # rather than silently fetching from a mutable Racket package catalog.
    raco pkg install --batch --no-setup --deps fail "$out"/sources/*
    raco setup --no-docs --avoid-main -j "$NIX_BUILD_CORES" \
      --pkgs ${lib.concatStringsSep " " (builtins.attrNames packages)} forge
    # Something's experimental shells/examples do not compile in this
    # snapshot. Compile the supported DSL modules and their dependencies,
    # not those unused experiments (previously linked with --no-setup).
    raco make "$out/sources/something/something/base.rkt" \
      "$out/sources/something/something/infix.rkt" \
      "$out/sources/something/something/reader.rkt"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin"
    for tool in racket raco; do
      makeWrapper ${racket}/bin/$tool "$out/bin/$tool" \
        --set PLTADDONDIR "$out/share/racket" \
        --prefix PATH : ${lib.makeBinPath [ jdk21_headless ]}
    done
    runHook postInstall
  '';

  meta = {
    description = "Racket with pinned, offline-built dependencies for the nxtrt specs";
    platforms = lib.platforms.linux ++ lib.platforms.darwin;
    mainProgram = "racket";
  };
}
