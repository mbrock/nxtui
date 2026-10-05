if nxtui_sdl_available
  executable('nxt-agent-chat-demo', 'main.cpp',
    dependencies : nxtui_sdl_dep,
    win_subsystem : 'windows',
    install : true,
  )
  if get_option('tests')
    nxtui_graphical_tests = executable('nxtui-graphical-tests', 'ui-test.cpp',
      dependencies : nxtui_sdl_dep,
    )
    # Geometry runs without fonts/video; pass a proportional TTF font on the
    # command line to additionally exercise native shaping and pixel clips.
    test('graphical-layout', nxtui_graphical_tests)
  endif
endif
