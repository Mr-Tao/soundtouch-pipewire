# This subdirectory is entered only when the optional GTK controller is
# enabled by the top-level build.

dependency('gtk4', version: '>=4.12')
dependency('gio-2.0', version: '>=2.74')
dependency('glib-2.0', version: '>=2.74')

cargo = find_program('cargo', required: true)
sh = find_program('sh', required: true)
i18n = import('i18n')

control_manifest = meson.current_source_dir() / 'Cargo.toml'
control_target_dir = meson.current_build_dir() / 'cargo-target'
control_locale_dir = get_option('prefix') / get_option('localedir')

control_cargo_inputs = files(
  'Cargo.toml',
  'Cargo.lock',
  'build.rs',
  'resources/resources.gresource.xml',
  'resources/style.css',
  'src/app.rs',
  'src/dbus.rs',
  'src/i18n.rs',
  'src/lib.rs',
  'src/main.rs',
  'src/mixer.rs',
  'src/model.rs',
  'tools/meson-cargo-build.sh',
)

control_binary = custom_target(
  'soundtouch-pipewire-control-cargo',
  input: control_cargo_inputs,
  output: 'soundtouch-pipewire-control',
  command: [
    sh,
    files('tools/meson-cargo-build.sh'),
    cargo,
    control_manifest,
    control_target_dir,
    '@OUTPUT@',
    control_locale_dir,
  ],
  build_by_default: true,
  console: true,
  install: true,
  install_dir: get_option('bindir'),
  install_tag: 'soundtouch-pipewire-control',
)

i18n.merge_file(
  input: 'data/io.github.Mr_Tao.SoundTouchPipeWire.Control.desktop',
  output: 'io.github.Mr_Tao.SoundTouchPipeWire.Control.desktop',
  po_dir: 'po',
  type: 'desktop',
  install: true,
  install_dir: get_option('datadir') / 'applications',
  install_tag: 'soundtouch-pipewire-control',
)
i18n.merge_file(
  input: 'data/io.github.Mr_Tao.SoundTouchPipeWire.Control.metainfo.xml',
  output: 'io.github.Mr_Tao.SoundTouchPipeWire.Control.metainfo.xml',
  po_dir: 'po',
  type: 'xml',
  install: true,
  install_dir: get_option('datadir') / 'metainfo',
  install_tag: 'soundtouch-pipewire-control',
)
install_data(
  'data/icons/hicolor/scalable/apps/io.github.Mr_Tao.SoundTouchPipeWire.Control.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
  install_tag: 'soundtouch-pipewire-control',
)

# The CSS is compiled into the executable as a GResource by build.rs.
subdir('po')
