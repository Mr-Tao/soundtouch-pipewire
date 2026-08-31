# Public package acceptance contract

Status: operational gate for installing a published package on a real desktop.

This contract sits above the direct-output and persistent-service contracts. It
does not redefine receiver identity, volume authority, recovery, or PipeWire
object lifetime. It answers the narrower question: did the package built from
the public distribution replace the preceding known-good package safely and
remain usable in an ordinary user session?

## Authority and boundaries

- The running `service-v2` process, standard PipeWire Device Route, and fresh
  SoundTouch `/info` and `/volume` responses are authoritative.
- Package-manager success, `doctor`, a visible sink, desktop OSD, HTTP success,
  or the frozen v1 controller is supporting evidence only.
- Zones, stereo pairs, MPRIS, v1 D-Bus operations, dynamic configuration reload,
  endpoint migration, and latency tuning are outside this gate.
- Begin with direct inspection and ordinary commands. Add capture or focused
  diagnostics only after one concrete failure supplies a hypothesis. This gate
  requires no dedicated harness.

## Published input and rollback

Before changing the host:

1. Freeze the public AUR commit, release tag, source-archive checksum, package
   version, and exact PipeWire/WirePlumber dependency versions.
2. Build the public AUR checkout in a clean Arch environment with its normal
   `check()` function enabled. Verify `.SRCINFO` and every downloaded source.
3. Retain a complete, installable copy of the currently installed split-package
   set. If it is not cached, rebuild or reconstruct it before continuing.
4. Record the installed versions, service state, and current user-manager
   invocation. Hash the per-user configuration and acceptance record for local
   comparison. Publish only whether they changed and retained private modes,
   not their contents or hashes.

Failure before a complete target build and practical rollback copy changes
nothing on the host.

## Bounded transition

1. Stop the companion and confirm that its process and public objects are gone.
2. Upgrade all already installed exact-revision split packages in one package
   transaction. Do not add optional packages as part of this gate.
3. Preserve the existing user configuration. Show the installed supplemental
   terms and record acceptance explicitly; package installation never accepts
   on a user's behalf.
4. Reload the user service manager, restart PipeWire, pipewire-pulse, and
   WirePlumber, then run `doctor` while the companion remains stopped.
5. Do not reapply first-start discovery migration automatically. Investigate a
   failed preflight before making another configuration change.
6. Start the companion once. A restart loop or mixed old/new package set fails
   the transition.

## Runtime acceptance

Preserve each receiver's initial physical volume and mute tuple. Playback uses
a generated digital-silence stream and confirmed hardware volume zero unless
the user explicitly authorizes audible content.

The package is accepted when:

- the expected admitted receivers publish distinct Device, Route, and Node
  objects without stock SoundTouch duplicates;
- each receiver obtains fresh identity and volume state, opens a silent RAOP
  session, and remains present while outputs are switched;
- basic Route volume and mute work for each receiver and fresh `/volume`
  readback becomes the visible state;
- on one representative receiver, a PipeWire-native slider, a
  PulseAudio-compatible desktop control, muted volume adjustment, and a
  receiver-originated change all converge to physical readback;
- a valid readback which differs from a requested value is accepted rather than
  corrected, retried, or used to withdraw the sink; and
- after enabling the service, an actual fresh login associated with a new
  user-manager invocation starts it once with `NRestarts=0` and restores the
  expected sinks. Restarting the manager inside the same login is not
  sufficient evidence.

Always repeat startup, silent playback, and Route/WAPI smoke tests. More invasive
fault scenarios may reuse prior evidence when review of the intervening changes
shows that their subsystem and dependencies are unchanged. Diagnose a failure
with the smallest focused log, readback, or capture that can distinguish the
current hypothesis.

Restore every receiver's initial volume and mute tuple after the test.

## Failure and rollback

An uncertain hardware write is governed by the v2 transaction contract and is
never replayed by this procedure. If the new package cannot establish a known
running state:

1. Stop it and confirm that its process and public objects are gone.
2. Reinstall the complete preceding split-package set in one transaction,
   verify its exact installed versions and retained artifact checksums, and
   reload the user service manager. Verify that the loaded unit and its
   executable path belong to that reinstalled set.
3. If the installed acceptance record does not match the reinstalled package's
   supplement, show those preceding terms and require that user to accept them
   again before starting the rolled-back service. Do not restore or publish the
   contents of an older acceptance record.
4. With the companion still stopped, restart PipeWire, pipewire-pulse, and
   WirePlumber and require a successful `doctor` preflight.
5. Restore the preceding enablement state. If the service was running before
   the transition, start `service-v2` at most once and verify that its new
   process start follows the rollback transaction, the expected objects return,
   and physical readback is fresh. If it was stopped, leave it stopped and
   verify that no companion process or public objects remain.

Rollback restores the preceding `service-v2` package and the service enablement
and running state observed before the transition. It does not automatically
start v1 or restore stock RAOP discovery. Those are separate, explicitly
authorized recovery operations.

## Evidence record

Each release has one short Markdown record containing public input revisions,
artifact checksums, dependency and package versions, the result of the required
smoke tests, the fresh-login result, and the final outcome: `accepted`,
`blocked`, or `rolled-back`.

Use generic receiver labels. Do not commit household or receiver names,
operational-site addresses, device identifiers, raw logs, packet captures,
configuration contents, acceptance contents, or credentials. This evidence
restriction does not redact developer contact details deliberately published in
the project's legal notices. A missing physical result stays visibly
incomplete; deployment alone is not acceptance.
