# Pinned, compiled Forge dependencies. Model/DSL sources deliberately stay in
# the checkout, so editing a spec does not rebuild the dependency environment.
{
  lib,
  stdenvNoCC,
  fetchzip,
  racket,
  racket-minimal,
  jdk21_headless,
  makeWrapper,
  callPackage,
}:

let
  # Command-line Racket avoids GTK and desktop wrapper dependencies on Linux.
  # Nixpkgs currently marks the minimal package broken on Darwin.
  racketRuntime = if stdenvNoCC.hostPlatform.isDarwin then racket else racket-minimal;
  sources = lib.importJSON ./racket-sources.json;
  packages = lib.mergeAttrsList (
    map (
      source:
      let
        src = fetchzip {
          inherit (source) url hash;
          # Racket release ZIPs contain the package files at the archive root.
          stripRoot = !(lib.hasSuffix ".zip" source.url);
        };
      in
      lib.mapAttrs (_: subdir: "${src}/${subdir}") source.packages
    ) sources
  );
  specSources = callPackage ./spec-sources.nix { };
in
stdenvNoCC.mkDerivation {
  pname = "nxt-spec-racket";
  version = "1";
  dontUnpack = true;

  nativeBuildInputs = [
    racketRuntime
    makeWrapper
  ];

  buildPhase = ''
    runHook preBuild
    export HOME="$TMPDIR/home"
    export PLTADDONDIR="$out/share/racket"
    mkdir -p "$HOME" "$out/sources"

    # --skip-installed only skips packages in the target scope. Full Racket
    # on Darwin provides many locked packages in the installation scope;
    # omit those explicitly instead of trying to reinstall them as user packages.
    racket -e '(require pkg/lib)
      (for ([name (in-hash-keys (installed-pkg-table #:scope (quote installation)))])
        (displayln name))' > "$TMPDIR/installed-packages"
    copyPackage() {
      if ! grep -Fxq "$1" "$TMPDIR/installed-packages"; then
        cp -R "$2" "$out/sources/$1"
      fi
    }

    # Copy to stable output paths before installing/compiling: package links
    # and bytecode must not refer to the disposable build directory.
    ${lib.concatStringsSep "\n" (
      lib.mapAttrsToList (name: src: ''
        copyPackage ${name} ${src}
      '') packages
    )}
    cp -R ${specSources}/forge "$out/sources/forge"
    cp -R ${specSources}/something "$out/sources/something"
    chmod -R u+w "$out/sources"
    # raco validates build-deps even with --no-setup. Remove only that metadata
    # from dependency copies so --deps fail still checks every runtime dep.
    racket ${./runtime-package-info.rkt} ${lib.concatMapStringsSep " " (name: ''"$out/sources/${name}"'') (builtins.attrNames packages)}

    # All dependencies are explicit local inputs. Missing dependencies fail
    # rather than silently fetching from a mutable Racket package catalog.
    # Darwin's full Racket already supplies some of the locked libraries.
    raco pkg install --batch --no-setup --deps fail "$out"/sources/*
    # Compile the backend entry points and their imports, not Forge's editor,
    # domain examples, browser UI, or every module in its dependencies.
    raco setup --no-docs --avoid-main -j "$NIX_BUILD_CORES" --pkgs compiler-lib
    raco make "$out/sources/forge/sigs-functional.rkt" \
      "$out/sources/forge/temporal/lang/temporal-lang-specific-checks.rkt" \
      "$out/sources/forge/server/modelToXML.rkt"
    # Something's experimental shells/examples do not compile in this
    # snapshot. Its patched package declares only the reader's dependencies.
    raco make "$out/sources/something/something/base.rkt" \
      "$out/sources/something/something/infix.rkt" \
      "$out/sources/something/something/reader.rkt"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin"
    for tool in racket raco; do
      makeWrapper ${racketRuntime}/bin/$tool "$out/bin/$tool" \
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
