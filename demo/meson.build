nxtdemo_sources = [
  'main.cpp',
  'build_sim.cpp',
  'cassette.cpp',
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

nxt_liburing_dep = dependency('liburing', required : false)
if nxt_liburing_dep.found()
  executable('ng-uring-wand-demo',
    'ng_uring_wand.cpp',
    include_directories : nxt_ng_inc,
    dependencies : [nxt_liburing_dep],
  )
else
  message('liburing not found; skipping ng-uring-wand-demo')
endif
