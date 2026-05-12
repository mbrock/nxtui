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

nlohmann_json_dep = dependency('nlohmann_json')
utf8proc_dep = dependency(
  'libutf8proc',
  fallback : ['utf8proc', 'utf8proc_dep'],
  default_options : ['tests=disabled'],
)
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
  dependency('arrow', required : get_option('parquet_dataset_example')),
  dependency('arrow-dataset', required : get_option('parquet_dataset_example')),
  dependency('parquet', required : get_option('parquet_dataset_example')),
]
arrow_dataset_available = true
foreach dep : arrow_dataset_deps
  arrow_dataset_available = arrow_dataset_available and dep.found()
endforeach

nxt_cairo_dep = dependency('cairo', required : false)
nxt_pangocairo_dep = dependency('pangocairo', required : false)
nxt_fontconfig_dep = dependency('fontconfig', required : false)
nxt_png_available = nxt_cairo_dep.found() \
  and nxt_pangocairo_dep.found() \
  and nxt_fontconfig_dep.found()

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

nxt_core_deps = []
nxt_core_deps += utf8proc_dep
nxt_core_deps += nxt_libvterm_dep

nxt_core_sources = files(
  'src/nxt/ansi.cpp',
  'src/nxt/compositor.cpp',
  'src/nxt/glyph-table.cpp',
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

nxt_runtime_deps = [
  nxt_core_dep,
  nxt_libcoro_dep,
  nlohmann_json_dep,
]

nxt_runtime_cpp_args = []

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

  nxt_runtime_deps += nxt_png_dep
  nxt_runtime_cpp_args += '-DNXT_HAVE_PNG=1'
endif

nxt_runtime_sources = files(
  'src/nxtio/app.cpp',
  'src/nxtio/arrow.cpp',
  'src/nxtio/http.cpp',
  'src/nxtio/input.cpp',
  'src/nxtio/net.cpp',
  'src/nxtio/short_id.cpp',
  'src/nxtio/signal-pipe.cpp',
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

if get_option('examples')
  if nxt_png_available
    executable('nxtshot',
      'src/nxt/nxtshot.cpp',
      include_directories : nxt_inc,
      dependencies : [nxt_dep, nxt_png_dep],
    )
  endif

  executable('tui_example',
    'src/nxt/tui_example.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('input_hud_example',
    'src/nxt/input_hud_example.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('shell_column_example',
    'src/nxt/shell_column_example.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('slot_example',
    'src/nxt/slot_example.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('process_example',
    'src/nxt/process_example.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('signal_example',
    'src/nxt/signal_example.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('nxthttp',
    'src/nxtio/nxthttp.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  executable('nxtchat',
    'src/nxtio/nxtchat.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )

  if arrow_dataset_available
    executable('parquet_dataset_example',
      'demo/parquet_dataset.cpp',
      include_directories : nxt_inc,
      dependencies : arrow_dataset_deps,
    )
  endif

  subdir('demo')
endif

if get_option('llm_tool')
  executable('nxtllm',
    'src/nxtai/nxtllm.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
    cpp_args : ['-Wno-unused-parameter'],
  )
endif

executable('nxttrace',
  'src/nxtai/nxttrace.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)

if get_option('tests')
  nxt_test_inc = include_directories('test')

  nxt_raster_test = executable('raster-test',
    'test/raster-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_core_dep],
  )

  nxt_http_test = executable('http-test',
    'test/http-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_core_dep],
  )

  nxt_http_io_test = executable('http-io-test',
    'test/http-io-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_buffers_test = executable('buffers-test',
    'test/buffers-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_input_test = executable('input-test',
    'test/input-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_llm_test = executable('llm-test',
    'test/llm-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_net_include_test = executable('net-include-test',
    'test/net-include-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_terminal_test = executable('terminal-test',
    'test/terminal-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_subprocess_test = executable('subprocess-test',
    'test/subprocess-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_text_field_test = executable('text-field-test',
    'test/text-field-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  nxt_trace_smoke_test = executable('trace-smoke-test',
    'test/trace-smoke-test.cpp',
    include_directories : [nxt_inc, nxt_test_inc],
    dependencies : [nxt_dep],
  )

  test('raster-test', nxt_raster_test)
  test('http-test', nxt_http_test)
  test('http-io-test', nxt_http_io_test)
  test('buffers-test', nxt_buffers_test)
  test('input-test', nxt_input_test)
  test('llm-test', nxt_llm_test)
  test('net-include-test', nxt_net_include_test)
  test('terminal-test', nxt_terminal_test)
  test('subprocess-test', nxt_subprocess_test)
  test('text-field-test', nxt_text_field_test)
endif
