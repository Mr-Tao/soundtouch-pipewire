#!/bin/sh
set -u

expected=$1
shift

"$@"
result=$?
if [ "$result" -ne "$expected" ]; then
  echo "expected exit $expected, got $result" >&2
  exit 1
fi
