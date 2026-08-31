#!/usr/bin/env bash
set -euo pipefail

if (( $# < 3 || $# > 4 )); then
  printf 'usage: %s v1 TEST-EXECUTABLE PRIVATE-RAOP-MODULE [PRIVATE-ZONE-MODULE]\n' \
    "$0" >&2
  printf '       %s v2 TEST-EXECUTABLE PRIVATE-RAOP-MODULE DIRECT-OUTPUT-TEST\n' \
    "$0" >&2
  exit 64
fi

mode=$1
test_executable=$2
private_module=$3
direct_output_test=
private_zone_module=

case "${mode}" in
  v1)
    private_zone_module=${4:-"$(dirname -- "${private_module}")/libpipewire-module-soundtouch-zone-sink.so"}
    ;;
  v2)
    if (( $# != 4 )); then
      printf 'v2 integration requires DIRECT-OUTPUT-TEST\n' >&2
      exit 64
    fi
    direct_output_test=$4
    ;;
  *)
    printf 'unknown integration mode: %s\n' "${mode}" >&2
    exit 64
    ;;
esac

if [[ ! -x ${test_executable} ]]; then
  printf 'integration test executable is unavailable: %s\n' \
    "${test_executable}" >&2
  exit 77
fi
if [[ ${mode} == v2 && ! -x ${direct_output_test} ]]; then
  printf 'direct output v2 test executable is unavailable: %s\n' \
    "${direct_output_test}" >&2
  exit 77
fi
if [[ ! -f ${private_module} ]]; then
  printf 'private RAOP module is unavailable; skipping: %s\n' \
    "${private_module}" >&2
  exit 77
fi
if [[ ${mode} == v1 && ! -f ${private_zone_module} ]]; then
  printf 'private zone module is unavailable; skipping: %s\n' \
    "${private_zone_module}" >&2
  exit 77
fi
required_tools=(pw-cat pw-cli pw-link wireplumber)
if [[ ${mode} == v2 ]]; then
  required_tools+=(pipewire-pulse pactl)
fi
for required_tool in "${required_tools[@]}"; do
  if ! command -v "${required_tool}" >/dev/null; then
    printf '%s is unavailable; install the PipeWire tools and audio support to run the zone lifecycle test\n' \
      "${required_tool}" >&2
    exit 77
  fi
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
system_module_dir=$(pkg-config --variable=moduledir libpipewire-0.3)
test_root=$(mktemp -d /tmp/stpw-pipewire-integration.XXXXXX)
runtime_dir="${test_root}/runtime"
config_home="${test_root}/config-home"
config_dirs="${test_root}/config-dirs"
cache_home="${test_root}/cache-home"
state_home="${test_root}/state-home"
data_home="${test_root}/data-home"
private_home="${test_root}/home"
module_dir="${test_root}/modules"
server_log="${test_root}/pipewire.log"
wireplumber_log="${test_root}/wireplumber.log"
pulse_runtime="${test_root}/pulse-runtime"
pulse_log="${test_root}/pipewire-pulse.log"
server_pid=
wireplumber_pid=
pulse_pid=

cleanup() {
  if [[ -n ${pulse_pid} ]] && kill -0 "${pulse_pid}" 2>/dev/null; then
    kill "${pulse_pid}"
    for _ in {1..50}; do
      if ! kill -0 "${pulse_pid}" 2>/dev/null; then
        break
      fi
      sleep 0.02
    done
    if kill -0 "${pulse_pid}" 2>/dev/null; then
      kill -KILL "${pulse_pid}"
    fi
    wait "${pulse_pid}" 2>/dev/null || true
  fi
  if [[ -n ${wireplumber_pid} ]] && kill -0 "${wireplumber_pid}" 2>/dev/null; then
    kill "${wireplumber_pid}"
    for _ in {1..50}; do
      if ! kill -0 "${wireplumber_pid}" 2>/dev/null; then
        break
      fi
      sleep 0.02
    done
    if kill -0 "${wireplumber_pid}" 2>/dev/null; then
      kill -KILL "${wireplumber_pid}"
    fi
    wait "${wireplumber_pid}" 2>/dev/null || true
  fi
  if [[ -n ${server_pid} ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill "${server_pid}"
    for _ in {1..50}; do
      if ! kill -0 "${server_pid}" 2>/dev/null; then
        break
      fi
      sleep 0.02
    done
    if kill -0 "${server_pid}" 2>/dev/null; then
      kill -KILL "${server_pid}"
    fi
    wait "${server_pid}" 2>/dev/null || true
  fi
  rm -rf -- "${test_root}"
}
trap cleanup EXIT

mkdir -m 700 "${runtime_dir}" "${config_home}" "${config_dirs}" "${cache_home}" \
  "${state_home}" "${data_home}" "${private_home}" "${module_dir}" \
  "${pulse_runtime}"
install -Dm644 "${script_dir}/pipewire-integration-client.conf" \
  "${config_home}/pipewire/client.conf.d/99-stpw-integration.conf"
install -Dm644 "${script_dir}/wireplumber-integration.conf" \
  "${config_home}/wireplumber/wireplumber.conf.d/99-stpw-integration.conf"
if [[ ${mode} == v2 ]]; then
  install -Dm644 \
    "${script_dir}/../wireplumber/wireplumber.conf.d/90-soundtouch-current-route-defaults.conf" \
    "${config_home}/wireplumber/wireplumber.conf.d/90-soundtouch-current-route-defaults.conf"
  install -Dm644 \
    "${script_dir}/../wireplumber/scripts/soundtouch/current-route-defaults.lua" \
    "${data_home}/wireplumber/scripts/soundtouch/current-route-defaults.lua"
fi
ln -s -- "$(realpath -- "${private_module}")" \
  "${module_dir}/libpipewire-module-soundtouch-raop-sink.so"
if [[ ${mode} == v1 ]]; then
  ln -s -- "$(realpath -- "${private_zone_module}")" \
    "${module_dir}/libpipewire-module-soundtouch-zone-sink.so"
fi

env \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  XDG_CONFIG_HOME="${config_home}" \
  PIPEWIRE_LOG="${server_log}" \
  pipewire -c "${script_dir}/pipewire-integration.conf" &
server_pid=$!

for _ in {1..100}; do
  if [[ -S ${runtime_dir}/stpw-integration ]]; then
    break
  fi
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    printf 'isolated PipeWire server exited before creating its socket\n' >&2
    sed -n '1,200p' "${server_log}" >&2
    exit 1
  fi
  sleep 0.05
done

if [[ ! -S ${runtime_dir}/stpw-integration ]]; then
  printf 'isolated PipeWire server did not become ready\n' >&2
  sed -n '1,200p' "${server_log}" >&2
  exit 1
fi

if ! timeout 5 env \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  XDG_CONFIG_HOME="${config_home}" \
  pw-cli -r stpw-integration info 0 >/dev/null; then
  printf 'isolated PipeWire core did not answer a readiness probe\n' >&2
  sed -n '1,200p' "${server_log}" >&2
  exit 1
fi

if [[ ${mode} == v2 ]]; then
v2_probe_common='{ raop.ip = "127.0.0.1" raop.port = "9" raop.name = "0200000000D2@Contract probe" raop.hostname = "contract-probe.local" raop.device = "0200000000D2" raop.transport = "udp" raop.encryption.type = "none" raop.audio.codec = "PCM" audio.channels = "2" audio.position = "[ FL FR ]" audio.rate = "44100" audio.format = "S16" remote.name = "stpw-integration" raop.latency.ms = "500" raop.dacp.enabled = "false" raop.volume.initial = "0" raop.volume.initial.mute = "true" stream.props = { node.name = "stpw_v2_contract_probe" }'

assert_v2_module_rejected() {
  local label=$1
  local module_args=$2
  local probe_output

  probe_output=$(env \
    XDG_RUNTIME_DIR="${runtime_dir}" \
    PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
    XDG_CONFIG_HOME="${config_home}" \
    PIPEWIRE_MODULE_DIR="${module_dir}:${system_module_dir}" \
    pw-cli -r stpw-integration load-module \
      libpipewire-module-soundtouch-raop-sink "${module_args}" 2>&1) || true
  # pw-cli 1.6.8 reports a rejected module in-band while still exiting 0.
  # Match that protocol instead of treating the process status as authority.
  if [[ ${probe_output} == *'Could not load module'* ]]; then
    printf 'private RAOP module correctly rejected %s\n' "${label}" >&2
    return
  fi
  printf 'private RAOP module unexpectedly accepted %s\n' "${label}" >&2
  printf '%s\n' "${probe_output}" >&2
  exit 1
}

assert_v2_module_rejected \
  'unknown raop.reconnect.mode' \
  "${v2_probe_common} raop.volume.control = \"external\" raop.volume.contract = \"2\" raop.reconnect.mode = \"unknown\" }"
assert_v2_module_rejected \
  'activation reconnect outside contract 2 + external control' \
  "${v2_probe_common} raop.volume.control = \"local\" raop.volume.contract = \"2\" raop.reconnect.mode = \"activation\" }"

if env \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  XDG_CONFIG_HOME="${config_home}" \
  PIPEWIRE_MODULE_DIR="${module_dir}:${system_module_dir}" \
  STPW_TEST_PIPEWIRE_REMOTE=stpw-integration \
  "${test_executable}" \
    -p /pipewire/direct-route-v2-export-integration; then
  :
else
  test_status=$?
  printf 'direct Route v2 exporter test failed; PipeWire log follows\n' >&2
  sed -n '1,240p' "${server_log}" >&2
  exit "${test_status}"
fi

fi

env \
  HOME="${private_home}" \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_REMOTE=stpw-integration \
  XDG_CONFIG_HOME="${config_home}" \
  XDG_CONFIG_DIRS="${config_dirs}" \
  XDG_DATA_HOME="${data_home}" \
  XDG_CACHE_HOME="${cache_home}" \
  XDG_STATE_HOME="${state_home}" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=${test_root}/no-session-bus" \
  WIREPLUMBER_DEBUG=3 \
  wireplumber -p stpw-smoke >"${wireplumber_log}" 2>&1 &
wireplumber_pid=$!

for _ in {1..100}; do
  if ! kill -0 "${wireplumber_pid}" 2>/dev/null; then
    printf 'WirePlumber exited before attaching to the isolated core\n' >&2
    sed -n '1,200p' "${wireplumber_log}" >&2
    exit 1
  fi
  if env XDG_RUNTIME_DIR="${runtime_dir}" PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
      pw-cli -r stpw-integration ls Client 2>/dev/null | \
      grep -Fq 'WirePlumber'; then
    break
  fi
  sleep 0.05
done

if ! env XDG_RUNTIME_DIR="${runtime_dir}" PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
    pw-cli -r stpw-integration ls Client 2>/dev/null | \
    grep -Fq 'WirePlumber'; then
  printf 'WirePlumber did not attach to the isolated core\n' >&2
  sed -n '1,200p' "${wireplumber_log}" >&2
  exit 1
fi

if [[ ${mode} == v2 ]]; then
env \
  HOME="${private_home}" \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_REMOTE=stpw-integration \
  XDG_CONFIG_HOME="${config_home}" \
  XDG_CONFIG_DIRS="${config_dirs}" \
  XDG_DATA_HOME="${data_home}" \
  XDG_CACHE_HOME="${cache_home}" \
  XDG_STATE_HOME="${state_home}" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=${test_root}/no-session-bus" \
  PULSE_RUNTIME_PATH="${pulse_runtime}" \
  PIPEWIRE_LOG="${pulse_log}" \
  pipewire-pulse -c "${script_dir}/pipewire-pulse-integration.conf" &
pulse_pid=$!

for _ in {1..100}; do
  if ! kill -0 "${pulse_pid}" 2>/dev/null; then
    printf 'isolated pipewire-pulse exited before becoming ready\n' >&2
    sed -n '1,200p' "${pulse_log}" >&2
    exit 1
  fi
  if PULSE_SERVER="unix:${pulse_runtime}/native" pactl info >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

if ! PULSE_SERVER="unix:${pulse_runtime}/native" pactl info >/dev/null 2>&1; then
  printf 'isolated pipewire-pulse did not become ready\n' >&2
  sed -n '1,200p' "${pulse_log}" >&2
  exit 1
fi

if env \
  HOME="${private_home}" \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  XDG_CONFIG_HOME="${config_home}" \
  XDG_CONFIG_DIRS="${config_dirs}" \
  XDG_DATA_HOME="${data_home}" \
  XDG_CACHE_HOME="${cache_home}" \
  XDG_STATE_HOME="${state_home}" \
  PIPEWIRE_MODULE_DIR="${module_dir}:${system_module_dir}" \
  STPW_TEST_PIPEWIRE_REMOTE=stpw-integration \
  STPW_TEST_PULSE_SERVER="unix:${pulse_runtime}/native" \
  "${direct_output_test}" \
    -p /direct-output-v2/runtime-wireplumber; then
  :
else
  test_status=$?
  printf 'direct output v2 WirePlumber test failed; PipeWire log follows\n' >&2
  sed -n '1,240p' "${server_log}" >&2
  printf 'WirePlumber log follows\n' >&2
  tail -n 240 "${wireplumber_log}" >&2
  exit "${test_status}"
fi

else

if env \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  XDG_CONFIG_HOME="${config_home}" \
  PIPEWIRE_MODULE_DIR="${module_dir}:${system_module_dir}" \
  STPW_TEST_PIPEWIRE_REMOTE=stpw-integration \
  "${test_executable}" \
    -p /pipewire/private-node-lifecycle-integration; then
  :
else
  test_status=$?
  printf 'private sink lifecycle test failed; PipeWire log follows\n' >&2
  sed -n '1,240p' "${server_log}" >&2
  printf 'WirePlumber log follows\n' >&2
  tail -n 240 "${wireplumber_log}" >&2
  exit "${test_status}"
fi

if env \
  XDG_RUNTIME_DIR="${runtime_dir}" \
  PIPEWIRE_RUNTIME_DIR="${runtime_dir}" \
  XDG_CONFIG_HOME="${config_home}" \
  PIPEWIRE_MODULE_DIR="${module_dir}:${system_module_dir}" \
  STPW_TEST_PIPEWIRE_REMOTE=stpw-integration \
  STPW_TEST_ZONE_LIFECYCLE=1 \
  "${test_executable}" \
    -p /pipewire/private-zone-lifecycle-integration; then
  :
else
  test_status=$?
  printf 'private zone sink lifecycle test failed; PipeWire log follows\n' >&2
  sed -n '1,240p' "${server_log}" >&2
  exit "${test_status}"
fi

fi
