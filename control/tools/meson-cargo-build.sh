#!/bin/sh
set -eu

cargo=$1
manifest=$2
target_dir=$3
output=$4
locale_dir=$5

export CARGO_TARGET_DIR="$target_dir"
export SOUNDTOUCH_PIPEWIRE_LOCALEDIR="$locale_dir"

"$cargo" build \
  --manifest-path "$manifest" \
  --locked \
  --release

built_binary="$target_dir/release/soundtouch-pipewire-control"
test -x "$built_binary"
cp "$built_binary" "$output"
chmod 0755 "$output"
