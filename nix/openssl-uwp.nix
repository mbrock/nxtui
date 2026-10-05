{
  lib,
  stdenv,
  openssl,
  perl,
}:
stdenv.mkDerivation {
  pname = "openssl-uwp";
  inherit (openssl) version src;
  patches = [ ./patches/openssl-uwp-environment.patch ];
  nativeBuildInputs = [ perl ];
  configurePhase = ''
    runHook preConfigure
    cp ${./openssl-uwp.conf} Configurations/99-nxt-uwp.conf
    perl Configure nxt-uwp --prefix="$out" --libdir=lib \
      CC="$CC" AR="$AR" RANLIB="$RANLIB"
    runHook postConfigure
  '';
  buildPhase = ''
    runHook preBuild
    make -j"$NIX_BUILD_CORES" build_generated
    make -j"$NIX_BUILD_CORES" libcrypto.a
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p "$out/lib/pkgconfig" "$out/include"
    cp libcrypto.a "$out/lib/crypto.lib"
    # FindOpenSSL's non-MSVC Windows search uses libcrypto, whereas our
    # MSVC-ABI pkg-config consumers use -lcrypto. Keep both names valid.
    ln -s crypto.lib "$out/lib/libcrypto.lib"
    cp -r include/openssl "$out/include/"
    cat > "$out/lib/pkgconfig/libcrypto.pc" <<EOF
    prefix=$out
    libdir=$out/lib
    includedir=$out/include
    Name: libcrypto
    Description: Static MSVC-ABI UWP OpenSSL certificate verifier
    Version: ${openssl.version}
    Libs: -L$out/lib -lcrypto -lwindowsapp
    Cflags: -I$out/include
    EOF
    runHook postInstall
  '';
  meta = {
    description = "OpenSSL libcrypto for UWP with explicit application trust";
    license = lib.licenses.asl20;
    platforms = lib.platforms.windows;
  };
}
