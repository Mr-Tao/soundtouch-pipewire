project(
  'soundtouch-pipewire',
  'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.3.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',
    'werror=true',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

cc = meson.get_compiler('c')
gnome = import('gnome')
glib_dep = dependency('glib-2.0', version: '>=2.74')
gio_dep = dependency('gio-2.0', version: '>=2.74')
gobject_dep = dependency('gobject-2.0', version: '>=2.74')
pipewire_dep = dependency('libpipewire-0.3', version: '>=1.0')
avahi_client_dep = dependency('avahi-client', version: '>=0.8')
avahi_glib_dep = dependency('avahi-glib', version: '>=0.8')
libsoup_dep = dependency('libsoup-3.0', version: '>=3.2')
libxml_dep = dependency('libxml-2.0', version: '>=2.9')
json_glib_dep = dependency('json-glib-1.0', version: '>=1.6')
m_dep = cc.find_library('m', required: true)

dbus_interface = files('data/io.github.Mr_Tao.SoundTouchPipeWire1.xml')
stpw_dbus_sources = gnome.gdbus_codegen(
  'stpw-dbus',
  sources: dbus_interface,
  autocleanup: 'all',
  interface_prefix: 'io.github.Mr_Tao.SoundTouchPipeWire1.',
  namespace: 'StpwDbus',
  object_manager: true,
  install_header: false,
)

status_v2_dbus_interface = files(
  'data/io.github.Mr_Tao.SoundTouchPipeWire2.xml',
)
stpw_status_v2_dbus_sources = gnome.gdbus_codegen(
  'stpw-status-v2-dbus',
  sources: status_v2_dbus_interface,
  autocleanup: 'all',
  interface_prefix: 'io.github.Mr_Tao.SoundTouchPipeWire2.',
  namespace: 'StpwStatusV2',
  object_manager: true,
  install_header: false,
)
install_data(
  dbus_interface,
  status_v2_dbus_interface,
  install_dir: get_option('datadir') / 'dbus-1/interfaces',
  install_tag: 'soundtouch-pipewire',
)

build_config = configuration_data()
build_config.set_quoted('STPW_PRIVATE_MODULE_DIR', get_option('private_module_dir'))
build_config.set_quoted(
  'STPW_EULA_INSTALL_PATH',
  get_option('prefix') / get_option('datadir') / 'licenses' /
    'soundtouch-pipewire' / 'EULA.txt',
)
build_config.set_quoted(
  'STPW_SYSTEM_MODULE_DIR',
  pipewire_dep.get_variable(
    pkgconfig: 'moduledir',
    default_value: '/usr/lib/pipewire-0.3',
  ),
)
configure_file(
  output: 'stpw-build-config.h',
  configuration: build_config,
)

install_data(
  'wireplumber/scripts/soundtouch/current-route-defaults.lua',
  install_dir: get_option('datadir') / 'wireplumber/scripts/soundtouch',
  install_tag: 'soundtouch-pipewire',
)
install_data(
  'wireplumber/wireplumber.conf.d/90-soundtouch-current-route-defaults.conf',
  install_dir: get_option('datadir') / 'wireplumber/wireplumber.conf.d',
  install_tag: 'soundtouch-pipewire',
)

stpw_inc = include_directories('.', 'include', 'src')
stpw_deps = [
  glib_dep,
  gio_dep,
  gobject_dep,
  pipewire_dep,
  avahi_client_dep,
  avahi_glib_dep,
  libsoup_dep,
  libxml_dep,
  json_glib_dep,
  m_dep,
]

stpw_core_sources = files(
  'src/config.c',
  'src/control-service.c',
  'src/daemon-safety.c',
  'src/direct-bootstrap-v2.c',
  'src/direct-bootstrap-v2-state.c',
  'src/direct-manager-v2.c',
  'src/direct-pw-context-v2.c',
  'src/direct-route-v2.c',
  'src/direct-service-v2-runtime.c',
  'src/direct-status-v2.c',
  'src/direct-output-v2.c',
  'src/direct-volume-v2.c',
  'src/direct-volume-v2-wapi.c',
  'src/discovery.c',
  'src/instance-lock.c',
  'src/mpris-router.c',
  'src/sink-fault-policy.c',
  'src/pipewire-echo.c',
  'src/pipewire-dispatch.c',
  'src/pipewire-props.c',
  'src/pipewire-route-device.c',
  'src/pipewire-backend.c',
  'src/presets.c',
  'src/topology-controller.c',
  'src/topology-executor.c',
  'src/topology-preflight.c',
  'src/topology.c',
  'src/types.c',
  'src/volume.c',
  'src/wapi.c',
  'src/zone-volume.c',
)

stpw_core = static_library(
  'stpw-core',
  stpw_core_sources,
  stpw_dbus_sources,
  stpw_status_v2_dbus_sources,
  include_directories: stpw_inc,
  dependencies: stpw_deps,
  pic: true,
)

stpw_core_dep = declare_dependency(
  link_with: stpw_core,
  include_directories: stpw_inc,
  dependencies: stpw_deps,
)

stpw_cli_sources = files(
  'src/daemon.c',
  'src/direct-daemon-v2.c',
  'src/direct-service-v2.c',
  'src/main.c',
)

stpw_exe = executable(
  'soundtouch-pipewire',
  stpw_cli_sources,
  include_directories: stpw_inc,
  dependencies: stpw_core_dep,
  install: true,
  install_tag: 'soundtouch-pipewire',
)

control_app = get_option('control_app')
if control_app.enabled()
  subdir('control')
elif control_app.auto()
  gtk4_probe = dependency('gtk4', version: '>=4.12', required: false)
  cargo_probe = find_program('cargo', required: false)
  if gtk4_probe.found() and cargo_probe.found()
    subdir('control')
  endif
endif

subdir('tests')
