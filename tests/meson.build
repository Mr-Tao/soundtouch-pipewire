test_specs = {
  'volume': 'test-volume.c',
  'config': 'test-config.c',
  'control-service': 'test-control-service.c',
  'wapi': 'test-wapi.c',
  'discovery': 'test-discovery.c',
  'pipewire-backend': 'test-pipewire-backend.c',
  'presets': 'test-presets.c',
  'daemon-safety': 'test-daemon-safety.c',
  'direct-bootstrap-v2-state': 'test-direct-bootstrap-v2-state.c',
  'direct-bootstrap-v2': 'test-direct-bootstrap-v2.c',
  'direct-manager-v2': 'test-direct-manager-v2.c',
  'direct-pw-context-v2': 'test-direct-pw-context-v2.c',
  'direct-route-v2': 'test-direct-route-v2.c',
  'direct-service-v2-runtime': 'test-direct-service-v2-runtime.c',
  'direct-status-v2': 'test-direct-status-v2.c',
  'direct-volume-v2': 'test-direct-volume-v2.c',
  'direct-volume-v2-wapi': 'test-direct-volume-v2-wapi.c',
  'direct-output-v2': 'test-direct-output-v2.c',
  'sink-fault-policy': 'test-sink-fault-policy.c',
  'instance-lock': 'test-instance-lock.c',
  'mpris-router': 'test-mpris-router.c',
  'topology-controller': 'test-topology-controller.c',
  'topology-executor': 'test-topology-executor.c',
  'topology-preflight': 'test-topology-preflight.c',
  'topology': 'test-topology.c',
  'zone-volume': 'test-zone-volume.c',
}

test_env = environment()
test_env.set('GSETTINGS_BACKEND', 'memory')
pipewire_lsan_suppression = meson.current_source_dir() / 'pipewire-integration.lsan.supp'
pipewire_backend_test_exe = []
direct_output_v2_test_exe = []

foreach test_name, test_source : test_specs
  test_case_env = test_env
  if test_name == 'direct-pw-context-v2'
    test_case_env = environment()
    test_case_env.set('GSETTINGS_BACKEND', 'memory')
    test_case_env.append(
      'LSAN_OPTIONS',
      'suppressions=' + pipewire_lsan_suppression,
      separator: ':',
    )
  endif
  test_exe = executable(
    'test-' + test_name,
    files(test_source),
    include_directories: stpw_inc,
    dependencies: stpw_core_dep,
  )
  test(
    test_name,
    test_exe,
    env: test_case_env,
    timeout: test_name in ['topology-controller', 'topology-executor'] ? 60 : 30,
  )
  if test_name == 'pipewire-backend'
    pipewire_backend_test_exe = test_exe
  elif test_name == 'direct-output-v2'
    direct_output_v2_test_exe = test_exe
  endif
endforeach

lua = find_program('lua', required: true)
test(
  'current-route-defaults',
  lua,
  args: [
    files('test-current-route-defaults.lua'),
    meson.project_source_root() / 'wireplumber/scripts/soundtouch/current-route-defaults.lua',
  ],
  env: test_env,
)

daemon_settle_test_exe = executable(
  'test-daemon-settle',
  files('test-daemon-settle.c'),
  include_directories: stpw_inc,
  dependencies: stpw_core_dep,
)
test(
  'daemon-settle',
  daemon_settle_test_exe,
  env: test_env,
  timeout: 30,
)

pipewire_integration_runner = find_program('run-pipewire-integration.sh')
pipewire_integration_module = get_option('private_module_test_path')
if pipewire_integration_module == ''
  pipewire_integration_module = get_option('private_module_dir') / 'libpipewire-module-soundtouch-raop-sink.so'
endif
pipewire_integration_env = environment()
pipewire_integration_env.append(
  'LSAN_OPTIONS',
  'suppressions=' + pipewire_lsan_suppression,
  separator: ':',
)
test(
  'pipewire-v1-lifecycle-isolated',
  pipewire_integration_runner,
  args: [
    'v1',
    pipewire_backend_test_exe,
    pipewire_integration_module,
  ],
  env: pipewire_integration_env,
  is_parallel: false,
  timeout: 30,
)
test(
  'direct-output-v2-isolated',
  pipewire_integration_runner,
  args: [
    'v2',
    pipewire_backend_test_exe,
    pipewire_integration_module,
    direct_output_v2_test_exe,
  ],
  env: pipewire_integration_env,
  is_parallel: false,
  timeout: 40,
)

test_eula_path = meson.current_source_dir() / 'fixtures' / 'EULA.txt'
stpw_test_cli = executable(
  'soundtouch-pipewire-test-cli',
  stpw_cli_sources,
  include_directories: stpw_inc,
  dependencies: stpw_core_dep,
  c_args: ['-DSTPW_TEST_EULA_PATH="' + test_eula_path + '"'],
)

cli_env = environment()
sh = find_program('sh')
assert_exit = files('assert-exit.sh')
cli_env.set('XDG_CONFIG_HOME', meson.current_build_dir() / 'xdg-config')

test(
  'cli-eula-show',
  stpw_test_cli,
  args: ['eula', 'show'],
  env: cli_env,
)
test(
  'cli-eula-wrong-version',
  stpw_test_cli,
  args: ['eula', 'accept', 'NOT-THE-EULA'],
  env: cli_env,
  should_fail: true,
)
test(
  'cli-eula-acceptance-boundary',
  sh,
  args: [files('test-eula-acceptance.sh'), stpw_test_cli],
  env: test_env,
)
test(
  'cli-eula-production-path',
  sh,
  args: [files('test-eula-production-path.sh'), stpw_exe, test_eula_path],
  env: test_env,
)
test(
  'cli-daemon-rejects-unknown-option',
  stpw_test_cli,
  args: ['daemon', '--unknown'],
  env: cli_env,
  should_fail: true,
)
test(
  'cli-direct-v2-requires-device',
  sh,
  args: [assert_exit, '64', stpw_test_cli, 'direct-v2'],
  env: cli_env,
)
test(
  'cli-direct-v2-rejects-repeated-device',
  sh,
  args: [
    assert_exit, '64', stpw_test_cli,
    'direct-v2', '--device', '020000000001',
    '--device', '020000000001',
  ],
  env: cli_env,
)
test(
  'cli-direct-v2-rejects-repeated-config',
  sh,
  args: [
    assert_exit, '64', stpw_test_cli, 'direct-v2',
    '--device', '020000000001',
    '--config', 'one.ini',
    '--config', 'two.ini',
  ],
  env: cli_env,
)
test(
  'cli-direct-v2-rejects-invalid-device',
  sh,
  args: [
    assert_exit, '64', stpw_test_cli,
    'direct-v2', '--device', 'not-a-mac',
  ],
  env: cli_env,
)

test(
  'cli-service-v2-rejects-device',
  sh,
  args: [
    assert_exit, '64', stpw_test_cli,
    'service-v2', '--device', '020000000001',
  ],
  env: cli_env,
)
test(
  'cli-service-v2-rejects-repeated-config',
  sh,
  args: [
    assert_exit, '64', stpw_test_cli, 'service-v2',
    '--config', 'one.ini',
    '--config', 'two.ini',
  ],
  env: cli_env,
)
service_unit_test_path = get_option('service_unit_test_path')
if service_unit_test_path == ''
  service_unit_test_path = meson.project_source_root() / 'packaging/aur/soundtouch-pipewire.service'
endif

test(
  'packaged-service-selects-v2-only',
  sh,
  args: [
    files('assert-service-v2-unit.sh'),
    service_unit_test_path,
  ],
  env: test_env,
)
