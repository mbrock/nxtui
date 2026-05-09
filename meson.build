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

nxt_mdspan_args = [
  '-DMDSPAN_USE_BRACKET_OPERATOR=1',
  '-DMDSPAN_USE_PAREN_OPERATOR=0',
]

nxt_inc = include_directories(
  'src',
  'vendor/mdspan/include',
)

nxt_core_deps = []

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

nxt_runtime_sources = files(
  'src/nxtio/app.cpp',
  'src/nxtio/http.cpp',
  'src/nxtio/input.cpp',
  'src/nxtio/net.cpp',
  'src/nxtio/signal-pipe.cpp',
)

nxt_lib = static_library('nxtui',
  nxt_runtime_sources,
  include_directories : nxt_inc,
  dependencies : nxt_runtime_deps,
  install : true,
)

nxt_dep = declare_dependency(
  link_with : nxt_lib,
  include_directories : nxt_inc,
  dependencies : nxt_runtime_deps,
)

if get_option('examples')
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

  executable('nxthttp',
    'src/nxtio/nxthttp.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep],
  )
endif

if get_option('llm_tool')
  duckdb_dep = dependency('DuckDB', method : 'cmake', required : false)
  duckdb_rpath = ''

  if not duckdb_dep.found()
    duckdb_prefix = meson.project_build_root() / 'deps' / 'duckdb'
    duckdb_dep = declare_dependency(
      compile_args : ['-I' + duckdb_prefix],
      link_args : [duckdb_prefix / 'libduckdb.so'],
    )
    duckdb_rpath = duckdb_prefix
  endif

  executable('nxtllm',
    'src/nxtio/nxtllm.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep, duckdb_dep],
    cpp_args : ['-Wno-unused-parameter'],
    build_rpath : duckdb_rpath,
  )
endif

if get_option('tests')
  nxt_test_inc = include_directories('test')

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
    dependencies : [nxt_dep, nxt_libvterm_dep],
  )

  test('raster-test', nxt_raster_test)
  test('http-test', nxt_http_test)
  test('http-io-test', nxt_http_io_test)
  test('buffers-test', nxt_buffers_test)
  test('input-test', nxt_input_test)
  test('llm-test', nxt_llm_test)
  test('net-include-test', nxt_net_include_test)
  test('terminal-test', nxt_terminal_test)
endif
