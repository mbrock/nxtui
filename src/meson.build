nxt_libvterm_sources = files(
  '../vendor/libvterm/src/encoding.c',
  '../vendor/libvterm/src/keyboard.c',
  '../vendor/libvterm/src/mouse.c',
  '../vendor/libvterm/src/parser.c',
  '../vendor/libvterm/src/pen.c',
  '../vendor/libvterm/src/screen.c',
  '../vendor/libvterm/src/state.c',
  '../vendor/libvterm/src/unicode.c',
  '../vendor/libvterm/src/vterm.c',
)

# libvterm is linked whole into nxt-core, so installs carry no separate
# libvterm that could clash with a system copy.
nxt_libvterm = static_library('nxt-vterm',
  nxt_libvterm_sources,
  include_directories : include_directories(
    '../vendor/libvterm/include',
    '../vendor/libvterm/src',
  ),
  override_options : ['warning_level=0'],
)

nxt_libvterm_inc = include_directories('../vendor/libvterm/include', is_system : true)

nxt_libvterm_dep = declare_dependency(
  link_with : nxt_libvterm,
  include_directories : nxt_libvterm_inc,
)

nxt_wisp_profile_args = ['-DNXT_WISP_PROFILE=' + (get_option('wisp_profile') ? '1' : '0')]
nxt_threads_dep = dependency('threads')
nxt_core_deps = [
  utf8proc_dep,
  boost_dep,
  dependency('libcrypto'),
  nxt_threads_dep,
  declare_dependency(include_directories : nxt_libvterm_inc),
  declare_dependency(compile_args : nxt_wand_args),
  declare_dependency(compile_args : nxt_wisp_profile_args),
] + nxt_compression_deps

# Wisp sources and the boot tape use #embed as a C++23 extension.
if not cpp.compiles('''
  constexpr unsigned char data[] = {
  #embed __FILE__ limit(1)
  };
  static_assert(sizeof(data) == 1);
''', name : '#embed in C++23')
  error('Wisp requires #embed support (GCC 15+ or Clang 19+)')
endif

# Related sources compile through unity translation units, sharing header
# parsing and template instantiation. The HTTP server stays on its own so
# it builds in parallel with the rest.
nxt_core_sources = files(
  'nxt-core-rt.cpp',
  'nxt-core-ui.cpp',
  'nxt-core-wisp.cpp',
  'nxtrt/http-server.cpp',
)

nxt_stacktrace_deps = []
nxt_stacktrace_cpp_args = []

cpptrace_dep = dependency(
  'cpptrace',
  method : 'cmake',
  required : get_option('cpptrace'),
)
cpptrace_probe = '''
  #include <cpptrace/cpptrace.hpp>
  #include <cpptrace/formatting.hpp>
  #include <sstream>
  int main() {
    std::ostringstream out;
    cpptrace::formatter{}.print(out, cpptrace::generate_trace(), false);
    return 0;
  }
'''
if cpptrace_dep.found() and cpp.links(
  cpptrace_probe,
  dependencies : [cpptrace_dep],
  name : 'cpptrace')
  nxt_stacktrace_deps += cpptrace_dep
  nxt_stacktrace_cpp_args += '-DNXT_HAVE_CPPTRACE=1'
  message('cpptrace available')
else
  message('cpptrace not available')
endif

std_stacktrace_probe = '''
  #include <stacktrace>
  int main() {
    auto trace = std::stacktrace::current();
    return static_cast<int>(trace.size());
  }
'''
if cpp.links(std_stacktrace_probe, name : 'std::stacktrace')
  message('std::stacktrace available')
else
  stdcxxexp_dep = cpp.find_library('stdc++exp', required : false)
  if stdcxxexp_dep.found() and cpp.links(
    std_stacktrace_probe,
    dependencies : [stdcxxexp_dep],
    name : 'std::stacktrace with stdc++exp')
    nxt_stacktrace_deps += stdcxxexp_dep
    message('std::stacktrace available with stdc++exp')
  else
    message('std::stacktrace not available')
  endif
endif

nxt_stacktrace_lib = library('nxt-stacktrace',
  'nxt/stacktrace.cpp',
  include_directories : nxt_inc,
  dependencies : nxt_stacktrace_deps,
  cpp_args : nxt_stacktrace_cpp_args,
  install : true,
)

nxt_stacktrace_dep = declare_dependency(
  link_with : nxt_stacktrace_lib,
  dependencies : nxt_stacktrace_deps,
  compile_args : nxt_stacktrace_cpp_args,
)

# Exception classes and inline task code depend on NXT_HAVE_CPPTRACE.
# Compile the library and every consumer with the same public ABI.
nxt_core_deps += nxt_stacktrace_dep
nxt_core_lib = library('nxt-core',
  nxt_core_sources,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  link_whole : nxt_libvterm,
  cpp_args : nxt_mdspan_args + nxt_compression_args,
  install : true,
)

nxt_core_dep = declare_dependency(
  link_with : nxt_core_lib,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  compile_args : nxt_mdspan_args + nxt_compression_args,
)

if get_option('wisp_tool')
  wisp_boot = executable('wisp-boot', 'wisp/boot.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_core_dep],
  )
  wisp_boot_tape = custom_target('wisp-boot-tape',
    output : 'wisp-boot.tape',
    input : files('wisp/base.wisp', 'wisp/compiler.wisp', 'wisp/host.wisp'),
    command : [wisp_boot, '@OUTPUT@',
      get_option('wisp_boot_compression') ? 'zlib' : 'none'],
    # The generator uses the embedded sources and the current evaluator
    # and tape codec. Regenerate when any of those change, even if the
    # shared library's exported symbols remain the same.
    depends : [nxt_core_lib],
  )
  wisp_tool = executable('wisp', ['wisp/main.cpp', wisp_boot_tape],
    include_directories : nxt_inc,
    dependencies : [nxt_core_dep, nxt_stacktrace_dep] + nxt_dns_deps,
    # Unlike #include, GCC's #embed does not search -I directories.
    # Use a build-relative search path so the compiler depfile and Ninja agree
    # on the generated tape's build-relative identity.
    cpp_args : ['--embed-dir=' + import('fs').relative_to(
      meson.current_build_dir(), meson.global_build_root())] + nxt_dns_args,
    install : true,
  )
endif

if get_option('llm_tool')
  nxtllm = executable('nxtllm',
    'nxtai/nxtllm.cpp',
    include_directories : nxt_inc,
    dependencies : [
      nxt_core_dep,
      nxt_stacktrace_dep,
    ] + nxt_dns_deps,
    cpp_args : nxt_dns_args,
    install : true,
  )

  nxtmt = executable('nxtmt',
    'nxtai/nxtmt.cpp',
    include_directories : nxt_inc,
    dependencies : [
      nxt_core_dep,
      nxt_stacktrace_dep,
    ] + nxt_dns_deps,
    cpp_args : nxt_dns_args,
    install : true,
  )
endif

# Public headers. The vendored headers that public headers include
# (<vterm.h>, <experimental/mdspan>, <boost/container/hub.hpp>) go under
# include/nxt-vendor so they never shadow system copies.
nxt_header_install_excludes = {
  'nxt' : [
    'crypto.cpp',
    'crypto/aes128gcm.cpp',
    'crypto/mlkem_libcrypto.cpp',
    'crypto/sha1.cpp',
    'crypto/sha256.cpp',
    'crypto/x25519.cpp',
    'png.cpp',
    'stacktrace.cpp',
    'tls/cert.cpp',
  ],
  'nxtai' : [
    'nxtllm.cpp',
    'nxtmt.cpp',
  ],
  'nxtrt' : [
    'alloc_trace.cpp',
    'buffers.cpp',
    'compression.cpp',
    'crypto.cpp',
    'format.cpp',
    'wand/kqueue.cpp',
    'wand/iocp.cpp',
    'wand/meson.build',
    'http-server.cpp',
    'tls.cpp',
  ],
  'nxtui' : [
    'ansi.cpp',
    'compositor.cpp',
    'glyph-table.cpp',
    'input.cpp',
    'raster.cpp',
  ],
  'wisp' : [
    'heap.cpp',
    'eval.cpp',
    'reader.cpp',
    'printer.cpp',
    'load.cpp',
    'tape.cpp',
    'base.wisp',
    'compiler.wisp',
    'boot.cpp',
    'main.cpp',
    'host.wisp',
  ],
}

foreach dir : ['nxt', 'nxtai', 'nxtrt', 'nxtui', 'wisp']
  # Recursive installation includes implementation headers such as nxtrt/task/.
  install_subdir(dir,
    install_dir : get_option('includedir'),
    exclude_files : nxt_header_install_excludes.get(dir, []),
  )
endforeach

install_headers('nxtrt.hpp')

nxt_vendor_includedir = get_option('includedir') / 'nxt-vendor'

install_headers(
  '../vendor/libvterm/include/vterm.h',
  '../vendor/libvterm/include/vterm_keycodes.h',
  install_dir : nxt_vendor_includedir,
)
install_subdir('../vendor/mdspan/include/experimental',
  install_dir : nxt_vendor_includedir,
)
install_subdir('../vendor/hub/include/boost',
  install_dir : nxt_vendor_includedir,
)

import('pkgconfig').generate(nxt_core_lib,
  libraries : [nxt_stacktrace_lib, nxt_threads_dep] + nxt_stacktrace_deps,
  name : 'nxt',
  description : 'nxt coroutine runtime and terminal toolkit',
  subdirs : ['.', 'nxt-vendor'],
  extra_cflags : nxt_mdspan_args + nxt_compression_args + nxt_stacktrace_cpp_args + nxt_wand_args + nxt_wisp_profile_args,
)
