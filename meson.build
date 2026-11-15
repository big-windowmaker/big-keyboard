# SPDX-License-Identifier: GPL-2.0-or-later
project('big-keyboard', 'c', version: '0.2.0', license: 'GPL-2.0-or-later', meson_version: '>= 0.61',
        default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments('-DBK_VERSION="@0@"'.format(meson.project_version()),
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                      language: 'c')

glib = dependency('glib-2.0', version: '>= 2.70')
gio = dependency('gio-2.0')
udev = dependency('libudev')
libm = cc.find_library('m', required: false)

# Núcleo sem interface (testado sem X11 nem Wayland).
core_inc = include_directories('src/core', 'src/input', 'src/gamepad')
core = static_library('bkcore',
  'src/core/bk-keys.c', 'src/core/bk-geom.c', 'src/core/bk-config.c',
  'src/input/big-wm-gamepad.c', 'src/input/bk-rtouch.c', 'src/input/bk-touchmouse.c', 'src/gamepad/bk-vpad.c', 'src/gamepad/bk-controls.c',
  include_directories: core_inc, dependencies: [glib, udev, libm])
core_dep = declare_dependency(link_with: core, include_directories: core_inc, dependencies: [glib, udev, libm])

gtk = dependency('gtk+-3.0', version: '>= 3.24')
xkb = dependency('xkbcommon')
# atspi: abrir o teclado ao focar um campo de texto (como no celular)
atspi = dependency('atspi-2')
app_deps = [core_dep, gtk, gio, xkb, atspi]
app_args = []

x11 = dependency('x11', required: get_option('x11'))
xtst = dependency('xtst', required: get_option('x11'))
gtk_x11 = dependency('gtk+-x11-3.0', required: get_option('x11'))
if x11.found() and xtst.found() and gtk_x11.found()
  app_deps += [x11, xtst, gtk_x11]
  app_args += '-DBK_HAVE_X11'
endif

wl_client = dependency('wayland-client', required: get_option('wayland'))
gtk_wl = dependency('gtk+-wayland-3.0', required: get_option('wayland'))
layer_shell = dependency('gtk-layer-shell-0', version: '>= 0.6', required: get_option('wayland'))
wl_src = []
if wl_client.found() and gtk_wl.found() and layer_shell.found()
  scanner = find_program(dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))
  xml = files('protocols/virtual-keyboard-unstable-v1.xml')
  wl_src += custom_target('vk-client-header', input: xml, output: 'virtual-keyboard-unstable-v1-client-protocol.h',
                          command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  wl_src += custom_target('vk-code', input: xml, output: 'virtual-keyboard-unstable-v1-protocol.c',
                          command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  vp_xml = files('protocols/wlr-virtual-pointer-unstable-v1.xml')
  wl_src += custom_target('vp-client-header', input: vp_xml, output: 'wlr-virtual-pointer-unstable-v1-client-protocol.h',
                          command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  wl_src += custom_target('vp-code', input: vp_xml, output: 'wlr-virtual-pointer-unstable-v1-protocol.c',
                          command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  # xdg-output: o nome do conector de cada monitor (o GTK 3 não o dá no Wayland)
  wl_protocols = dependency('wayland-protocols', version: '>= 1.20')
  xo_xml = wl_protocols.get_variable('pkgdatadir') / 'unstable/xdg-output/xdg-output-unstable-v1.xml'
  wl_src += custom_target('xo-client-header', input: xo_xml, output: 'xdg-output-unstable-v1-client-protocol.h',
                          command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  wl_src += custom_target('xo-code', input: xo_xml, output: 'xdg-output-unstable-v1-protocol.c',
                          command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  app_deps += [wl_client, gtk_wl, layer_shell]
  app_args += '-DBK_HAVE_WAYLAND'
endif

executable('big-keyboard',
  'src/ui/bk-app.c', 'src/ui/bk-focus.c', 'src/ui/bk-osk.c', 'src/ui/bk-overlay.c', 'src/ui/bk-outputs.c', 'src/ui/bk-settings.c', 'src/ui/bk-remote.c', 'src/ui/bk-ctlpaint.c',
  'src/input/bk-inject.c',
  wl_src, c_args: app_args, dependencies: app_deps, include_directories: include_directories('src/ui'),
  install: true)

if x11.found() and gtk_x11.found()
  executable('big-keyboard-dockapp', 'src/dockapp/bk-dockapp.c', 'src/dockapp/bk-dock.c',
    dependencies: [gtk, gtk_x11, x11, gio, libm, dependency('cairo-xlib')], install: true)
endif

subdir('data')

foreach t : ['keys', 'geom', 'config', 'controls', 'vpad', 'rtouch', 'touchmouse']
  test(t, executable('test-' + t, 'tests/test-@0@.c'.format(t), dependencies: core_dep))
endforeach

# Dockapp sem X11: geometria, cliques e desenho (pixels).
test('dock', executable('test-dock', 'tests/test-dock.c', 'src/dockapp/bk-dock.c',
  dependencies: [glib, dependency('cairo'), libm]))
