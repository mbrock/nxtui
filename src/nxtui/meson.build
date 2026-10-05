# Independently invokable after the Windows early branch. Deliberately no
# nxt-core, POSIX runtime, Wisp, libvterm, or SDL_GPU dependency.
nxtui_sdl_available = false
nxtui_sdl_dep = disabler()
nxtui_sdl = dependency('sdl3', version : '>= 3.2', required : get_option('sdl_ui'))
nxtui_ttf = dependency('sdl3-ttf', version : '>= 3.2', required : get_option('sdl_ui'))
if nxtui_sdl.found() and nxtui_ttf.found()
  nxtui_sdl_available = true
  nxtui_sdl_inc = include_directories('..')
  nxtui_sdl_naming = {}
  if host_machine.system() == 'windows' and get_option('default_library') == 'static' and meson.get_compiler('cpp').get_define('_MSC_VER') != ''
    # GNU-syntax Clang with the MSVC ABI resolves -lnxtui-sdl to
    # nxtui-sdl.lib, just like nxtrt-iocp. Leave shared/Unix naming alone.
    nxtui_sdl_naming = {'name_prefix' : '', 'name_suffix' : 'lib'}
  endif
  nxtui_sdl_lib = library('nxtui-sdl', 'sdl.cpp',
    include_directories : nxtui_sdl_inc,
    dependencies : [nxtui_sdl, nxtui_ttf],
    install : true,
    kwargs : nxtui_sdl_naming,
  )
  nxtui_sdl_dep = declare_dependency(
    link_with : nxtui_sdl_lib,
    include_directories : nxtui_sdl_inc,
    dependencies : [nxtui_sdl, nxtui_ttf],
  )
  install_headers('sdl.hpp', 'ui.hpp', 'ui-units.hpp', 'layout.hpp',
    'units.hpp', 'style.hpp', subdir : 'nxtui')
  import('pkgconfig').generate(nxtui_sdl_lib,
    name : 'nxtui-sdl',
    description : 'Fractional rhythm layouts and SDL3_ttf graphical painting',
    subdirs : '.',
  )
endif
