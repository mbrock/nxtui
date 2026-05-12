executable('build_sim',
  'build_sim.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)

if arrow_dataset_available
  executable('span_browser',
    'span_browser.cpp',
    include_directories : nxt_inc,
    dependencies : [nxt_dep] + arrow_dataset_deps,
  )
endif

executable('cgroup_browser',
  'cgroup_browser.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)

executable('shell_scope',
  'shell_scope.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)
