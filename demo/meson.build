ls_demo_dependencies = []
ls_demo_available = false
http_client_demo_dependencies = []
http_client_demo_available = false

if host_machine.system() in ['linux', 'darwin']
  ls_demo_available = true
endif

if ls_demo_available
  executable('nxt-ls-demo',
    'ls_demo.cpp',
    include_directories : nxt_inc,
    dependencies : ls_demo_dependencies,
  )

  executable('nxt-tui-demo',
    'tui_demo.cpp',
    include_directories : nxt_inc,
    dependencies : ls_demo_dependencies + [nxt_core_dep],
  )

  executable('nxt-shell-scope-demo',
    'shell_scope_demo.cpp',
    include_directories : nxt_inc,
    dependencies : ls_demo_dependencies + [nxt_core_dep],
  )
endif

nxt_cares_dep = dependency('libcares', required : false)

if ls_demo_available and nxt_cares_dep.found()
  http_client_demo_available = true
  http_client_demo_dependencies = ls_demo_dependencies + [
    nxt_core_dep,
    nxt_cares_dep,
  ]
endif

if http_client_demo_available
  executable('nxt-http-client-demo',
    'http_client_demo.cpp',
    include_directories : nxt_inc,
    dependencies : http_client_demo_dependencies,
  )

  executable('nxt-openai-sse-demo',
    'openai_sse_demo.cpp',
    include_directories : nxt_inc,
    dependencies : http_client_demo_dependencies,
  )
else
  message('no HTTP demo dependencies found; skipping nxt-http-client-demo')
endif

if not nxt_cares_dep.found()
  message('libcares not found; skipping nxt-http-client-demo')
endif
