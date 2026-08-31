#!/usr/bin/env bash
set -euo pipefail

if (( $# < 1 )); then
  printf 'usage: %s BUILD-DIRECTORY [MESON-TEST-ARGUMENT ...]\n' "$0" >&2
  exit 64
fi

build_dir=$1
shift

if [[ ! -d ${build_dir} ]]; then
  printf 'Meson build directory does not exist: %s\n' "${build_dir}" >&2
  exit 66
fi

for command_name in env meson mkdir mktemp rm; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    printf 'missing required command: %s\n' "${command_name}" >&2
    exit 69
  fi
done

clean_root=$(mktemp -d /tmp/soundtouch-pipewire-meson-env.XXXXXX)
umask 077

# shellcheck disable=SC2329 # Invoked by the EXIT trap.
cleanup() {
  if [[ -n ${clean_root:-} && -d ${clean_root} ]]; then
    rm -rf -- "${clean_root}"
  fi
}
trap cleanup EXIT

mkdir -m 700 \
  "${clean_root}/home" \
  "${clean_root}/tmp" \
  "${clean_root}/cache" \
  "${clean_root}/config" \
  "${clean_root}/data" \
  "${clean_root}/state" \
  "${clean_root}/runtime"

test_status=0
env -i \
  HOME="${clean_root}/home" \
  PATH='/usr/local/sbin:/usr/local/bin:/usr/bin' \
  LANG='C.UTF-8' \
  LC_ALL='C.UTF-8' \
  TMPDIR="${clean_root}/tmp" \
  XDG_CACHE_HOME="${clean_root}/cache" \
  XDG_CONFIG_HOME="${clean_root}/config" \
  XDG_DATA_HOME="${clean_root}/data" \
  XDG_STATE_HOME="${clean_root}/state" \
  XDG_RUNTIME_DIR="${clean_root}/runtime" \
  meson test \
    -C "${build_dir}" \
    --no-rebuild \
    --print-errorlogs \
    "$@" ||
  test_status=$?

exit "${test_status}"
