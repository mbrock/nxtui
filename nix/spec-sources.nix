# Upstream sources and the small integration patches used by the spec backend.
{ stdenvNoCC, fetchzip }:
let
  forge = fetchzip {
    url = "https://github.com/tnelson/Forge/archive/2f80c9e64fcd13277b25887b2138dfefb4e5c18e.tar.gz"; # v5.2
    hash = "sha256-6jHFBGyl/c+tRb36GQj9Kvt/JdaHmc96qQOZ79V7pM0=";
  };
  something = fetchzip {
    url = "https://git.leastfixedpoint.com/tonyg/racket-something/archive/f6116bf3861b76970f5ce291a628476adef820b4.tar.gz";
    hash = "sha256-8dDICwNR2UrK1zTaaQUOHGMYxi0afND+ob4I1DlF6BU=";
  };
in
stdenvNoCC.mkDerivation {
  pname = "nxt-spec-sources";
  version = "1";
  dontUnpack = true;
  dontConfigure = true;
  dontBuild = true;
  # This is a source output, including upstream's prebuilt solver libraries.
  dontFixup = true;
  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp -R ${forge}/forge "$out/forge"
    cp -R ${something}/src "$out/something"
    chmod -R u+w "$out"
    for patchFile in ${./patches/forge-xml-export.patch} \
      ${./patches/forge-optional-git.patch} ${./patches/forge-spec-backend.patch}; do
      patch --directory="$out/forge" -p1 < "$patchFile"
    done
    patch --directory="$out/something" -p1 < ${./patches/something-spec-reader.patch}
    runHook postInstall
  '';
}
