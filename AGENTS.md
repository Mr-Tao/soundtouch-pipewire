# Repository guidance

## Codex orchestration for substantial changes

Use the full multi-agent workflow for durable schemas, migrations, concurrency,
IPC, security boundaries, public APIs, and uncertain state machines. Do not
force it onto trivial edits or simple questions.

- Use a read-only explorer when the execution path or integration seam is not
  already established.
- Require an architecture review that freezes invariants, adversarial negative
  cases, and exit criteria before implementation.
- Assign exactly one source-editing agent to each patch. Explorers and
  reviewers stay read-only in the source session.
- Freeze the diff after focused tests, then require a final read-only review of
  the actual diff and evidence.
- Run executable independent validation in a separate session rooted at a
  fresh disposable copy. Verify that the original checkout is outside that
  session's writable roots; child sandbox defaults alone are not proof.
- The parent resolves disagreements and owns commit or continuation decisions.
- Keep project configuration free of authentication, providers, credentials,
  proxies, and machine-managed policy.

## Architecture generations

- `docs/v2-direct-output-contract.md` is authoritative for new v2 direct
  output work. The current `daemon.c`, its private gate/source-marker/demand
  protocol, topology coordinator, and v1 contract documents are frozen
  references, not invariants to reproduce in v2.
- Maintain v1 only when a ticket explicitly names it. Do not import v1
  quarantine, fault-recovery, topology, source-marker, route-revision, or
  multi-generation proof machinery into a v2 ticket without a new design gate
  demonstrating that the first v2 acceptance criteria cannot be met without
  it.
- Prefer the smallest vertical slice and behavior-based tests. If a proposed
  v2 change adds a second authority, autonomous corrective loop, or speculative
  compatibility layer, stop and simplify the design before implementation.

## SoundTouch v2 boundaries

- Treat the matching RAOP identity and fresh `/info` `deviceID` as receiver
  identity. Hostnames, cached state, sparse PipeWire announcements, and desktop
  mixer state are not identity authority.
- Treat fresh WAPI `/volume` readback as hardware observation and the standard
  Device Route as the sole public hardware-volume control/current-state
  interface. Publish `actualvolume`; a differing `targetvolume` is not an
  equality gate. Node Props may mirror Route but are not a second persisted
  authority.
- Keep numeric software gain at unity. Never initialize or write hardware from
  a missing, cached, malformed, or default volume. A valid WirePlumber-restored
  Route may increase or decrease volume after a fresh receiver baseline. WAPI
  is the only hardware writer; the RAOP module must not emit RTSP volume.
- Serialize one receiver operation and retain at most one latest queued Route
  request. Publish actual readback even when it differs from the request; do
  not replay uncertain writes or run autonomous correction loops. Follow every
  POST result with one GET; an unknown write permits no later POST before that
  readback.
- Retain the Device and sink after a failed or unknown write, readback timeout,
  or event disconnect. Degrade control health only while a fresh observation
  is unavailable; a valid differing readback is success. Retry only after a
  named external event. A single mDNS REMOVE or browser/transport failure is
  not disappearance. Withdraw only for a fresh contradictory identity,
  explicit policy removal, or shutdown.
- Treat changes to public PipeWire objects, D-Bus schemas, packaging, or the v2
  authority/lifecycle rules as architecture sensitive and keep their focused
  tests and contract documentation synchronized.
- Custom agents must not probe household speakers, WAPI, mDNS, RAOP, live
  PipeWire state, user services, or installed packages. The parent may do so
  only when the task explicitly authorizes live operations and must verify the
  physical receiver state, not merely a desktop value. Playback tests default
  to actual hardware volume zero unless the user explicitly permits otherwise.

## Validation and release

- Never run plain `meson test`. Use
  `./tools/run-meson-tests-clean-env.sh BUILD` so Meson does not record session
  credentials or user state.
- Keep builds and executable independent validation outside the source
  checkout when acting as `test_runner`; use a fresh disposable copy whose
  writable roots exclude this repository.
- Do not install packages, restart services, publish, push, change
  `_release_state`, or bypass legal/provenance gates unless the parent task
  explicitly authorizes that scope.
- Preserve unrelated user changes and ignored local build/package artifacts.
