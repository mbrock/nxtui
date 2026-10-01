{
  description = "nxt: C++23 coroutine runtime, terminal toolkit, and LLM tooling";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        nxt = pkgs.callPackage ./nix/package.nix { };
        default = nxt;
      });

      devShells = forAllSystems (
        pkgs:
        let
          inherit (pkgs) lib stdenv;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${stdenv.hostPlatform.system}.nxt ];

            packages =
              with pkgs;
              [
                aws-lc
                clang-tools
                gnumake

                # `make spec`: the Racket model, and the JVM that Forge's
                # Pardinus/Kodkod solver backend runs on.
                racket
                jdk21_headless

                # `make docs`
                uv
                doxygen
              ]
              ++ lib.optionals stdenv.hostPlatform.isLinux [
                mold
                gdb
              ];

            shellHook = ''
              # Same project-local Racket package and compiled-code dirs the
              # Makefile uses, so plain `racket nxtrt/model.rkt` works here too.
              export PLTADDONDIR="$PWD/.racket"
              export PLTCOMPILEDROOTS="$PLTADDONDIR/$(racket -e '(display (version))')/compiled:same"
            '';
          };
        }
      );

      checks = forAllSystems (
        pkgs:
        let
          nxt = self.packages.${pkgs.stdenv.hostPlatform.system}.nxt;
        in
        {
          inherit nxt;

          consumer =
            pkgs.runCommandCC "nxt-consumer-check"
              {
                nativeBuildInputs = [ pkgs.pkg-config ];
                buildInputs = [
                  nxt
                  pkgs.boost
                ];
              }
              ''
                $CXX -std=c++23 ${./nix/consumer.cpp} $(pkg-config --cflags --libs nxt) -o consumer
                ./consumer
                touch $out
              '';
        }
      );

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
