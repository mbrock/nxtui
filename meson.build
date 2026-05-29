project('nxtui', 'cpp', 'c',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=2',
    'buildtype=debug',
  ],
  meson_version : '>= 1.1',
)

cmake = import('cmake')

nxt_awslc_options = cmake.subproject_options()
nxt_awslc_options.add_cmake_defines({
  'BUILD_LIBSSL': false,
  'BUILD_TESTING': false,
  'BUILD_TOOL': false,
  'DISABLE_GO': true,
  'DISABLE_PERL': true,
})

nxt_awslc_project = cmake.subproject(
  'aws-lc',
  options : nxt_awslc_options,
)
nxt_awslc_dep = nxt_awslc_project.dependency('crypto')

utf8proc_dep = dependency(
  'libutf8proc',
  fallback : ['utf8proc', 'utf8proc_dep'],
  default_options : ['tests=disabled'],
)

nxt_mdspan_args = [
  '-DMDSPAN_USE_BRACKET_OPERATOR=1',
  '-DMDSPAN_USE_PAREN_OPERATOR=0',
]

nxt_libvterm_sources = files(
  'vendor/libvterm/src/encoding.c',
  'vendor/libvterm/src/keyboard.c',
  'vendor/libvterm/src/mouse.c',
  'vendor/libvterm/src/parser.c',
  'vendor/libvterm/src/pen.c',
  'vendor/libvterm/src/screen.c',
  'vendor/libvterm/src/state.c',
  'vendor/libvterm/src/unicode.c',
  'vendor/libvterm/src/vterm.c',
)

nxt_libvterm = static_library('vterm',
  nxt_libvterm_sources,
  include_directories : include_directories(
    'vendor/libvterm/include',
    'vendor/libvterm/src'
  ),
  c_args : ['-I' + meson.current_build_dir()],
  override_options : ['warning_level=0'],
)

nxt_libvterm_dep = declare_dependency(
  link_with : nxt_libvterm,
  include_directories : include_directories('vendor/libvterm/include'),
)

nxt_inc = include_directories(
  'src',
  'vendor/hub/include',
  'vendor/mdspan/include',
)
nxt_core_deps = []
nxt_core_deps += nxt_awslc_dep
nxt_core_deps += utf8proc_dep
nxt_core_deps += nxt_libvterm_dep

nxt_core_sources = files(
  'src/nxtui/ansi.cpp',
  'src/nxtui/compositor.cpp',
  'src/nxt/crypto.cpp',
  'src/nxtui/glyph-table.cpp',
  'src/nxtui/input.cpp',
  'src/nxtui/raster.cpp',
)

nxt_core_lib = static_library('nxt-core',
  nxt_core_sources,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  cpp_args : nxt_mdspan_args,
  install : true,
)

nxt_core_dep = declare_dependency(
  link_with : nxt_core_lib,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  compile_args : nxt_mdspan_args,
)

nxt_stacktrace_deps = []
nxt_stacktrace_cpp_args = []

cpp = meson.get_compiler('cpp')

cpptrace_dep = dependency('cpptrace', method : 'cmake', required : false)
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

if get_option('demo')
  subdir('demo')
endif

if get_option('llm_tool')
  if host_machine.system() == 'linux'
    nxt_llm_liburing_dep = dependency(
      'liburing',
      fallback : ['liburing', 'uring'],
      required : false,
    )
  else
    nxt_llm_liburing_dep = disabler()
  endif

  nxtllm_dependencies = [nxt_core_dep]
  nxtllm_dependencies += nxt_stacktrace_deps
  if host_machine.system() == 'linux' and nxt_llm_liburing_dep.found()
    nxtllm_dependencies += nxt_llm_liburing_dep
  elif host_machine.system() == 'linux'
    error('nxtllm requires liburing on Linux')
  endif

  nxt_llm_cares_dep = dependency('libcares', required : true)
  nxtllm_dependencies += nxt_llm_cares_dep

  executable('nxtllm',
    'src/nxtai/nxtllm.cpp',
    'src/nxtai/trace_tui.cpp',
    'src/nxtai/tool_tui.cpp',
    'src/nxtai/agent.cpp',
    'src/nxtai/stream.cpp',
    'src/nxtai/tool_ui.cpp',
    include_directories : nxt_inc,
    dependencies : nxtllm_dependencies,
    cpp_args : nxt_stacktrace_cpp_args,
    cpp_pch : 'src/nxtai/nxtllm_pch.hpp',
  )
endif

if get_option('benchmarks')
  nxt_bench_deps = [nxt_core_dep]
  nxt_bench_deps += nxt_stacktrace_deps
  nxt_bench_cpp_args = []
  nxt_bench_cpp_args += nxt_stacktrace_cpp_args
  nxt_bench_cpp_args += [
    '-DNXT_RT_ENABLE_TRACE=0',
    '-fno-omit-frame-pointer',
  ]

  if host_machine.system() == 'linux'
    nxt_bench_liburing_dep = dependency(
      'liburing',
      fallback : ['liburing', 'uring'],
      required : false,
    )

    if nxt_bench_liburing_dep.found()
      nxt_bench_deps += nxt_bench_liburing_dep
    else
      error('benchmarks require liburing on Linux')
    endif
  endif

  executable('nxt-echo-bench',
    'bench/nxtrt-echo-bench.cpp',
    include_directories : nxt_inc,
    dependencies : nxt_bench_deps,
    cpp_args : nxt_bench_cpp_args,
  )
endif

if get_option('tests')
  nxt_test_inc = include_directories('test')

  nxt_test_sources = files(
	    'test/input-test.cpp',
	    'test/json-test.cpp',
	    'test/include-test.cpp',
    'test/runtime-test.cpp',
    'test/terminal-test.cpp',
    'src/nxt/stacktrace.cpp',
    'test/test-main.cpp',
  )
  nxt_test_deps = []
  nxt_test_deps += nxt_core_dep
  nxt_test_deps += utf8proc_dep
  nxt_test_deps += nxt_stacktrace_deps

  if host_machine.system() == 'linux'
    nxt_test_liburing_dep = dependency(
      'liburing',
      fallback : ['liburing', 'uring'],
      required : false,
    )

    if nxt_test_liburing_dep.found()
      nxt_test_sources += files('test/uring-wand-test.cpp')
      nxt_test_deps += nxt_test_liburing_dep

      nxt_test_cares_dep = dependency('libcares', required : false)
      if nxt_test_cares_dep.found()
        nxt_test_sources += files('test/cares-test.cpp')
        nxt_test_deps += nxt_test_cares_dep
      endif
    endif
  elif host_machine.system() in ['darwin', 'dragonfly', 'freebsd', 'netbsd', 'openbsd']
    nxt_test_sources += files('test/kqueue-wand-test.cpp')
  endif

  nxt_tests = executable('nxt-tests',
    nxt_test_sources,
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : nxt_test_deps,
    cpp_args : nxt_stacktrace_cpp_args,
  )
  test('nxt-tests', nxt_tests)
endif
