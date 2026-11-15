# SPDX-License-Identifier: GPL-2.0-or-later
install_data('big-keyboard.desktop', install_dir: get_option('datadir') / 'applications')
install_data('big-keyboard-dockapp.desktop', install_dir: get_option('datadir') / 'applications')
install_data('big-keyboard-autostart.desktop', rename: 'big-keyboard.desktop',
             install_dir: get_option('sysconfdir') / 'xdg' / 'autostart')
install_data('icons/big-keyboard.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
# uaccess para o touchscreen virtual do Sunshine (toque remoto); só ele
udev_pc = dependency('udev', required: false)
udevdir = udev_pc.found() ? udev_pc.get_variable('udevdir') : get_option('prefix') / 'lib' / 'udev'
install_data('70-big-keyboard-remote-touch.rules', install_dir: udevdir / 'rules.d')
