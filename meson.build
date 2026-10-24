project('jimi', 'c',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
  '-Wundef',
  '-Wlogical-op',
  '-Wmissing-include-dirs',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Winit-self',
  '-Wstrict-prototypes',
  '-Wimplicit-fallthrough=2',
  '-Wendif-labels',
  '-Wstrict-aliasing=2',
  # Makes -Wstrict-aliasing take effect at -O0 too.
  '-fstrict-aliasing',
  '-Woverflow',
  '-Wmissing-prototypes',
  '-Walloca',
  '-Wunused-macros',
  '-Wshadow',

  '-Wno-unused-parameter',
  '-Wno-expansion-to-defined',
]), language: 'c')

wayland_protocols = dependency('wayland-protocols', version: '>=1.49')

deps = [
  dependency('libdrm'),
  dependency('libdisplay-info'),
  dependency('gbm'),
  dependency('libseat'),
  dependency('wayland-server', version: '>=1.26'),
  wayland_protocols,
  dependency('libinput'),
  dependency('libudev'),
  dependency('pixman-1'),
  dependency('pangocairo'),
  dependency('librsvg-2.0'),
  dependency('glib-2.0'),
  dependency('fontconfig'),
  dependency('xcursor'),
  dependency('xkbcommon'),
  dependency('yaml-0.1'),
  dependency('vulkan'),
  dependency('xcb'),
  dependency('xcb-composite'),
  dependency('xcb-cursor'),
  dependency('xcb-icccm'),
  dependency('xcb-xfixes'),
  cc.find_library('m'),
]

jimi_inc = include_directories('include')

protocol_sources = []
subdir('protocol')

subdir('src')

executable('jimi',
  jimi_sources + protocol_sources,
  dependencies: deps,
  include_directories: jimi_inc,
  c_args: ['-D_POSIX_C_SOURCE=200809L'],
  install: true)

# xdg-desktop-portal reads the one named after XDG_CURRENT_DESKTOP.
install_data('jimi-portals.conf',
  install_dir: get_option('datadir') / 'xdg-desktop-portal')

# Display managers list it as a session.
install_data('jimi.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions')
