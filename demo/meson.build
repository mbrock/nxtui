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
    dependencies : ls_demo_dependencies + [nxt_core_dep],
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

if ls_demo_available
  http_client_demo_available = true
  http_client_demo_dependencies = ls_demo_dependencies + [
    nxt_core_dep,
  ] + nxt_dns_deps
endif

if http_client_demo_available
  executable('nxt-http-client-demo',
    'http_client_demo.cpp',
    include_directories : nxt_inc,
    dependencies : http_client_demo_dependencies,
    cpp_args : nxt_dns_args,
  )

  executable('nxt-openai-sse-demo',
    'openai_sse_demo.cpp',
    include_directories : nxt_inc,
    dependencies : http_client_demo_dependencies,
    cpp_args : nxt_dns_args,
  )
else
  message('no HTTP demo dependencies found; skipping nxt-http-client-demo')
endif
