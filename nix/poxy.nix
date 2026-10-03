{
  lib,
  python3Packages,
  fetchPypi,
  doxygen,
}:

let
  inherit (python3Packages) buildPythonPackage setuptools;
  misk = buildPythonPackage rec {
    pname = "misk";
    version = "0.8.1";
    pyproject = true;
    src = fetchPypi {
      inherit pname version;
      hash = "sha256-HarD1feWtYF2IypP+3rbmHGoGFdvVYnJLS+wC6rrpBo=";
    };
    build-system = [ setuptools ];
    # The sdist omits setup.py's inputs; recover dependencies from its metadata.
    postPatch = ''
      touch CHANGELOG.md
      cp misk.egg-info/requires.txt requirements.txt
    '';
    dependencies = [ python3Packages.requests ];
    pythonImportsCheck = [ "misk" ];
    meta.license = lib.licenses.mit;
  };
  trieregex = buildPythonPackage rec {
    pname = "trieregex";
    version = "1.0.0";
    pyproject = true;
    src = fetchPypi {
      inherit pname version;
      hash = "sha256-o03THQSqFp4ZiZcaMV/L1SQSYzDH8vnxaZGwqMkITq8=";
    };
    build-system = [ setuptools ];
    nativeCheckInputs = [ python3Packages.pytestCheckHook ];
    # Upstream's memoizer is global and keys instances by repr (an address).
    # Reset it between tests so recycled addresses cannot reuse another test's cache.
    preCheck = ''
      cat > tests/conftest.py <<'PY'
      import pytest
      from trieregex import TrieRegEx

      @pytest.fixture(autouse=True)
      def clear_memoizers():
          TrieRegEx.add.clear_cache()
          TrieRegEx.regex.clear_cache()
      PY
    '';
    pythonImportsCheck = [ "trieregex" ];
    meta.license = lib.licenses.mit;
  };
in
python3Packages.buildPythonApplication rec {
  pname = "poxy";
  version = "0.27.1";
  pyproject = true;
  src = fetchPypi {
    inherit pname version;
    hash = "sha256-zttPTTKT8SgP7rGRnc/FnRIk9qdjDG6B9EBQPOFpLx0=";
  };
  build-system = [ setuptools ];
  dependencies = with python3Packages; [
    misk
    trieregex
    beautifulsoup4
    jinja2
    pygments
    html5lib
    lxml
    schema
    requests
    colorama
  ];
  # The PyPI archive omits the upstream test helpers and snapshot fixtures.
  pythonImportsCheck = [ "poxy" ];
  makeWrapperArgs = [ "--prefix PATH : ${lib.makeBinPath [ doxygen ]}" ];
  postInstall = ''
    # Poxy's bundled m.css theme invokes these scripts directly.
    patchShebangs "$out/${python3Packages.python.sitePackages}/poxy"
  '';
  meta = {
    description = "C++ documentation generator with a bundled Doxygen runtime";
    homepage = "https://github.com/marzer/poxy";
    license = lib.licenses.mit;
    mainProgram = "poxy";
  };
}
