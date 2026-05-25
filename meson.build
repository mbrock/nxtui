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

nxt_legacy_runtime_enabled = get_option('legacy_runtime')

if nxt_legacy_runtime_enabled
  nxt_libcoro_options = cmake.subproject_options()
  nxt_libcoro_options.add_cmake_defines({
    'LIBCORO_BUILD_TESTS': false,
    'LIBCORO_BUILD_EXAMPLES': false,
    'LIBCORO_EXTERNAL_DEPENDENCIES': false,
    'LIBCORO_FEATURE_NETWORKING': true,
    'LIBCORO_FEATURE_TLS': true,
  })

  nxt_libcoro_project = cmake.subproject(
    'libcoro',
    options : nxt_libcoro_options,
  )
  nxt_libcoro_dep = declare_dependency(
    dependencies : [
      nxt_libcoro_project.dependency('libcoro'),
      nxt_libcoro_project.dependency('c_ares'),
    ],
  )
endif

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

glaze_dep = declare_dependency(
  include_directories : include_directories('vendor/glaze/include'),
)
utf8proc_dep = dependency(
  'libutf8proc',
  fallback : ['utf8proc', 'utf8proc_dep'],
  default_options : ['tests=disabled'],
)

if nxt_legacy_runtime_enabled
  nanoarrow_ipc_dep = dependency(
    'nanoarrow-ipc',
    fallback : ['nanoarrow', 'nanoarrow_ipc_dep'],
    default_options : [
      'ipc=enabled',
      'apps=disabled',
      'benchmarks=disabled',
      'device=disabled',
      'testing=disabled',
      'tests=disabled',
    ],
  )
  arrow_dataset_deps = [
    dependency('arrow', required : false),
    dependency('arrow-dataset', required : false),
    dependency('parquet', required : false),
  ]
  arrow_dataset_available = true
  foreach dep : arrow_dataset_deps
    arrow_dataset_available = arrow_dataset_available and dep.found()
  endforeach
endif

if nxt_legacy_runtime_enabled
  nxt_cairo_dep = dependency('cairo', required : false)
  nxt_pangocairo_dep = dependency('pangocairo', required : false)
  nxt_fontconfig_dep = dependency('fontconfig', required : false)
  nxt_png_available = nxt_cairo_dep.found() \
    and nxt_pangocairo_dep.found() \
    and nxt_fontconfig_dep.found()
else
  nxt_png_available = false
endif

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
  'vendor/mdspan/include',
)

nxt_ng_inc = include_directories(
  'src-ng',
)

nxt_core_deps = []
nxt_core_deps += nxt_awslc_dep
nxt_core_deps += utf8proc_dep
nxt_core_deps += nxt_libvterm_dep

nxt_core_sources = files(
  'src/nxt/ansi.cpp',
  'src/nxt/compositor.cpp',
  'src/nxt/crypto.cpp',
  'src/nxt/glyph-table.cpp',
  'src/nxt/input.cpp',
  'src/nxt/raster.cpp',
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

if nxt_png_available
  nxt_png_lib = static_library('nxt-png',
    'src/nxt/png.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_core_dep, nxt_cairo_dep, nxt_pangocairo_dep, nxt_fontconfig_dep],
    cpp_args : nxt_mdspan_args,
    install : true,
  )

  nxt_png_dep = declare_dependency(
    link_with : nxt_png_lib,
    include_directories : nxt_inc,
    dependencies : [nxt_core_dep, nxt_cairo_dep, nxt_pangocairo_dep, nxt_fontconfig_dep],
    compile_args : nxt_mdspan_args,
  )

endif

if nxt_legacy_runtime_enabled
  nxt_runtime_deps = [
    nxt_core_dep,
    nxt_libcoro_dep,
    glaze_dep,
  ]

  nxt_runtime_cpp_args = []

  nxt_runtime_deps += nxt_stacktrace_deps
  nxt_runtime_cpp_args += nxt_stacktrace_cpp_args

  if nxt_png_available
    nxt_runtime_deps += nxt_png_dep
    nxt_runtime_cpp_args += '-DNXT_HAVE_PNG=1'
  endif

  nxt_runtime_sources = files(
    'src/nxtio/app.cpp',
    'src/nxtio/arrow.cpp',
    'src/nxtio/http.cpp',
    'src/nxtio/net.cpp',
    'src/nxtio/short_id.cpp',
    'src/nxtio/signal-pipe.cpp',
    'src/nxtio/stacktrace.cpp',
    'src/nxtio/subprocess.cpp',
  )

  nxt_runtime_deps += nanoarrow_ipc_dep

  nxt_lib = static_library('nxtui',
    nxt_runtime_sources,
    include_directories : nxt_inc,
    dependencies : nxt_runtime_deps,
    cpp_args : nxt_runtime_cpp_args,
    install : true,
  )

  nxt_dep = declare_dependency(
    link_with : nxt_lib,
    include_directories : nxt_inc,
    dependencies : nxt_runtime_deps,
    compile_args : nxt_runtime_cpp_args,
  )
endif

if get_option('demo')
  subdir('demo')
endif

if get_option('llm_tool') and nxt_legacy_runtime_enabled
  executable('nxtllm',
    'src/nxtai/nxtllm.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
    cpp_args : ['-Wno-unused-parameter'],
  )
elif get_option('llm_tool')
  message('legacy runtime disabled; skipping nxtllm')
endif

if get_option('tests')
  nxt_test_inc = include_directories('test')

  if nxt_legacy_runtime_enabled
    nxt_tests = executable('nxt-tests',
      files(
        'test/arrow-test.cpp',
        'test/buffers-test.cpp',
        'test/crypto-test.cpp',
        'test/http-io-test.cpp',
        'test/http-test.cpp',
        'test/input-test.cpp',
        'test/llm-test.cpp',
        'test/net-include-test.cpp',
        'test/raster-test.cpp',
        'test/subprocess-test.cpp',
        'test/terminal-test.cpp',
        'test/test-main.cpp',
        'test/text-field-test.cpp',
      ),
      include_directories : [nxt_inc, nxt_test_inc],
      dependencies : [nxt_dep],
    )

    test('nxt-tests', nxt_tests)
  endif

  nxt_ng_test_sources = files(
    'test/ng-include-test.cpp',
    'test/ng-runtime-test.cpp',
    'src/nxtio/stacktrace.cpp',
    'test/test-main.cpp',
  )
  nxt_ng_test_deps = []
  nxt_ng_test_deps += nxt_stacktrace_deps

  if host_machine.system() == 'linux'
    nxt_test_liburing_dep = dependency(
      'liburing',
      fallback : ['liburing', 'uring'],
      required : false,
    )

    if nxt_test_liburing_dep.found()
      nxt_ng_test_sources += files('test/ng-uring-wand-test.cpp')
      nxt_ng_test_deps += nxt_test_liburing_dep

      nxt_test_cares_dep = dependency('libcares', required : false)
      if nxt_test_cares_dep.found()
        nxt_ng_test_sources += files('test/ng-cares-test.cpp')
        nxt_ng_test_deps += nxt_test_cares_dep
      endif
    endif
  endif

  nxt_ng_tests = executable('ng-tests',
    nxt_ng_test_sources,
    include_directories : [nxt_inc, nxt_ng_inc, nxt_test_inc],
    dependencies : nxt_ng_test_deps,
    cpp_args : nxt_stacktrace_cpp_args,
  )
  test('ng-tests', nxt_ng_tests)
endif
