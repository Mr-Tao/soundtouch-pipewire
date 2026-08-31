#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(git -C "${script_dir}/../.." rev-parse --show-toplevel)
pkgbuild="${script_dir}/PKGBUILD"

for command_name in git gzip makepkg sha256sum updpkgsums; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    printf 'missing required command: %s\n' "${command_name}" >&2
    exit 1
  fi
done

pkgbase=$(sed -n 's/^pkgbase=//p' "${pkgbuild}" | head -n1)
pkgver=$(sed -n 's/^pkgver=//p' "${pkgbuild}" | head -n1)

if [[ -z ${pkgbase} || -z ${pkgver} ]]; then
  printf 'could not read pkgbase/pkgver from %s\n' "${pkgbuild}" >&2
  exit 1
fi

for required_file in \
  LICENSE \
  meson.build \
  meson_options.txt \
  src/main.c \
  src/daemon.c \
  tools/run-meson-tests-clean-env.sh \
  licenses/EULA.txt \
  licenses/RUST-DEPENDENCIES.html; do
  if [[ ! -f ${repo_root}/${required_file} ]]; then
    printf 'repository is missing implementation file: %s\n' \
      "${required_file}" >&2
    exit 1
  fi
done

if [[ ! -x ${repo_root}/tools/run-meson-tests-clean-env.sh ]]; then
  printf 'clean Meson test helper is not executable\n' >&2
  exit 1
fi

project_version=$(
  sed -n "s/^[[:space:]]*version:[[:space:]]*'\\([^']*\\)'.*/\\1/p" \
    "${repo_root}/meson.build" |
    head -n1
)
if [[ ${project_version} != "${pkgver}" ]]; then
  printf 'Meson project version %s does not match PKGBUILD pkgver %s\n' \
    "${project_version:-<missing>}" "${pkgver}" >&2
  exit 1
fi

if [[ -n $(git -C "${repo_root}" status --porcelain --untracked-files=normal) ]]; then
  printf 'the worktree must be clean before creating a source archive\n' >&2
  exit 1
fi

archive="${script_dir}/${pkgbase}-${pkgver}.tar.gz"
archive_tmp=$(mktemp "${script_dir}/.${pkgbase}-${pkgver}.XXXXXX.tar.gz")
trap 'rm -f -- "${archive_tmp}"' EXIT

# Packaging metadata is export-ignored. Archive the tree with a fixed mtime so
# a packaging-only commit changes neither tar contents nor commit metadata.
git -C "${repo_root}" archive \
  --format=tar \
  --mtime='1970-01-01T00:00:00Z' \
  --prefix="${pkgbase}-${pkgver}/" \
  'HEAD^{tree}' |
  gzip -n > "${archive_tmp}"
mv -- "${archive_tmp}" "${archive}"
trap - EXIT

(
  cd -- "${script_dir}"
  updpkgsums
  makepkg --printsrcinfo > .SRCINFO
)

printf 'created %s\n' "${archive}"
sha256sum "${archive}"
printf 'updated PKGBUILD checksums and .SRCINFO; review both before building\n'
