project('nxtui', 'cpp', 'c',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=3',
    'buildtype=debugoptimized',
    'default_library=shared',
  ],
  meson_version : '>= 1.3',
)

nxt_section_args = [
  '-ffunction-sections',
  '-fdata-sections',
]

add_project_arguments(nxt_section_args, language : ['c', 'cpp'])

if host_machine.system() == 'darwin'
  add_project_link_arguments('-Wl,-dead_strip', language : ['cpp'])
else
  add_project_link_arguments('-Wl,--gc-sections', language : ['cpp'])
endif

cpp = meson.get_compiler('cpp')

# Keep useful diagnostics on for both compilers, including checks that Meson's
# -Wall/-Wextra/-Wpedantic baseline does not enable.
add_project_arguments(cpp.get_supported_arguments(
  '-Wformat=2',
  '-Wundef',
  '-Wimplicit-fallthrough',
  '-Wextra-semi',
), language : 'cpp')

if cpp.get_id() == 'gcc'
  # GCC diagnoses compiler-generated coroutine frames and generic lambda
  # closures containing local types, although those types never cross a TU.
  add_project_arguments('-Wno-subobject-linkage', language : 'cpp')
endif

nxt_filc = cpp.get_define('__FILC__') != ''
nxt_wand = get_option('default_wand')
if nxt_filc
  if nxt_wand == 'uring'
    error('Fil-C does not support io_uring; use -Ddefault_wand=epoll')
  elif nxt_wand == 'auto'
    nxt_wand = 'epoll'
  endif
endif
nxt_wand_args = []
if nxt_wand != 'auto'
  if nxt_wand in ['epoll', 'uring']
    if host_machine.system() != 'linux'
      error(nxt_wand + ' requires Linux')
    endif
  elif host_machine.system() not in ['darwin', 'dragonfly', 'freebsd', 'netbsd', 'openbsd']
    error('kqueue requires BSD or macOS')
  endif
  nxt_wand_args = ['-DNXTRT_DEFAULT_WAND=' + nxt_wand + '_wand']
endif
message('Default Wand: ' + nxt_wand)

utf8proc_dep = dependency(
  'libutf8proc',
  fallback : ['utf8proc', 'utf8proc_dep'],
  default_options : ['tests=disabled'],
)

# uncompress2 reports consumed input so compressed tapes reject trailing data.
zlib_dep = dependency('zlib', version : '>= 1.2.9')
boost_dep = dependency('boost')
zstd_dep = cpp.find_library('zstd', required : false)
brotli_dep = dependency('libbrotlidec', required : false)
cares_dep = dependency('libcares', required : get_option('cares'))

nxt_compression_deps = [zlib_dep]
nxt_compression_args = []
nxt_dns_deps = []
nxt_dns_args = []

if zstd_dep.found() and cpp.has_header('zstd.h')
  message('zstd decompression available')
  nxt_compression_deps += zstd_dep
  nxt_compression_args += '-DNXTRT_HAVE_ZSTD=1'
endif

if brotli_dep.found()
  message('brotli decompression available')
  nxt_compression_deps += brotli_dep
  nxt_compression_args += '-DNXTRT_HAVE_BROTLI=1'
endif

if cares_dep.found()
  message('c-ares DNS resolver available')
  nxt_dns_deps += cares_dep
  nxt_dns_args += '-DNXTRT_HAVE_CARES=1'
else
  message('c-ares DNS resolver not available; using libc getaddrinfo')
endif

nxt_mdspan_args = [
  '-DMDSPAN_USE_BRACKET_OPERATOR=1',
  '-DMDSPAN_USE_PAREN_OPERATOR=0',
]

# Diagnostics belong to project sources, not bundled dependency headers.
nxt_inc = [
  include_directories('src'),
  include_directories('vendor/hub/include', 'vendor/mdspan/include',
    is_system : true),
]

subdir('src')

if get_option('demo')
  subdir('demo')
endif

subdir('bench')
subdir('test')

if get_option('dev') and get_option('demo') and get_option('tests') and get_option('llm_tool')
  nxt_dev_sources = [
    'src/nxt-dev.cpp',
    'src/nxt/stacktrace.cpp',
    'src/nxtai/nxtllm.cpp',
    'demo/ls_demo.cpp',
    'demo/tui_demo.cpp',
    'demo/http_client_demo.cpp',
    'demo/openai_sse_demo.cpp',
  ]

  if host_machine.system() == 'linux'
    nxt_dev_sources += ['demo/shell_scope_demo.cpp']
  endif

  nxt_dev = executable('nxt-dev',
    nxt_dev_sources + nxt_core_sources + nxt_test_sources,
    include_directories : [nxt_inc, include_directories('test')],
    dependencies : nxt_core_deps + [nxt_libvterm_dep] + nxt_stacktrace_deps
      + nxt_test_extra_deps + nxt_dns_deps,
    cpp_args : [
      '-DNXT_EMBEDDED_MAIN=1',
    ] + nxt_mdspan_args
      + nxt_compression_args
      + nxt_stacktrace_cpp_args
      + nxt_test_cpp_args
      + nxt_dns_args,
  )
endif

if get_option('wisp_tool')
  custom_target('wisp-root-link',
    output : 'wisp',
    input : wisp_tool,
    command : ['ln', '-sf', 'src/wisp', '@OUTPUT@'],
    build_by_default : true,
  )
endif

if get_option('llm_tool')
  custom_target('nxtllm-root-link',
    output : 'nxtllm',
    input : nxtllm,
    command : ['ln', '-sf', 'src/nxtllm', '@OUTPUT@'],
    build_by_default : true,
  )

  custom_target('nxtmt-root-link',
    output : 'nxtmt',
    input : nxtmt,
    command : ['ln', '-sf', 'src/nxtmt', '@OUTPUT@'],
    build_by_default : true,
  )
endif

if get_option('benchmarks')
  custom_target('nxt-echo-bench-root-link',
    output : 'nxt-echo-bench',
    input : nxt_echo_bench,
    command : ['ln', '-sf', 'bench/nxt-echo-bench', '@OUTPUT@'],
    build_by_default : true,
  )
endif

if get_option('tests')
  custom_target('nxt-tests-root-link',
    output : 'nxt-tests',
    input : nxt_tests,
    command : ['ln', '-sf', 'test/nxt-tests', '@OUTPUT@'],
    build_by_default : true,
  )
endif
