nxtdemo_sources = [
  'main.cpp',
  'build_sim.cpp',
  'cgroup_browser.cpp',
  'shell_scope.cpp',
]
nxtdemo_dependencies = [nxt_dep]
nxtdemo_cpp_args = []

if arrow_dataset_available
  nxtdemo_sources += 'span_browser.cpp'
  nxtdemo_dependencies += arrow_dataset_deps
  nxtdemo_cpp_args += '-DNXTDEMO_HAVE_SPAN_BROWSER'
endif

executable('nxtdemo',
  nxtdemo_sources,
  include_directories : nxt_inc,
  dependencies : nxtdemo_dependencies,
  cpp_args : nxtdemo_cpp_args,
)
