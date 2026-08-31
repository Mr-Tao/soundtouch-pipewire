#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

unit=$1

test "$(grep -Fxc 'ExecStart=/usr/bin/soundtouch-pipewire service-v2' "$unit")" -eq 1
test "$(grep -Fc 'ExecStart=' "$unit")" -eq 1
test "$(grep -Fxc 'RestartPreventExitStatus=64 75 78' "$unit")" -eq 1
if grep -Fq 'ExecStart=/usr/bin/soundtouch-pipewire daemon' "$unit"; then
  echo "legacy daemon remains enabled in packaged service" >&2
  exit 1
fi
