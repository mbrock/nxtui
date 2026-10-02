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

  wisp_revision = vcs_tag(
    input : 'wisp/revision.hpp.in',
    output : 'wisp-revision.hpp',
    command : ['git', 'describe', '--always', '--dirty', '--abbrev=40', '--match=NO-TAG'],
    fallback : 'unknown',
  )
  wisp_bench = executable('wisp-bench',
    ['wisp-bench.cpp', wisp_revision],
    dependencies : [nxt_core_dep],
    cpp_args : ['-DWISP_BENCH_BUILDTYPE="' + get_option('buildtype') + '"'],
  )
  wisp_native_bench = executable('wisp-native-bench',
    'wisp/cross-language/native.c',
    override_options : ['c_std=c11'],
  )
  wisp_profile_test = executable('wisp-profile-test',
    'wisp-profile-test.cpp', dependencies : [nxt_core_dep])
  test('wisp-semantic-counters', wisp_profile_test, suite : ['wisp', 'benchmarks'])
  test('wisp-benchmark-fixtures', wisp_bench,
    args : ['all', '1', '0'], suite : ['wisp', 'benchmarks'], timeout : 120)
  foreach bad_args : [['tak', '0'], ['tak', '-1'], ['tak', '1junk'], ['missing-case']]
    test('wisp-benchmark-reject-' + '-'.join(bad_args), wisp_bench,
      args : bad_args, should_fail : true, suite : ['wisp', 'benchmarks'])
  endforeach
endif
