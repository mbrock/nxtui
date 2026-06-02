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

nxt_libvterm = library('vterm',
  nxt_libvterm_sources,
  include_directories : include_directories(
    '../vendor/libvterm/include',
    '../vendor/libvterm/src',
  ),
  override_options : ['warning_level=0'],
)

nxt_libvterm_dep = declare_dependency(
  link_with : nxt_libvterm,
  include_directories : include_directories('../vendor/libvterm/include'),
)

nxt_core_deps = [
  utf8proc_dep,
  nxt_libvterm_dep,
] + nxt_compression_deps

nxt_core_sources = files(
  'nxtrt/buffers.cpp',
  'nxtrt/compression.cpp',
  'nxtrt/crypto.cpp',
  'nxtui/ansi.cpp',
  'nxtui/compositor.cpp',
  'nxt/crypto.cpp',
  'nxt/crypto/aes128gcm.cpp',
  'nxt/crypto/sha1.cpp',
  'nxt/crypto/sha256.cpp',
  'nxt/crypto/x25519.cpp',
  'nxtui/glyph-table.cpp',
  'nxtui/input.cpp',
  'nxtui/raster.cpp',
)

nxt_core_lib = library('nxt-core',
  nxt_core_sources,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  cpp_args : nxt_mdspan_args + nxt_compression_args,
  install : true,
)

nxt_core_dep = declare_dependency(
  link_with : nxt_core_lib,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  compile_args : nxt_mdspan_args + nxt_compression_args,
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
)

nxt_stacktrace_dep = declare_dependency(
  link_with : nxt_stacktrace_lib,
  dependencies : nxt_stacktrace_deps,
  compile_args : nxt_stacktrace_cpp_args,
)

if get_option('llm_tool')
  nxtllm = executable('nxtllm',
    'nxtai/nxtllm.cpp',
    include_directories : nxt_inc,
    dependencies : [
      nxt_core_dep,
      nxt_stacktrace_dep,
    ] + nxt_dns_deps,
    cpp_args : nxt_dns_args,
  )

  nxtmt = executable('nxtmt',
    'nxtai/nxtmt.cpp',
    include_directories : nxt_inc,
    dependencies : [
      nxt_core_dep,
      nxt_stacktrace_dep,
    ] + nxt_dns_deps,
    cpp_args : nxt_dns_args,
  )
endif
