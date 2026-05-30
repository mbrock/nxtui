if get_option('benchmarks')
  nxt_echo_bench = executable('nxt-echo-bench',
    'nxtrt-echo-bench.cpp',
    include_directories : nxt_inc,
    dependencies : [
      nxt_core_dep,
      nxt_stacktrace_dep,
    ],
    cpp_args : [
      '-DNXT_RT_ENABLE_TRACE=0',
      '-fno-omit-frame-pointer',
    ],
  )
endif
