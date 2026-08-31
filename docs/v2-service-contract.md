# Persistent multi-receiver v2 service contract

Status: implementation and production-cutover gate.

This contract extends the per-output behavior in
[`v2-direct-output-contract.md`](v2-direct-output-contract.md) to the installed
long-running service. It does not change the volume, mute, Route, identity, or
RAOP transport authority defined there.

## Ownership boundary

One `service-v2` process owns:

- one process instance lock shared with the frozen v1 daemon and the manual
  `direct-v2` canary;
- one configuration snapshot loaded at startup;
- one Avahi discovery client;
- one PipeWire context and thread loop; and
- a map from normalized receiver MAC to one independent direct-v2 bootstrap
  and output actor.

Each receiver actor owns only that receiver's identity verification, WAPI
serialization, volume state, Device, Route, Node, and RAOP session. There is no
shared volume queue and no cross-receiver corrective or topology authority.

The installed systemd unit starts `service-v2` only. The legacy `daemon`
command remains a frozen rollback/debug entry point, cannot run concurrently
because of the shared lock, and is not an automatic fallback.

## Admission and configuration

Configuration is immutable for one service lifetime. A receiver is admitted
when its normalized MAC has `enabled=true`, or when it is `auto` and
`manage-all-verified=true`. An explicit `enabled=false` always wins. The
configured `raop-latency-ms` is copied into the actor when it is created.

An unavailable discovery event never creates an actor. A failed actor
construction may be retried only by a later external discovery event. Policy
changes take effect after an explicit service restart; there is no dynamic
configuration reload in this slice.

## Discovery and publication lifecycle

Before publication, incomplete discovery, REMOVE, timeout, failed `/info`, or
failed `/volume` cancels the candidate and publishes nothing. A later fresh
announcement may start a new candidate.

After publication:

- any uncorroborated mDNS REMOVE, resolver timeout, incomplete endpoint, RAOP
  failure, or WAPI failure retains the exact Device, Route, and Node;
- repeated unavailable events are idempotent and do not accumulate into proof
  of disappearance;
- a same-endpoint recovery is routed only to that receiver actor and may start
  only the GET-only recovery allowed by the per-output contract;
- a changed endpoint is not adopted and does not withdraw the output; an
  explicit service restart is required to validate and adopt it; and
- every network endpoint identity observed for an actor is fenced to that MAC
  for the process lifetime. Because WAPI and RAOP use unscoped IPv4 sockets, a
  different MAC advertised at the same IP cannot create a second actor or take
  over authority even if Avahi reports another interface; a restart is required
  for fresh identity validation; and
- local PipeWire object loss may recreate only that receiver's local objects
  after a fresh receiver announcement. It cannot alter a sibling actor.

A fatal Avahi client failure first retains all receiver actors and public
outputs, then restarts discovery on a fixed three-second cadence. A successful
restart reconciles only from fresh announcements. It never replays a volume
write. Failure to start discovery during initial service startup is fatal and
is left to systemd's ordinary restart policy.

A connection loss on the one shared PipeWire core is a service-level failure,
not a set of unrelated receiver failures. It terminates `service-v2` with
`EX_UNAVAILABLE`; systemd then creates a fresh core, discovery generation, and
actor set. Loss of one receiver's module or Node remains actor-local and cannot
restart or withdraw a sibling.

Withdrawal is permitted only during service shutdown in this slice. Dynamic
policy removal and post-publication contradictory-identity probing are not
implemented. Shutdown stops discovery first, then destroys all receiver actors,
then the shared PipeWire context, and performs no hardware write.

## User-facing control surface

The standard PipeWire Device Route and its Node mirror are the only supported
volume/mute control surface. WirePlumber remains responsible for normal Route
selection and persistence. A separately versioned, read-only desktop status
mirror and a Route-only GTK client are permitted only under the
[`v2 desktop indicator contract`](v2-indicator-contract.md); neither is a
second control or confirmed-state authority.

The v1 D-Bus control API, GTK controller operations, MPRIS router, SoundTouch
zones, stereo-pair orchestration, and virtual-zone module are not connected to
`service-v2`. Their code and optional package remain frozen references; a GUI
warning that the legacy daemon is absent is not evidence that `service-v2`
failed. They must not be silently reintroduced as a second authority.

After acquiring the shared instance lock, `service-v2` removes the frozen v1
runtime-status file. No v1 process can still own it at that point. The legacy
`status` command consequently reports unavailable rather than misrepresenting
stale v1 JSON as current v2 state. V2 status and GUI behavior are defined
separately by the
[`v2 desktop indicator contract`](v2-indicator-contract.md).

## Acceptance criteria

Production cutover requires all of the following:

1. Two admitted receivers create two actors with unique Device, Route, and Node
   objects, using one shared PipeWire context and zero startup writes.
2. A timeout or unavailable event for receiver A removes no public object,
   preserves A and B, and performs no WAPI operation solely because of the
   timeout.
3. RAOP REMOVE, WAPI REMOVE, and their repetition remain idempotent.
4. Same-endpoint recovery for A affects only A; when degraded it performs at
   most the GET-only recovery allowed by the per-output contract, with no
   autonomous POST.
5. A stale resolver completion cannot revive an invalidated candidate.
6. A changed endpoint for A is neither adopted nor allowed to withdraw A or B.
7. If endpoint identity already retained for A is advertised as a different
   MAC, no second actor, public output, `/info`, `/volume`, or write is created;
   A and unrelated siblings remain unchanged until explicit restart.
8. Initial `/info` mismatch publishes nothing and never requests `/volume`.
9. Concurrent Route requests serialize independently per receiver.
10. Local object loss and later rediscovery recreate only the affected receiver;
   sibling public identities remain stable.
11. Shared PipeWire core loss exits `EX_UNAVAILABLE`; an actor-local module or
    Node loss does not terminate the service or alter a sibling.
12. Shutdown withdraws each public object once and performs no WAPI write.
13. The packaged unit executes only `service-v2`; shared locking excludes v1;
    missing supplemental-terms acceptance, invalid configuration, and a lock
    conflict do not restart-loop.
14. All source, isolated PipeWire/WirePlumber, package, and ABI tests pass in a
    clean environment.
15. A live digital-silence canary proves every admitted local receiver, stable
    public objects across an observed timeout, independent Route control, and
    authoritative delayed WAPI readback.

## Rollback

Stop `service-v2`, verify that its process, lock, and public objects are gone,
restore the previous package and unit, then start the frozen v1 daemon. Verify
receiver state and sink availability after rollback. Never start v1 beside an
active v2 process.
