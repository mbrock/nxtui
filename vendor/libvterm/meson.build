project('libvterm', 'c',
  version : '0.3.3',
  default_options : ['warning_level=1'],
)

perl = find_program('perl')
tbl2inc = files('tbl2inc_c.pl')

# Generate encoding .inc files from .tbl files
encoding_inc_dir = meson.current_build_dir() / 'encoding'

decdrawing_inc = custom_target('DECdrawing.inc',
  input : 'src/encoding/DECdrawing.tbl',
  output : 'DECdrawing.inc',
  command : [perl, tbl2inc, '@INPUT@'],
  capture : true,
)

uk_inc = custom_target('uk.inc',
  input : 'src/encoding/uk.tbl',
  output : 'uk.inc',
  command : [perl, tbl2inc, '@INPUT@'],
  capture : true,
)

libvterm_sources = files(
  'src/encoding.c',
  'src/keyboard.c',
  'src/mouse.c',
  'src/parser.c',
  'src/pen.c',
  'src/screen.c',
  'src/state.c',
  'src/unicode.c',
  'src/vterm.c',
)

libvterm_inc = include_directories('include', 'src')

libvterm = static_library('vterm',
  libvterm_sources,
  decdrawing_inc,
  uk_inc,
  include_directories : libvterm_inc,
  # Add build dir to include path for generated .inc files
  c_args : ['-I' + meson.current_build_dir()],
  install : false,
)

libvterm_dep = declare_dependency(
  link_with : libvterm,
  include_directories : include_directories('include'),
)

meson.override_dependency('libvterm', libvterm_dep)
