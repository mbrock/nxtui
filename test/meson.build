if get_option('tests')
  nxt_test_sources = files(
    'input-test.cpp',
    'json-test.cpp',
    'crypto-test.cpp',
    'include-test.cpp',
    'http-server-test.cpp',
    'runtime-test.cpp',
    'terminal-test.cpp',
    'mtproto-test.cpp',
    'wisp-test.cpp',
    'wisp-eval-test.cpp',
    'wisp-reader-test.cpp',
    'wisp-printer-test.cpp',
    'wisp-primitives-test.cpp',
    'wisp-load-test.cpp',
    'wisp-runtime-test.cpp',
    'wisp-tape-test.cpp',
    'test-main.cpp',
  )
  nxt_test_deps = [
    nxt_core_dep,
    nxt_stacktrace_dep,
  ]
  nxt_test_extra_deps = []
  nxt_test_cpp_args = []

  nxt_crypto_dep = dependency('libcrypto', required : false)
  nxt_test_libcrypto_probe = '''
    #include <openssl/ec.h>
    #include <openssl/ec_key.h>
    #include <openssl/ecdsa.h>
    #include <openssl/evp.h>
    #include <openssl/nid.h>
    #include <openssl/rsa.h>
    int main() {
      return EVP_PKEY_RSA != 0 && NID_X9_62_prime256v1 != 0;
    }
  '''
  if nxt_crypto_dep.found() and cpp.compiles(
    nxt_test_libcrypto_probe,
    dependencies : [nxt_crypto_dep],
    name : 'libcrypto test fixture API')
    nxt_test_deps += nxt_crypto_dep
    nxt_test_extra_deps += nxt_crypto_dep
    nxt_test_cpp_args += '-DNXT_HAVE_TEST_LIBCRYPTO=1'
  endif

  nxt_mlkem_probe = '''
    #include <openssl/evp.h>
    #include <openssl/nid.h>
    int main() {
      EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_KEM, nullptr);
      return ctx != nullptr && NID_MLKEM768 != 0
        && EVP_PKEY_CTX_kem_set_params(ctx, NID_MLKEM768);
    }
  '''
  if nxt_crypto_dep.found() and cpp.compiles(
    nxt_mlkem_probe,
    dependencies : [nxt_crypto_dep],
    name : 'libcrypto ML-KEM-768 EVP API')
    message('ML-KEM-768 test support available via system libcrypto')
    nxt_test_sources += files('../src/nxt/crypto/mlkem_libcrypto.cpp')
    nxt_test_deps += nxt_crypto_dep
    nxt_test_extra_deps += nxt_crypto_dep
    nxt_test_cpp_args += '-DNXT_HAVE_MLKEM768=1'
  else
    message('ML-KEM-768 test support not available')
  endif

  if host_machine.system() == 'linux'
    nxt_test_sources += files(
      'epoll-wand-test.cpp',
      'uring-wand-test.cpp',
      'cares-test.cpp',
    )
    nxt_test_deps += nxt_dns_deps
    nxt_test_extra_deps += nxt_dns_deps
    nxt_test_cpp_args += nxt_dns_args
  elif host_machine.system() in ['darwin', 'dragonfly', 'freebsd', 'netbsd', 'openbsd']
    nxt_test_sources += files('kqueue-wand-test.cpp')
  endif

  nxt_tests = executable('nxt-tests',
    nxt_test_sources,
    include_directories : [nxt_inc, include_directories('.')],
    dependencies : nxt_test_deps,
    cpp_args : nxt_test_cpp_args,
  )
  # Includes full Lisp bootstraps and >4096 HTTP request/connection stress
  # cases, run during discovery and execution. Each leaf keeps its deadline.
  test('nxt-tests', nxt_tests, timeout : 180)

  # Fault injection replaces operator new only in this executable. Compile
  # the guest heap directly rather than linking nxt-core's allocation hooks.
  wisp_alloc_tests = executable('wisp-alloc-tests',
    'wisp-alloc-test.cpp', 'test-main.cpp', '../src/wisp/heap.cpp',
    include_directories : [nxt_inc, include_directories('.')],
    dependencies : [nxt_stacktrace_dep],
  )
  test('wisp-alloc-tests', wisp_alloc_tests)

  if get_option('wisp_tool')
    # Many independent interpreter bootstraps plus the 4100-job stress
    # case (90s process deadline); keep each process's own timeout active.
    test('wisp-host-tests', find_program('python3'),
      args : [files('wisp-host-test.py'), wisp_tool],
      timeout : 240,
    )
    test('wisp-http-tests', find_program('python3'),
      args : [files('wisp-http-test.py'), wisp_tool],
      timeout : 120,
    )
  endif
endif
