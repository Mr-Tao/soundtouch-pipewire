#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 PRODUCTION-CLI TEST-EULA" >&2
  exit 64
fi

cli=$1
test_eula=$2
test_root=$(mktemp -d "${TMPDIR:-/tmp}/stpw-eula-production-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM
mkdir -m 700 "$test_root/runtime"

set +e
STPW_EULA_PATH="$test_eula" \
XDG_CONFIG_HOME="$test_root/config" \
XDG_RUNTIME_DIR="$test_root/runtime" \
  "$cli" eula show >"$test_root/stdout" 2>"$test_root/stderr"
status=$?
set -e

case $status in
  0 | 66) ;;
  *)
    cat "$test_root/stdout" "$test_root/stderr" >&2
    echo "unexpected production eula show exit: $status" >&2
    exit 1
    ;;
esac

if grep -Fq 'Test-only SoundTouch supplemental-terms fixture.' \
  "$test_root/stdout" "$test_root/stderr"; then
  echo 'production binary honored STPW_EULA_PATH' >&2
  exit 1
fi
