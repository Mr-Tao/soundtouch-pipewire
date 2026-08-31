#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(git -C "${script_dir}/../.." rev-parse --show-toplevel)
pkgbuild="${script_dir}/PKGBUILD"
eula="${repo_root}/licenses/EULA.txt"
project_license="${repo_root}/LICENSE"
third_party_notices="${repo_root}/licenses/THIRD-PARTY-NOTICES.txt"
rust_notices="${repo_root}/licenses/RUST-DEPENDENCIES.html"
clean_meson_test="${repo_root}/tools/run-meson-tests-clean-env.sh"
pkgbase=$(sed -n 's/^pkgbase=//p' "${pkgbuild}" | head -n1)
pkgver=$(sed -n 's/^pkgver=//p' "${pkgbuild}" | head -n1)
pkgrel=$(sed -n 's/^pkgrel=//p' "${pkgbuild}" | head -n1)
archive="${script_dir}/${pkgbase}-${pkgver}.tar.gz"
failed=0
srcinfo_tmp=$(mktemp -t soundtouch-pipewire-srcinfo.XXXXXX)
archive_tmp=$(mktemp -t soundtouch-pipewire-archive.XXXXXX.tar.gz)
archive_list_tmp=$(mktemp -t soundtouch-pipewire-archive-list.XXXXXX)
trap 'rm -f -- "${srcinfo_tmp}" "${archive_tmp}" "${archive_list_tmp}"' EXIT

fail() {
  printf 'release gate: %s\n' "$*" >&2
  failed=1
}

patch_paths_match_exactly() {
  local patch=$1

  shift
  cmp -s \
    <(awk '
      $1 == "diff" && $2 == "--git" {
        sub(/^a\//, "", $3)
        sub(/^b\//, "", $4)
        print $3
        print $4
      }
    ' "${patch}" | LC_ALL=C sort -u) \
    <(printf '%s\n' "$@" | LC_ALL=C sort -u)
}

grep -q "^_release_state='READY'$" "${pkgbuild}" ||
  fail 'PKGBUILD release state is not READY'

if [[ -n $(git -C "${repo_root}" status --porcelain --untracked-files=normal) ]]; then
  fail 'repository worktree is not clean'
fi

if [[ -z ${pkgbase} || -z ${pkgver} || -z ${pkgrel} ]]; then
  fail 'could not read pkgbase/pkgver/pkgrel from PKGBUILD'
fi

if grep -Eq 'DRAFT|NOT VALID FOR DISTRIBUTION|LEGAL .* REVIEW REQUIRED|\[\[|\]\]|PUBLIC-RELEASE-CONTACT-REQUIRED' \
  "${eula}"; then
  fail 'SoundTouch supplement still contains draft text or placeholders'
fi

grep -q '^Identifier: STPW-SOUNDTOUCH-SUPPLEMENT-1$' "${eula}" ||
  fail 'SoundTouch supplement identifier is missing or unexpected'

if [[ ! -f ${project_license} ]] ||
   ! grep -q '^MIT License$' "${project_license}"; then
  fail 'project MIT license is missing or unexpected'
fi

if grep -q 'RUST LICENSE INVENTORY REQUIRED BEFORE DISTRIBUTION' \
  "${third_party_notices}"; then
  fail 'Rust controller third-party license inventory is incomplete'
fi

if [[ ! -s ${rust_notices} ]] ||
   ! grep -q 'soundtouch-pipewire-control' "${rust_notices}" ||
   ! grep -q 'Unicode License v3' "${rust_notices}"; then
  fail 'Rust controller target/runtime license report is incomplete'
fi
if grep -Eq 'crates.io/crates/soundtouch-pipewire-control|&lt;year&gt;|&lt;copyright holders&gt;' \
  "${rust_notices}"; then
  fail 'Rust dependency report contains the project crate or a generic notice'
fi

if git -C "${repo_root}" grep -q \
     'SPDX-License-Identifier: LicenseRef-' -- \
     . ':(exclude)packaging/aur/check-release-gate.sh'; then
  fail 'custom identifier remains in a source-file SPDX header'
fi
grep -q "license: 'MIT'" "${repo_root}/meson.build" ||
  fail 'Meson project license is not MIT'
grep -q '^license = "MIT"$' "${repo_root}/control/Cargo.toml" ||
  fail 'Rust project license is not MIT'
grep -q '<project_license>MIT</project_license>' \
  "${repo_root}/control/data/io.github.Mr_Tao.SoundTouchPipeWire.Control.metainfo.xml" ||
  fail 'AppStream project license is not MIT'
grep -q '^publish = false$' "${repo_root}/control/Cargo.toml" ||
  fail 'Rust project must be private to the dependency-notice generator'
grep -q '^private = { ignore = true }$' "${repo_root}/control/about.toml" ||
  fail 'cargo-about must exclude the project crate from third-party notices'

daemon_license="license=('MIT AND LicenseRef-STPW-SoundTouch-Supplement-1')"
controller_license="license=('MIT AND Unicode-3.0 AND LicenseRef-STPW-SoundTouch-Supplement-1')"
if [[ $(grep -Fc "${daemon_license}" "${pkgbuild}") -ne 1 ]]; then
  fail 'daemon package license expression is missing or duplicated'
fi
if [[ $(grep -Fc "${controller_license}" "${pkgbuild}") -ne 2 ]]; then
  fail 'pkgbase/controller license expression is missing or duplicated'
fi

declare -A required_install_counts=(
  ["\"\${pkgbase}-\${pkgver}/LICENSE\""]=2
  ["\"\${pkgbase}-\${pkgver}/licenses/EULA.txt\""]=2
  ["\"\${pkgbase}-\${pkgver}/licenses/PIPEWIRE-MIT.txt\""]=1
  ["\"\${pkgbase}-\${pkgver}/licenses/RUST-DEPENDENCIES.html\""]=1
  ["\"\${pkgbase}-\${pkgver}/licenses/THIRD-PARTY-NOTICES.txt\""]=2
)
for installed_source in "${!required_install_counts[@]}"; do
  if [[ $(grep -Fc "${installed_source}" "${pkgbuild}") -ne \
        ${required_install_counts[${installed_source}]} ]]; then
    fail "unexpected package install count for ${installed_source}"
  fi
done

if [[ ! -x ${clean_meson_test} ]]; then
  fail 'clean Meson test helper is missing or not executable'
fi

if grep -q "'SKIP'" "${pkgbuild}"; then
  fail 'PKGBUILD still contains SKIP checksums'
fi

for required_file in \
  "${archive}" \
  "${script_dir}/0001-pipewire-raop-free-pending-messages.patch" \
  "${script_dir}/0002-pipewire-raop-safe-volume.patch" \
  "${script_dir}/0003-pipewire-raop-dacp-control.patch" \
  "${script_dir}/0004-pipewire-raop-safety-gate.patch" \
  "${script_dir}/0005-pipewire-soundtouch-zone-sink.patch" \
  "${script_dir}/0006-pipewire-raop-ownership-marker.patch" \
  "${script_dir}/0007-pipewire-raop-explicit-demand.patch" \
  "${script_dir}/0008-pipewire-raop-publish-marker-after-progress.patch" \
  "${script_dir}/0009-pipewire-raop-route-revision.patch" \
  "${script_dir}/0010-pipewire-raop-activation-reconnect.patch" \
  "${script_dir}/0011-pipewire-raop-contract2-props-normalizer.patch" \
  "${script_dir}/soundtouch-pipewire.install" \
  "${script_dir}/.SRCINFO"; do
  [[ -f ${required_file} ]] || fail "missing ${required_file}"
done

zone_patch="${script_dir}/0005-pipewire-soundtouch-zone-sink.patch"
if [[ -f ${zone_patch} ]] &&
   ! patch_paths_match_exactly \
     "${zone_patch}" \
       'src/modules/meson.build' \
       'src/modules/module-soundtouch-zone-sink.c' \
       'src/modules/module-soundtouch-zone/control.c' \
       'src/modules/module-soundtouch-zone/control.h' \
       'src/modules/module-soundtouch-zone/test-volume.c' \
       'src/modules/module-soundtouch-zone/volume.c' \
       'src/modules/module-soundtouch-zone/volume.h'; then
  fail '0005 patch is not isolated to the finalized seven zone paths'
fi

marker_patch="${script_dir}/0006-pipewire-raop-ownership-marker.patch"
if [[ -f ${marker_patch} ]] &&
   ! patch_paths_match_exactly \
     "${marker_patch}" \
       'src/modules/meson.build' \
       'src/modules/module-raop-sink.c' \
       'src/modules/module-raop/safety-gate.c' \
       'src/modules/module-raop/safety-gate.h' \
       'src/modules/module-raop/test-safety-gate.c' \
       'src/modules/module-raop/test-source-marker.c' \
       'src/modules/module-raop/test-volume.c' \
       'src/modules/module-raop/volume.c' \
       'src/modules/module-rtp/stream.c' \
       'src/modules/module-rtp/stream.h'; then
  fail '0006 patch is not isolated to the finalized ten source-marker, safety-gate, volume and RTP-stream paths'
fi

demand_patch="${script_dir}/0007-pipewire-raop-explicit-demand.patch"
if [[ -f ${demand_patch} ]] &&
   ! patch_paths_match_exactly \
     "${demand_patch}" \
       'src/modules/meson.build' \
       'src/modules/module-raop-sink.c' \
       'src/modules/module-raop/demand.c' \
       'src/modules/module-raop/demand.h' \
       'src/modules/module-raop/test-demand.c' \
       'src/modules/module-raop/test-volume.c' \
       'src/modules/module-raop/volume.c'; then
  fail '0007 patch is not isolated to the finalized seven explicit-demand and RAOP volume paths'
fi

marker_order_patch="${script_dir}/0008-pipewire-raop-publish-marker-after-progress.patch"
if [[ -f ${marker_order_patch} ]] &&
   ! patch_paths_match_exactly \
     "${marker_order_patch}" \
       'src/modules/module-raop-sink.c'; then
  fail '0008 patch is not isolated to the RAOP sink metadata ordering path'
fi

route_revision_patch="${script_dir}/0009-pipewire-raop-route-revision.patch"
if [[ -f ${route_revision_patch} ]] &&
   ! patch_paths_match_exactly \
     "${route_revision_patch}" \
       'src/modules/module-raop-sink.c' \
       'src/modules/module-raop/demand.h' \
       'src/modules/module-raop/safety-gate.c' \
       'src/modules/module-raop/safety-gate.h' \
       'src/modules/module-raop/test-safety-gate.c' \
       'src/modules/module-raop/test-volume.c' \
       'src/modules/module-raop/volume.c'; then
  fail '0009 patch is not isolated to the finalized seven Route revision, safety-gate and RAOP volume paths'
fi

activation_patch="${script_dir}/0010-pipewire-raop-activation-reconnect.patch"
if [[ -f ${activation_patch} ]] &&
   ! patch_paths_match_exactly \
     "${activation_patch}" \
       'src/modules/meson.build' \
       'src/modules/module-raop-sink.c' \
       'src/modules/module-raop/reconnect.c' \
       'src/modules/module-raop/reconnect.h' \
       'src/modules/module-raop/test-demand.c' \
       'src/modules/module-raop/test-reconnect.c'; then
  fail '0010 patch is not isolated to the activation reconnect paths'
fi

contract2_props_patch="${script_dir}/0011-pipewire-raop-contract2-props-normalizer.patch"
if [[ -f ${contract2_props_patch} ]] &&
   ! patch_paths_match_exactly \
     "${contract2_props_patch}" \
       'src/modules/module-raop-sink.c' \
       'src/modules/module-raop/test-volume.c' \
       'src/modules/module-raop/volume.c' \
       'src/modules/module-raop/volume.h'; then
  fail '0011 patch is not isolated to the contract-2 Props normalization paths'
fi

if [[ -f ${script_dir}/.SRCINFO ]]; then
  (
    cd -- "${script_dir}"
    makepkg --printsrcinfo > "${srcinfo_tmp}"
  )
  cmp -s "${script_dir}/.SRCINFO" "${srcinfo_tmp}" ||
    fail '.SRCINFO is not synchronized with PKGBUILD'

  grep -qx 'pkgname = soundtouch-pipewire' "${script_dir}/.SRCINFO" ||
    fail '.SRCINFO is missing the daemon package'
  grep -qx 'pkgname = soundtouch-pipewire-control' "${script_dir}/.SRCINFO" ||
    fail '.SRCINFO is missing the controller package'
  grep -qx $'\t'"depends = soundtouch-pipewire=${pkgver}-${pkgrel}" \
    "${script_dir}/.SRCINFO" ||
    fail 'controller dependency is not pinned to the daemon pkgver-pkgrel'
fi

if [[ -f ${archive} ]]; then
  git -C "${repo_root}" archive \
    --format=tar \
    --mtime='1970-01-01T00:00:00Z' \
    --prefix="${pkgbase}-${pkgver}/" \
    'HEAD^{tree}' |
    gzip -n > "${archive_tmp}"
  cmp -s "${archive}" "${archive_tmp}" ||
    fail 'source archive is not the canonical archive of the HEAD tree'

  if ! tar -tzf "${archive}" > "${archive_list_tmp}"; then
    fail 'source archive could not be listed'
  elif grep -Eq \
    "^${pkgbase}-${pkgver}/(\\.gitattributes|packaging)(/|$)" \
    "${archive_list_tmp}"; then
    fail 'source archive contains export-ignored packaging metadata'
  elif ! grep -qx \
    "${pkgbase}-${pkgver}/tools/run-meson-tests-clean-env.sh" \
    "${archive_list_tmp}"; then
    fail 'source archive is missing the clean Meson test helper'
  fi
fi

if ! grep -q "'SKIP'" "${pkgbuild}" &&
   [[ -f ${archive} &&
      -f ${script_dir}/0002-pipewire-raop-safe-volume.patch &&
      -f ${script_dir}/0003-pipewire-raop-dacp-control.patch &&
      -f ${script_dir}/0004-pipewire-raop-safety-gate.patch &&
      -f ${script_dir}/0005-pipewire-soundtouch-zone-sink.patch &&
      -f ${script_dir}/0006-pipewire-raop-ownership-marker.patch &&
      -f ${script_dir}/0007-pipewire-raop-explicit-demand.patch &&
      -f ${script_dir}/0008-pipewire-raop-publish-marker-after-progress.patch &&
      -f ${script_dir}/0009-pipewire-raop-route-revision.patch &&
      -f ${script_dir}/0010-pipewire-raop-activation-reconnect.patch &&
      -f ${script_dir}/0011-pipewire-raop-contract2-props-normalizer.patch ]]; then
  (
    cd -- "${script_dir}"
    makepkg --verifysource --skippgpcheck -f
  ) || fail 'makepkg source verification failed'
fi

if (( failed != 0 )); then
  exit 1
fi

printf 'release metadata gate passed; legal approval and AUR review remain manual gates\n'
