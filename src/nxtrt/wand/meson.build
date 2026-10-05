# Runtime-only Windows/UWP package. The SDK's WindowsApp umbrella import
# library enforces the app API partition (including IOCP and Winsock).
iocp_args = nxt_wand_args + ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN']
iocp_platform = declare_dependency(link_args :
  cpp.get_argument_syntax() == 'msvc' ? ['WindowsApp.lib'] : ['-lwindowsapp'])
iocp_dependencies = [iocp_platform, dependency('libcrypto'), dependency('zlib'), dependency('boost')]
iocp_inc = include_directories('../..')
iocp_lib = static_library('nxtrt-iocp',
  ['iocp.cpp', '../format.cpp', '../buffers.cpp', '../compression.cpp', '../crypto.cpp', '../tls.cpp',
   '../../nxt/crypto.cpp', '../../nxt/crypto/aes128gcm.cpp', '../../nxt/crypto/sha1.cpp',
   '../../nxt/crypto/sha256.cpp', '../../nxt/crypto/x25519.cpp', '../../nxt/tls/cert.cpp'],
  # Clang's MSVC-ABI driver resolves -lnxtrt-iocp to nxtrt-iocp.lib.
  name_prefix : '',
  name_suffix : 'lib',
  include_directories : iocp_inc,
  cpp_args : iocp_args,
  dependencies : iocp_dependencies,
  install : true,
)
iocp_dep = declare_dependency(
  link_with : iocp_lib,
  include_directories : iocp_inc,
  compile_args : iocp_args,
  dependencies : iocp_dependencies,
)
if get_option('tests')
  iocp_tests = executable('iocp-tests', '../../../test/iocp-wand-test.cpp',
    dependencies : iocp_dep,
    install : true,
  )
  test('iocp', iocp_tests, timeout : 30)
  network_probe = executable('network-probe', '../../../test/network-probe.cpp',
    dependencies : iocp_dep,
    install : true,
  )
  test('network-probe', network_probe, timeout : 90)
endif
install_headers(
  '../task.hpp', '../deck.hpp', '../wand.hpp', '../wish.hpp', '../wish_ops.hpp',
  '../env.hpp', '../ids.hpp', '../debug.hpp', '../trace.hpp', '../format.hpp',
  '../exceptions.hpp', '../exec_lifecycle.hpp', '../land.hpp', '../alloc_trace.hpp',
  '../arch.hpp', '../buffers.hpp', '../buffer-core.hpp', '../value-buffers.hpp',
  '../compression.hpp', '../crypto.hpp', '../tls.hpp', '../net.hpp', '../net_dns.hpp',
  '../http.hpp', '../cares.hpp', '../pool.hpp', '../farm.hpp', '../idea.hpp',
  subdir : 'nxtrt',
)
install_headers('../../nxt/crypto.hpp', '../../nxt/tls.hpp', '../../nxt/json.hpp', '../../nxt/http.hpp', subdir : 'nxt')
install_headers('../../nxt/tls/cert.hpp', subdir : 'nxt/tls')
install_headers('../../nxtai/responses_transport.hpp', '../../nxtai/responses_request.hpp',
  '../../nxtai/responses_stream.hpp', '../../nxtai/openai_types.hpp', '../../nxtai/tool_json.hpp',
  '../../nxtai/tool_batch.hpp', '../../nxtai/agent.hpp', subdir : 'nxtai')
install_subdir('../task', install_dir : get_option('includedir') / 'nxtrt')
install_headers('iocp.hpp', subdir : 'nxtrt/wand')
import('pkgconfig').generate(iocp_lib,
  name : 'nxtrt-iocp',
  description : 'C++23 Windows/UWP IOCP networking, HTTP/TLS and Responses transport',
  extra_cflags : iocp_args,
)
