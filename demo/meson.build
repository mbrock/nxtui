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

if host_machine.system() == 'linux'
  nxt_liburing_dep = dependency(
    'liburing',
    fallback : ['liburing', 'uring'],
    required : false,
  )
else
  nxt_liburing_dep = disabler()
endif

ng_ls_demo_dependencies = []
ng_ls_demo_available = false

if host_machine.system() == 'darwin'
  ng_ls_demo_available = true
elif nxt_liburing_dep.found()
  ng_ls_demo_available = true
  ng_ls_demo_dependencies += nxt_liburing_dep
endif

if ng_ls_demo_available
  executable('ng-ls-demo',
    'ng_ls_demo.cpp',
    include_directories : [nxt_inc, nxt_ng_inc],
    dependencies : ng_ls_demo_dependencies,
  )

  executable('ng-http-client-demo',
    'ng_http_client_demo.cpp',
    include_directories : [nxt_inc, nxt_ng_inc],
    dependencies : ng_ls_demo_dependencies + [nxt_core_dep, nxt_libcoro_dep],
  )
else
  message('no ng demo runtime found; skipping ng-ls-demo and ng-http-client-demo')
endif

if host_machine.system() == 'linux' and not nxt_liburing_dep.found()
  message('liburing not found; skipping ng-ls-demo')
endif
