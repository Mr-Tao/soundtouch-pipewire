#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 TEST-CLI" >&2
  exit 64
fi

cli=$1
test_root=$(mktemp -d "${TMPDIR:-/tmp}/stpw-eula-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM
mkdir -m 700 "$test_root/runtime"
export XDG_CONFIG_HOME="$test_root/config"
export XDG_RUNTIME_DIR="$test_root/runtime"
record="$XDG_CONFIG_HOME/soundtouch-pipewire/eula-acceptance"

expect_exit() {
  expected=$1
  needle=$2
  shift 2
  set +e
  "$@" >"$test_root/stdout" 2>"$test_root/stderr"
  actual=$?
  set -e
  if [ "$actual" -ne "$expected" ]; then
    cat "$test_root/stdout" "$test_root/stderr" >&2
    echo "expected exit $expected, got $actual" >&2
    exit 1
  fi
  if ! grep -Fq "$needle" "$test_root/stderr"; then
    cat "$test_root/stdout" "$test_root/stderr" >&2
    echo "missing expected diagnostic: $needle" >&2
    exit 1
  fi
}

expect_exit 78 "Cannot inspect EULA acceptance record" "$cli" daemon
expect_exit 78 "Cannot inspect EULA acceptance record" "$cli" service-v2
expect_exit 78 "Cannot inspect EULA acceptance record" \
  "$cli" direct-v2 --device 020000000001

"$cli" eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1 >"$test_root/accept"
test "$(stat -c '%a' "$record")" = 600

expect_exit 78 "Failed to open file" \
  "$cli" direct-v2 --device 020000000001

chmod 0644 "$record"
expect_exit 78 "accessible only to that user" \
  "$cli" direct-v2 --device 020000000001

chmod 0600 "$record"
printf '%s\n' \
  '[acceptance]' \
  'version=STPW-SOUNDTOUCH-SUPPLEMENT-1' >"$record"
expect_exit 78 "sha256" "$cli" direct-v2 --device 020000000001

"$cli" eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1 >"$test_root/reaccept"
sed -i 's/^version=.*/version=NOT-THE-SUPPLEMENT/' "$record"
expect_exit 78 "supplemental terms have not been accepted" \
  "$cli" direct-v2 --device 020000000001

"$cli" eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1 >"$test_root/reaccept"
sed -i 's/^sha256=.*/sha256=0000000000000000000000000000000000000000000000000000000000000000/' \
  "$record"
expect_exit 78 "supplemental terms have not been accepted" \
  "$cli" direct-v2 --device 020000000001

"$cli" eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1 >"$test_root/reaccept"
mv "$record" "$record.real"
ln -s "$record.real" "$record"
expect_exit 78 "not a regular file" \
  "$cli" direct-v2 --device 020000000001
