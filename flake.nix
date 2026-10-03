{
  description = "nxt: C++23 coroutine runtime, terminal toolkit, and LLM tooling";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  inputs.filnix.url = "github:mbrock/filnix";

  outputs =
    {
      self,
      nixpkgs,
      filnix,
    }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (
        pkgs:
        rec {
          nxt = pkgs.callPackage ./nix/package.nix { };
          spec-racket = pkgs.callPackage ./nix/spec-racket.nix { };
          spec-sources = pkgs.callPackage ./nix/spec-sources.nix { };
          poxy = pkgs.callPackage ./nix/poxy.nix { };
          default = nxt;
        }
        // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
          nxt-filc =
            let
              filcPkgs = filnix.legacyPackages.${pkgs.stdenv.hostPlatform.system}.pkgsFilc;
            in
            filcPkgs.callPackage ./nix/package.nix {
              cryptoLibrary = filcPkgs.openssl;
            };
        }
      );

      devShells = forAllSystems (
        pkgs:
        let
          inherit (pkgs) lib stdenv;
          llvm = pkgs.llvmPackages_23;
          clangStdenv = llvm.stdenv;
          gccStdenv = pkgs.gcc16Stdenv;
          certificateShellHook = ''
            # libcrypto reads SSL_CERT_FILE, not NIX_SSL_CERT_FILE.
            # Preserve custom trust bundles, with a portable store fallback.
            export SSL_CERT_FILE="''${SSL_CERT_FILE:-''${NIX_SSL_CERT_FILE:-${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt}}"
          '';
          mkDevShell =
            toolchainStdenv:
            (pkgs.mkShell.override { stdenv = toolchainStdenv; }) {
              inputsFrom = [
                (self.packages.${stdenv.hostPlatform.system}.nxt.override {
                  stdenv = toolchainStdenv;
                })
              ];

              packages =
                with pkgs;
                [
                  aws-lc
                  llvm.clang-tools
                  nixd
                  gnumake
                  self.packages.${stdenv.hostPlatform.system}.spec-racket

                  # Trace-analysis scripts use uv; docs use the Nix package.
                  uv
                  self.packages.${stdenv.hostPlatform.system}.poxy
                ]
                ++ lib.optionals stdenv.hostPlatform.isLinux [
                  mold
                  gdb
                ];

              shellHook = certificateShellHook + ''
                # Only editable model/DSL bytecode is local. Dependencies and
                # their compiled code come from the immutable Nix package.
                export PLTCOLLECTS="$PWD:"
                # The empty suffix preserves Racket's installed bytecode roots.
                export PLTCOMPILEDROOTS="$PWD/.racket/$(racket -e '(display (version))')/compiled:"
              '';
            };
        in
        rec {
          default = clang;
          clang = mkDevShell clangStdenv;
          # Publishing docs does not need the C++ or Racket toolchains.
          docs = pkgs.mkShellNoCC {
            shellHook = certificateShellHook;
            packages = [
              pkgs.gnumake
              self.packages.${stdenv.hostPlatform.system}.poxy
            ];
          };
          # Keep GCC's libstdc++ separate from Clang's toolchain.
          gcc = mkDevShell gccStdenv;
        }
        // lib.optionalAttrs stdenv.hostPlatform.isLinux {
          # Fil-C libraries must come from the same ABI as its compiler.
          filc =
            let
              filcPkgs = filnix.legacyPackages.${stdenv.hostPlatform.system}.pkgsFilc;
            in
            filcPkgs.mkShell {
              shellHook = certificateShellHook;
              inputsFrom = [ self.packages.${stdenv.hostPlatform.system}.nxt-filc ];
              nativeBuildInputs = [ pkgs.gnumake ];
              # Fil-C supplies its own linker; do not pick up host mold.
              NXT_MESON_LINK_ARGS = "";
            };
        }
      );

      checks = forAllSystems (
        pkgs:
        let
          inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) nxt spec-racket;
          specSource = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [
              ./Makefile
              ./nxtrt
              ./rdf-forge
            ];
          };
        in
        {
          inherit nxt;

          spec =
            pkgs.runCommand "nxt-spec-check"
              {
                nativeBuildInputs = [
                  spec-racket
                  pkgs.gnumake
                ];
              }
              ''
                export HOME="$TMPDIR/home"
                mkdir -p "$HOME"
                cp -R ${specSource} source
                chmod -R u+w source
                cd source
                make spec
                touch "$out"
              '';

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
