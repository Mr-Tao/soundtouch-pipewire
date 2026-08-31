---
name: soundtouch-pipewire-dev
description: Develop, diagnose, test, package, deploy, and recover the soundtouch-pipewire companion, its private PipeWire modules, and GTK controller. Use for this repository when investigating missing or misnamed Bose SoundTouch sinks, volume or mute races, topology and multiroom failures, PipeWire or WirePlumber hangs, Arch split-package builds, local service deployment, or upstream-ready changes.
---

# SoundTouch PipeWire Development

Work from observed receiver, PipeWire, service, and repository state. For new
direct-output implementation, use the minimal v2 contract. Treat the deployed
v1 fail-closed stack as a separate legacy diagnosis target rather than a source
of requirements for v2.

## Establish scope and state

1. Work in the repository root and inspect `git status --short`, the current
   branch, recent commits, and applicable `AGENTS.md` before editing.
2. Preserve unrelated dirty changes. Delegate edits only when file scopes are
   disjoint.
3. When diagnosing or deploying the live stack, inspect current user-unit
   state, recent journal entries, process CPU/RSS, `wpctl status -n`, and
   `pactl info` before restarting anything. Do not probe live state for a
   source-only implementation task.
4. Treat a sandbox-only process, D-Bus, ptrace, SSH, or systemd failure as
   unconfirmed host state. Repeat the necessary read-only check outside the
   sandbox with narrowly scoped approval.
5. Keep household identifiers, packet captures, cores, raw logs, credentials,
   and EULA records out of public commits and support artifacts.

## Diagnose audio failure

Classify the failure before changing state:

- If `wpctl`, `pactl`, or `pw-cli` hangs and PipeWire, WirePlumber, or
  pipewire-pulse consumes sustained CPU or growing RSS, inspect the companion
  and graph journal together. A companion-created module or link can survive
  the companion process until PipeWire is restarted.
- If only a SoundTouch sink is absent, inspect discovery, `/info` identity,
  live `/volume`, quarantine, and stock-RAOP exclusion. Do not restart the
  entire audio graph first.
- If a sink shows a model instead of the receiver name, compare `_raop._tcp`,
  `_soundtouch._tcp`, WAPI `/info`, and the published `node.description`.
  Preserve `device.model` as model metadata.
- If topology operations time out, treat HTTP acceptance as ambiguous and
  perform bounded fresh GET verification. Never issue cleanup against foreign
  or unreadable topology.

For a graph-wide runaway:

1. Capture exact PIDs, CPU/RSS, unit state, journal, and any available core.
2. Runtime-mask only `soundtouch-pipewire.service` so an automatic restart
   cannot republish the suspect graph.
3. Stop the exact PipeWire, WirePlumber, and pipewire-pulse user units. Kill
   only their confirmed lingering PIDs if normal stop does not finish.
4. Reset failed state and start the three standard audio units without the
   companion.
5. Confirm `wpctl` and `pactl` respond and an ordinary hardware sink works.
6. Leave the companion masked until a tested fix is installed. Unmask it only
   for a bounded canary while watching CPU, RSS, journal, and graph response.

Do not delete configuration, volume cache, presets, cores, or journal evidence
as part of recovery.

## Select the architecture generation

For new v2 direct-output work, read `docs/v2-direct-output-contract.md` and use
these invariants:

- Match RAOP identity to fresh `/info`, then obtain fresh `/volume` before
  publishing. Never initialize or write from cache, malformed input, or a
  default value.
- Use the standard Device Route as the sole public hardware-volume control and
  current-state interface. Publish WAPI `actualvolume`; a differing
  `targetvolume` is not an equality gate. Treat Node Props only as a
  client-facing mirror.
- Keep software gain at unity. A valid WirePlumber-restored Route may increase
  or decrease volume after the fresh baseline. WAPI is the sole hardware
  writer; the RAOP module must not emit RTSP volume.
- Serialize one WAPI operation and at most one latest queued Route request.
  Publish actual readback, including a quieter or otherwise different value.
- Follow every POST result with one GET. After an unknown result, permit no
  later POST before that readback. Reconnect and rediscovery may refresh state
  but must not replay the write.
- Retain the identified sink after a failed or unknown write, readback timeout,
  or event disconnect. Degrade control health only while fresh observation is
  unavailable; a valid differing readback is success. Do not quarantine,
  withdraw, poll, or autonomously correct it. Retry only on a named external
  event.
- Treat one mDNS REMOVE or browser/transport failure as transient. Withdraw
  only for a fresh contradictory identity, explicit policy removal, or
  shutdown.
- Do not import the v1 daemon, gate/source-marker/demand generations,
  topology, MPRIS, GUI operations, or compatibility scaffolding into the first
  slice.

For an explicitly named v1 maintenance or deployed-stack diagnosis, read
`docs/private-module-contract.md` and preserve its existing contract. Read
`docs/multiroom-control.md` only for an explicitly scoped zones, stereo-pairs,
D-Bus, operations, or controller task. Never use either v1 document to expand
a v2 ticket silently.

## Implement and test

1. Add the narrowest regression test that fails on the observed bug. For
   registry and lifecycle races, prefer the isolated PipeWire integration
   harness when a unit test cannot exercise callback ordering.
2. Keep model metadata, user-facing names, stable IDs, and transient
   publication IDs separate.
3. Defend invariants in the daemon or control service as well as the GUI. GUI
   sensitivity is not an authorization boundary.
4. Use `apply_patch` for source edits and run `git diff --check`.
5. Build the companion and optional controller:

   ```sh
   meson setup build-codex -Dcontrol_app=enabled
   meson compile -C build-codex
   ./tools/run-meson-tests-clean-env.sh build-codex
   ```

   Reconfigure an existing dedicated build directory instead of overwriting
   another agent's build tree. Never run `meson test` directly from a shell
   that carries credentials: Meson records its complete inherited environment
   before applying per-test environment overrides. The helper uses an
   allowlisted `env -i`, private temporary HOME and XDG directories, and
   `--no-rebuild`.
6. Run targeted tests while iterating, then the risk-proportional Meson suite.
   For v2, start with behavior tests for startup readback, Route roundtrip,
   receiver event refresh without echo, muted volume changes, retained sink on
   failure, and absence of autonomous retry. Run legacy RAOP or zone lifecycle
   tests only when the patch actually changes those v1 layers.
7. Run Rust formatting, tests, Clippy, translation, desktop, and AppStream
   checks through the normal Meson/package path for controller changes.

## Package and deploy an Arch canary

Read `packaging/aur/README.md` and the relevant parts of
`packaging/aur/RELEASE-CHECKLIST.md`. While `_release_state` is not `READY`,
the package is a local release candidate and must not be published to AUR.

1. Verify all private PipeWire patches are derived from the intended PipeWire
   version and that the installed PipeWire ABI version matches.
2. Prepare a reproducible committed source archive with
   `packaging/aur/prepare-local-source.sh`. Do not bypass its dirty-tree guard.
3. Build with the explicit local-candidate override (the historical variable
   name is retained for compatibility):

   ```sh
   SOUNDTOUCH_PIPEWIRE_ALLOW_DRAFT_BUILD=1 \
     makepkg -Csf --noconfirm --skippgpcheck --log
   ```

   The package `check()` function must invoke the source tree's clean Meson
   test helper. A clean chroot remains preferred for the complete package build
   because tools other than Meson may have their own logging behavior.

4. Inspect both split packages, their exact dependency, file lists, ownership,
   licenses, `provides`, `conflicts`, and `replaces` before installation.
5. Preserve the user's explicit SoundTouch supplement acceptance. A root
   package transaction must not accept it for a desktop user.
6. Install the daemon and matching controller revision together. Restart
   PipeWire only when the private-module or graph state requires it.

## Verify the live result

1. Start from an ordinary local sink and conservative receiver volume.
2. Run `soundtouch-pipewire doctor`, unmask the companion canary, and start it.
3. Watch service state, journal, CPU/RSS, `wpctl`, and `pactl` during discovery,
   sink publication, first stream connection, volume, mute, unmute, and
   teardown. For v2, verify actual WAPI readback, Route/OSD convergence,
   receiver-originated refresh without echo, and stable sink presence through
   a bounded control failure. Require source-marker/gate evidence only for an
   explicitly scoped v1 deployment.
4. Confirm the sink uses the WAPI receiver name while retaining correct model
   metadata and stable device identity.
5. Do not exercise zones during a v2 direct-output canary. For an explicitly
   scoped legacy zone task, verify its complete documented topology lifecycle.
6. After the observation window, confirm local audio still works and no
   process exhibits sustained CPU or RSS growth.
7. If any safety or liveness check fails, runtime-mask the companion again,
   recover the standard audio graph, preserve evidence, and report the exact
   unverified step.

Before a public commit, set repository-local contributor identity for the
upstream forge and verify it with `git config --get user.email`. Never change
global identity. Do not push or publish merely because local validation
passed.
