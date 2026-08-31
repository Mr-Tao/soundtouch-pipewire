# Public release checklist

This checklist records release decisions that are not already enforced by the
source tests or the authoritative v2 contracts. A checked command means it was
run against the exact release commit; an earlier build is supporting evidence,
not a substitute.

## License and provenance

- [x] License project-authored source, tests, documentation, controller, and
      packaging under MIT; add the root `LICENSE` and use `MIT` in Meson,
      Cargo, AppStream, and source SPDX headers.
- [x] Preserve PipeWire contributor notices and the PipeWire MIT text for the
      private modules and downstream patches.
- [x] Confirm from the complete local Git history that project-authored commits
      have one author, Lukáš Lipinský, under two private noreply identities.
- [x] Confirm no Bose code, documentation, firmware, logo, or SDK artifact is
      included.
- [x] Review Bose SoundTouch Web API document version 1.1, Terms effective
      2026-01-07, and record document SHA-256
      `294ddee5471bc74b8f692cfd36ecab8e2bbc8757408a50c2f25361af6f099d29`.
- [x] Put the nine Addendum A subjects in `licenses/EULA.txt`, scoped to use of
      the SoundTouch Connection and explicitly separate from MIT copyright
      permissions.
- [x] Record the release owner's 2026-08-31 decision to publish using the
      layered MIT plus per-user SoundTouch supplement model. The residual risk
      is that Bose defines "Application" broadly; the release must not be
      described as an unqualified pure-MIT application.
- [x] Replace every `PUBLIC-RELEASE-CONTACT-REQUIRED` marker with the public
      developer postal address, telephone number, and working contact email
      required by Addendum A.
- [x] Generate `licenses/RUST-DEPENDENCIES.html` from the exact locked
      `x86_64-unknown-linux-gnu` runtime graph using `control/about.toml` and
      `control/about.hbs`; exclude build-only and development-only crates.
- [x] Use `MIT AND LicenseRef-STPW-SoundTouch-Supplement-1` for the daemon and
      add `Unicode-3.0` for the statically linked controller; install all
      applicable license and notice files.

## Public source boundary

- [x] Replace household-specific fixture names in the proposed public tree;
      retain only synthetic locally administered MAC addresses, documentation
      ranges, and obviously artificial IDs.
- [x] Create a new single-commit public history from the reviewed final tree.
      Do not push the 203-commit private development history, which contains
      household identifiers in older revisions.
- [x] Verify the complete public tree contains no credentials, private network
      addresses, physical device IDs, generated logs, build products, or local
      acceptance records.
- [x] Verify repository-local Git identity is
      `Lukáš Lipinský <6032558+Mr-Tao@users.noreply.github.com>` before the
      public commit and tag.

## Runtime acceptance boundary

- [x] Keep `eula show` and `eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1` as the
      explicit per-user acceptance seam; package installation never accepts on
      a user's behalf.
- [x] Use the supplement version and exact SHA-256 in the private mode-0600
      acceptance record; a changed text or version requires fresh acceptance.
- [x] Remove the production `STPW_EULA_PATH` environment override. The fixture
      path exists only in a separately compiled, non-installed test executable.
- [x] Run focused tests proving that absent, stale, malformed, unsafe-mode, or
      wrong-digest acceptance fails before discovery, WAPI access, or PipeWire
      publication, while valid acceptance permits the runtime to continue to
      configuration validation.

## Source and package validation

- [x] Treat `docs/v2-direct-output-contract.md` and
      `docs/v2-service-contract.md` as the canonical behavioral criteria rather
      than duplicating their state-machine matrix here.
- [x] Record the accepted pre-licensing implementation baseline `0711976`, for
      which the complete source suite, isolated PipeWire/WirePlumber tests,
      clean Arch pkgrel-63 build, four-receiver digital-silence canary, timeout
      retention, and authoritative Route/WAPI readback were independently
      reviewed as passing.
- [x] Run `git diff --check` and inspect every changed path.
- [x] Configure a fresh out-of-tree Meson build and run
      `./tools/run-meson-tests-clean-env.sh BUILD`.
- [x] Run Cargo format, tests, and Clippy with the locked graph.
- [x] Regenerate the cargo-about report with `--frozen --fail` and require an
      empty diff.
- [x] Apply patches 0001 through 0011 to the checksum-verified PipeWire 1.6.8
      archive and run all seven private PipeWire module tests.
- [x] Run the complete split-package build in a clean Arch container with
      credentials and desktop state absent.
- [x] Inspect all package metadata and file lists, including debug build-ID
      links, exact PipeWire dependencies, empty provides/conflicts/replaces,
      no build-tree RPATH, no overlap with official PipeWire packages, and the
      expected license files in each split package.
- [x] Run `namcap` on the PKGBUILD and all generated packages; review every
      warning rather than treating `namcap` as an automatic verdict.

The 2026-08-31 `namcap` review retained the daemon's runtime-only PipeWire,
WirePlumber, and systemd dependencies and the controller's exact daemon
dependency. The loader-library warning and empty generated debug-source
directory do not identify a missing runtime dependency or payload collision.

## Publication

- [x] Replace `_release_state` with `READY` only in the final reviewed release
      commit.
- [x] Run `packaging/aur/prepare-local-source.sh`; review the deterministic
      source archive, updated checksum, and regenerated `.SRCINFO`.
- [x] Run `packaging/aur/check-release-gate.sh` successfully on a clean tree.
- [x] Create `Mr-Tao/soundtouch-pipewire` from the sanitized history, push the
      release commit and signed `v0.1.0` tag, and upload the exact generated
      source archive as the release asset referenced by the PKGBUILD.
- [x] Create the separate AUR `soundtouch-pipewire` packagebase from only the
      reviewed packaging files, root MIT license, and required patch notices;
      verify its source URL before pushing.
- [x] Verify the public GitHub release asset checksum from a fresh download and
      run `makepkg --verifysource --skippgpcheck` from the final AUR tree.
