# Runtime-only Windows/UWP package. The SDK's WindowsApp umbrella import
# library enforces the app API partition (including IOCP and Winsock).
iocp_args = nxt_wand_args + ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN']
iocp_platform = declare_dependency(link_args :
  cpp.get_argument_syntax() == 'msvc' ? ['WindowsApp.lib'] : ['-lwindowsapp'])
iocp_inc = include_directories('../..')
iocp_lib = static_library('nxtrt-iocp',
  ['iocp.cpp', '../format.cpp'],
  # Clang's MSVC-ABI driver resolves -lnxtrt-iocp to nxtrt-iocp.lib.
  name_prefix : '',
  name_suffix : 'lib',
  include_directories : iocp_inc,
  cpp_args : iocp_args,
  dependencies : iocp_platform,
  install : true,
)
iocp_dep = declare_dependency(
  link_with : iocp_lib,
  include_directories : iocp_inc,
  compile_args : iocp_args,
  dependencies : iocp_platform,
)
if get_option('tests')
  iocp_tests = executable('iocp-tests', '../../../test/iocp-wand-test.cpp',
    dependencies : iocp_dep,
    install : true,
  )
  test('iocp', iocp_tests, timeout : 30)
endif
install_headers(
  '../task.hpp', '../deck.hpp', '../wand.hpp', '../wish.hpp', '../wish_ops.hpp',
  '../env.hpp', '../ids.hpp', '../debug.hpp', '../trace.hpp', '../format.hpp',
  '../exceptions.hpp', '../exec_lifecycle.hpp', '../land.hpp', '../alloc_trace.hpp',
  '../arch.hpp',
  subdir : 'nxtrt',
)
install_subdir('../task', install_dir : get_option('includedir') / 'nxtrt')
install_headers('iocp.hpp', subdir : 'nxtrt/wand')
import('pkgconfig').generate(iocp_lib,
  name : 'nxtrt-iocp',
  description : 'C++23 coroutine runtime with Windows/UWP IOCP',
  extra_cflags : iocp_args,
)
