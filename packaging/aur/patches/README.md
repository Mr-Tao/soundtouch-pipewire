# PipeWire patch inputs

This package builds a uniquely named private RAOP module from PipeWire 1.6.8.
It must never install over `/usr/lib/pipewire-0.3/libpipewire-module-raop-sink.so`.

Required files:

- `0001-pipewire-raop-free-pending-messages.patch` is the upstream prerequisite
  from PipeWire commit `4cf5acf18d3641dbdfc8e3c69cf4f47c0ff285ac`.
- `0002-pipewire-raop-safe-volume.patch` is exported from the validated
  PipeWire 1.6.8 backport range `894a1a9e5..39ef775a5`.
- `0003-pipewire-raop-dacp-control.patch` is the exact diff from
  `39ef775a5`, the endpoint of patch 0002, to the validated PipeWire 1.6.8 DACP
  working tree. It adds the authenticated DACP listener and advertisement,
  callback properties, bounded RTSP reconnect handling, and its unit tests.
- `0004-pipewire-raop-safety-gate.patch` is the exact delta above the
  post-0003 tree to the validated contract-3 working tree. It adds the opt-in
  sequenced transport safety gate, strict gate-token release command, minimal
  RTP command forwarding, and its unit tests.
- `0005-pipewire-soundtouch-zone-sink.patch` is the exact delta above the
  post-0004 tree. It touches only `src/modules/meson.build` and the six
  `module-soundtouch-zone*` implementation/test paths. It adds the private
  inactive virtual sink, explicitly armed module-owned passive playback
  stream, core-derived passive transport route, strict unity software-gain
  contract, sequenced nonce-bound arm/disarm command, and unit tests.
- `0006-pipewire-raop-ownership-marker.patch` is the exact delta above the
  post-0005 tree and is isolated to ten source-marker, safety-gate, volume and
  RTP stream paths. It adds the random receiver-visible source marker,
  rotation command, active-session gate rebinding, and a release command bound
  to the exact confirmed marker sequence and value plus a module-enforced
  absolute `CLOCK_BOOTTIME` proof deadline. Its final deadline validation,
  atomic gate opening and close barriers run while serialized under the RTP
  data-loop lock. Contract 3 rejects `aes67.driver-group`, whose separate
  sender loop would bypass that boundary.
- `0007-pipewire-raop-explicit-demand.patch` is the exact delta above the
  post-0006 tree and is isolated to the RAOP module, its Meson target, the
  explicit-demand parser/state machine and the affected volume tests. It adds
  explicit-demand contract, whose nonce-bound successor command makes controller
  demand a necessary precondition for RTSP activation. An idle acknowledgement
  is published only after the packet gate is closed, reconnect authority is
  revoked, and pending pre-SETUP work is synchronously cancelled or a ready
  session has begun teardown.
- `0008-pipewire-raop-publish-marker-after-progress.patch` is the focused
  delta above the post-0007 tree. It makes the receiver-visible random marker
  the final initial metadata write, after the acknowledged RAOP progress
  update, because SoundTouch firmware can otherwise retain either update
  nondeterministically even when both RTSP requests return 200.
- `0009-pipewire-raop-route-revision.patch` adds contract 5's paired Device
  Route revision, strict hold-and-adopt command, publication/demand-bound
  release fencing, a bounded pre-Route Props quarantine, preserves the
  single-data-loop AES67 exclusion, and adds focused parser/state-machine and
  volume-contract tests.

The nine patch files live one level above this README because AUR source files
must be present at the package repository root.

When refreshing patches 0002 through 0009:

1. Export it from the corresponding validated 1.6.8 backport; do not export a
   diff against current master and assume it applies to 1.6.8.
2. Apply patches 0001 through 0009 in order to a fresh upstream 1.6.8
   archive with `patch -Np1`.
3. Configure with Avahi and tests enabled, then build
   `pipewire-module-raop-sink`, `pipewire-module-soundtouch-zone-sink`,
   `pw-test-raop-volume`, `pw-test-raop-dacp`,
   `pw-test-raop-demand`, `pw-test-raop-safety-gate`,
   `pw-test-raop-source-marker`, and
   `pw-test-soundtouch-zone-volume`.
4. Run all six module tests and the isolated companion lifecycle test with
   both modules staged. Confirm the RAOP external-volume, authenticated DACP
   and contract-5 Route/demand/safety-gate contracts as well as the zone
   volume and exact arm/disarm contract. Run the Route policy portion with
   WirePlumber 0.5.15 or newer and confirm a saved tuple returns with
   `save=false` after Device removal/republication in the same process.
5. Confirm both packaged modules have no build-tree RPATH.
6. Update the refreshed patch checksum in `PKGBUILD` and regenerate
   `.SRCINFO`.
