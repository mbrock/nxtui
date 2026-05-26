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
ng_http_client_demo_dependencies = []
ng_http_client_demo_available = false

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

  executable('ng-tui-demo',
    'ng_tui_demo.cpp',
    include_directories : [nxt_inc, nxt_ng_inc],
    dependencies : ng_ls_demo_dependencies + [nxt_core_dep],
  )

  executable('ng-shell-scope-demo',
    'ng_shell_scope_demo.cpp',
    include_directories : [nxt_inc, nxt_ng_inc],
    dependencies : ng_ls_demo_dependencies + [nxt_core_dep],
  )
endif

nxt_cares_dep = dependency('libcares', required : false)

if ng_ls_demo_available and nxt_cares_dep.found()
  ng_http_client_demo_available = true
  ng_http_client_demo_dependencies = ng_ls_demo_dependencies + [
    nxt_core_dep,
    nxt_cares_dep,
  ]
endif

if ng_http_client_demo_available
  executable('ng-http-client-demo',
    'ng_http_client_demo.cpp',
    include_directories : [nxt_inc, nxt_ng_inc],
    dependencies : ng_http_client_demo_dependencies,
  )
else
  message('no ng HTTP demo dependencies found; skipping ng-http-client-demo')
endif

if host_machine.system() == 'linux' and not nxt_liburing_dep.found()
  message('liburing not found; skipping ng-ls-demo')
endif

if not nxt_cares_dep.found()
  message('libcares not found; skipping ng-http-client-demo')
endif
