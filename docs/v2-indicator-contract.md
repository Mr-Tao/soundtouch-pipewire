# V2 desktop indicator and optimistic control contract

Status: architecture and implementation gate.

This contract adds a desktop status and control client to the persistent v2
service defined by [`v2-service-contract.md`](v2-service-contract.md). It does
not change the receiver identity, hardware-volume, recovery, or publication
authority in [`v2-direct-output-contract.md`](v2-direct-output-contract.md).

The delivered client is one Rust application using GTK 4 and GIO. It provides
a StatusNotifierItem, a compact accessible window, and bounded desktop
notifications. The exact StatusNotifierItem binding is an implementation
choice made only after a direct compatibility probe on the supported Xfce
session; it is not part of the public API.

## Authority boundary

The existing Device Route remains the sole public hardware-volume request and
authoritative confirmed-state interface. WAPI remains the sole hardware
writer. Fresh WAPI readback remains the only source of confirmed volume and
mute, and the Node remains only the ordinary desktop request ingress and Route
mirror.

The v2 desktop client:

- submits volume and mute only through the matching PipeWire Device Route;
- never writes WAPI, Node Props, the PulseAudio compatibility interface, or a
  private D-Bus method;
- never treats a local PipeWire dispatch or sync completion as receiver
  confirmation; and
- may display local optimistic intent only inside its own window. It never
  publishes that intent as Route, Node, D-Bus, persisted, or service state.

The service exposes a separately versioned read-only D-Bus status mirror. That
mirror follows confirmed Route publication; it is not a second control plane,
current-state authority, retry trigger, or acknowledgement channel. Status
reads and subscriptions perform no WAPI, discovery, PipeWire, or recovery
operation.

This application does not make the Xfce panel OSD, `pavucontrol`,
`pwvucontrol`, or another desktop client optimistic. Those clients continue to
show the confirmed PipeWire state according to their own policies.

## Read-only v2 D-Bus status

The service owns the session-bus name
`io.github.Mr_Tao.SoundTouchPipeWire2` and exports the ObjectManager root
`/io/github/Mr_Tao/SoundTouchPipeWire2`. The root also exports
`io.github.Mr_Tao.SoundTouchPipeWire2.Manager`. Each published receiver is a
managed child below `receivers/` with the interface
`io.github.Mr_Tao.SoundTouchPipeWire2.Receiver`.

The project interfaces have no methods and no writable properties. The
standard read-only ObjectManager and Properties calls are the complete D-Bus
operation surface. The v1 name, objects, operations, and JSON status are not a
fallback.

`Manager` exposes:

- `ApiVersion` (`u`), exactly `2`;
- `DiscoveryHealth` (`s`), one of `starting`, `ready`, or `degraded`; and
- `Detail` (`s`), a concise human-readable diagnostic which clients must not
  parse as state.

`Receiver` exposes:

- `DeviceId` (`s`), the normalized receiver identity;
- `DisplayName` (`s`);
- `Lifecycle` (`s`), one of `active` or `degraded`;
- `Detail` (`s`), a human-readable diagnostic which clients must not parse;
- `PipeWireDeviceName` (`s`) and `PipeWireNodeName` (`s`), stable names used to
  locate and verify the matching public objects rather than exporting
  process-local global IDs;
- `ControlAvailable` (`b`), a coarse capability which is true while the
  published Route is willing to accept a new explicit client request under the
  v2 contract;
- `ControlBusy` (`b`), an observational indication that the receiver volume
  core has an active bounded operation;
- `Volume` (`u`) in the inclusive range 0..100, `Muted` (`b`), and
  `ConfirmedRevision` (`t`) as one atomic confirmed observation.

Clients must tolerate additive properties and unknown future health strings by
falling back to a non-mutating degraded presentation. `ControlBusy` is never a
request acknowledgement and must not be used to infer success.

`ControlAvailable` is not the volume core's transient `confirmed_valid` bit and
is not a second authorization gate. It remains true through a normal healthy
POST/readback chain, through `ControlBusy`, and through a recoverable state in
which an explicit Route request may first require the bounded GET allowed by
the direct-output contract. It becomes false only when the actor has stopped
accepting Route requests, for example while its local public object is absent,
retiring, or terminally unavailable. The Route implementation still makes the
final admission decision.

A receiver object is first exported only after its Device, Route, and Node
have been published from an initial fresh WAPI observation. It is retained for
the same lifetime as those public objects and is removed before or with them.
Pre-publication candidates are deliberately not a public desktop API.

`ConfirmedRevision` is monotonic only within the current unique D-Bus name
owner. It increases exactly once for every causally fresh WAPI observation
accepted for public confirmed state, including an observation whose numeric
tuple is unchanged. A deferred observation which is not made public because it
starts a successor POST does not increment the revision. The service first
publishes the confirmed Route and schedules the existing coalesced Node mirror,
then atomically changes `Volume`, `Muted`, and `ConfirmedRevision` on D-Bus.
There is no cross-transport guarantee that another client observes the
asynchronous Node mirror before the D-Bus change; both carry the same confirmed
observation and only the Route defines authority. No desired value, requested
tuple, request generation, optimistic value, pending value, or request
identifier is exported.

The client pins the Manager, ObjectManager, and receiver proxies to one unique
name owner. Owner loss or replacement, receiver removal, incompatible API,
loss of `ControlAvailable`, or replacement of the matching PipeWire object
immediately cancels local timers and discards all optimistic, in-flight, and
pending client state. Delayed signals and callbacks from the old owner or old
proxy instance are ignored.

## StatusNotifierItem and window behavior

GTK 4 does not own the tray protocol. The application exports a standard
StatusNotifierItem and uses GTK only for its window. It does not use the
removed `GtkStatusIcon` API, require an XEmbed-only tray, or make an
implementation-specific AppIndicator API part of the product contract.

Activating the indicator opens or focuses one compact application window.
Closing the window hides it while the indicator continues to run. The menu
contains only actions suitable for a standard status menu: open the window,
show application information, and quit the desktop client. Quitting the client
does not stop or restart `service-v2`.

A continuous slider is not placed in the StatusNotifierItem menu. The window
shows one row per published receiver with its name, health, confirmed or local
optimistic volume, mute state, an accessible mute toggle, and a 0..100 volume
control. Unavailable controls are visibly disabled; no intent is queued for
later recovery. Multiple receiver rows remain independent.

The indicator summarizes online, degraded, and unavailable state in its icon
and tooltip. Ordinary busy transitions, volume changes, receiver-side changes,
and transient degradation do not produce popup notifications. A service loss
which persists for five seconds after the client previously observed a ready
owner produces one replaceable `GNotification`; one recovery notification is
allowed only when that outage was notified. Initial absence produces an
inline unavailable state without a popup. Notification sound and presentation
remain desktop policy; the application does not invoke `notify-send` or play a
private sound.

The GTK application ID remains
`io.github.Mr_Tao.SoundTouchPipeWire.Control`, so desktop activation focuses the
single existing instance. The control package installs a user-disableable XDG
autostart entry. The client connects with D-Bus auto-start disabled and never
starts, restarts, or stops the backend implicitly.

## Optimistic volume and mute presentation

Optimism exists to make the client's own controls react immediately despite
receiver latency. It is a bounded presentation overlay, not a prediction that
the write succeeded.

Each receiver row owns the latest confirmed tuple, one displayed
`(volume, muted)` tuple, a dirty bit and monotonically increasing local edit
generation for each field, and one gesture scheduler:

1. Every slider or mute input updates the local display immediately, marks only
   that field dirty, and advances that field's local edit generation.
2. At most one PipeWire Route submission and its local core sync may be in
   flight. At most one latest full tuple is pending; a newer input replaces it.
3. Submission starts are separated by at least 200 milliseconds. Ending a
   gesture promotes its latest tuple, but does not duplicate an equivalent
   tuple already submitted or pending in that gesture. A later deliberate
   gesture may submit the same tuple again. Pointer release, cancellation, or
   focus loss ends a pointer gesture; 200 milliseconds without another key
   event ends a keyboard gesture; a mute toggle is one discrete gesture.
4. Every submission contains both volume and mute and records the two local
   field generations it carries. Immediately before dispatch, each field which
   has no unresolved local edit is rebased from the latest confirmed tuple. A
   volume-only gesture therefore preserves the newest confirmed mute state,
   including `muted=true`, and a mute-only gesture preserves the newest
   confirmed volume. Only an explicit local action marks its corresponding
   field dirty.
5. A PipeWire sync proves only that the Route request was locally dispatched.
   It neither clears the optimistic display nor marks the receiver confirmed.
6. While the pointer or keyboard gesture is active, a new confirmed
   observation updates the underlying authoritative tuple and every clean
   local field without moving a dirty field away from the user's current
   input. After a final tuple is submitted, the first newer
   `ConfirmedRevision` retires each local field generation carried by that
   dispatch and replaces it with the confirmed value, even when the receiver
   chose a different tuple. A field edited again after that dispatch remains
   dirty and visible until its newer generation is submitted or discarded;
   every clean companion field keeps rebasing from confirmed state.
7. If no newer confirmation arrives within three seconds after the final
   dispatch, the row retires the field generations carried by that dispatch,
   shows the latest confirmed values for those fields, and reports one inline
   failure for that final gesture. A field edited again afterward remains
   local. The timeout performs no resend, read, correction, service mutation,
   or notification.
8. A local dispatch error follows the same no-retry rule. Intermediate errors
   which are superseded by newer input stay quiet; only failure of the latest
   final gesture is presented.

The client records the `ConfirmedRevision` current at each final dispatch only
to decide whether a later observation is new. It does not correlate a
readback to a request ID, require equality, or suppress an external update.
Another desktop client, the Bose application, a remote control, or the
receiver itself may therefore win. There is no cross-client lease, ownership
claim, corrective write, or conflict dialog.

## PipeWire object selection and failure behavior

The client locates a Device by `PipeWireDeviceName` and verifies its normalized
`soundtouch.device-id`. It verifies the paired Node name when presenting the
row. Process-local PipeWire global IDs are never persisted or obtained from
D-Bus. Failure to find one exact current match disables the row; the client
does not fall back to Node writes, PulseAudio emulation, a similarly named
sink, cached globals, or direct WAPI.

An unknown Route-dispatch outcome is terminal for that local gesture. The
service's existing mandatory readback may later produce confirmed state, but
the client never resends merely because it did not observe that state in time.
A newly initiated user gesture is a new explicit intent, not a replay.

The service and client retain their objects and UI rows across the transient
failures allowed by the v2 service contract. A degraded row shows the last
genuinely confirmed tuple as stale. Mutation is disabled only while
`ControlAvailable=false`; a degraded row with `ControlAvailable=true` may
submit a new explicit Route request whose bounded processing first refreshes
the baseline as allowed by the direct-output contract.

## Exploration and tooling gate

Implementation and diagnosis must begin with direct, read-only manual
exploration. For each new one-off uncertainty, the developer states one
concrete hypothesis, runs the smallest bounded probe using existing tools,
inspects the result, and adapts the next probe to the evidence.

The first investigation must not create or commit a wrapper, test harness,
capture pipeline, audit framework, reusable script, compatibility layer,
skill, or new state machine merely to make the exploration systematic. A
working path is recorded in code only after the manual probe has demonstrated
it and at least one of these concrete needs exists:

- the behavior is a product invariant requiring regression coverage;
- the operation will recur and manual repetition is demonstrably error-prone;
- automation is needed to preserve a safety boundary or deterministic CI; or
- the code is itself the smallest production implementation of the proven
  path.

When codification is justified, add the smallest behavior-focused artifact at
the existing seam. Prefer one focused unit or integration test over a generic
harness, and remove throwaway probes instead of turning them into permanent
infrastructure. Do not add production polling, telemetry, retries, or state
only to satisfy a test. This gate does not waive the focused tests required by
the acceptance criteria below.

Household receivers, live WAPI, the real user PipeWire graph, panel state, and
desktop notification behavior are inspected manually by the parent session
only when live testing is explicitly in scope. Automated tests use isolated
D-Bus and PipeWire fixtures and do not contact speakers. Playback canaries use
digital silence and hardware volume zero unless the user explicitly authorizes
audible content.

## Delivery slices

Implementation proceeds in independently reviewable slices:

1. Manually prove the smallest StatusNotifierItem activation/menu path and the
   current PipeWire Route lookup/write path on the supported Xfce session.
   Select dependencies only from that evidence.
2. Add the read-only v2 D-Bus schema and service projection without any client
   mutation.
3. Replace the frozen v1 controller runtime with a read-only v2 status client,
   window, and indicator.
4. Add Route-only volume/mute control and its local optimistic scheduler.
5. Add bounded notifications, autostart packaging, and final desktop
   integration.

No slice may introduce a fallback authority or block audio publication on the
presence of the desktop client. A status-only slice must remain useful and
safe if the later control slice is delayed.

## Acceptance criteria

1. D-Bus introspection shows only read-only project interfaces, with no
   mutation methods, writable properties, hidden intent fields, or v1
   fallback; repeated reads perform no WAPI or PipeWire operation.
2. One fresh terminal WAPI observation is published to Route, schedules the
   coalesced Node mirror, and then appears as one atomic D-Bus volume, mute, and
   revision update. No Node-versus-D-Bus observation order is promised. An
   unchanged tuple still advances the revision once; a deferred non-public
   observation does not.
3. A differing valid readback replaces client optimism and causes no client or
   service correction, equality retry, quarantine, or output withdrawal.
4. Rapid input proves immediate local display, at most one local Route sync in
   flight, one latest pending tuple, at least 200 milliseconds between starts,
   and no duplicate final submission within one gesture.
5. Muted slider movement submits one full `(new volume, muted=true)` Route
   tuple. If another client changes mute during a local volume gesture, the
   next local dispatch preserves that newer confirmed mute. Conversely, a
   mute-only gesture rebases its untouched volume from a newer confirmed
   external value. A local volume edit followed by a local mute edit before
   confirmation carries both unresolved field generations and does not revert
   either one; a confirmation retires only the generations included in its
   preceding final dispatch.
6. Route sync without a newer confirmed revision never appears as confirmed.
   Dispatch failure, unknown outcome, and presentation timeout perform no
   automatic resend.
7. A newer receiver-side or other-client observation becomes confirmed and
   wins without a cross-client lease or conflict write.
8. `ControlAvailable` remains true throughout a healthy busy POST/readback
   chain and is not tied to transient baseline validity. D-Bus owner
   replacement, receiver removal, incompatible API,
   `ControlAvailable=false`, and PipeWire object replacement immediately clear
   optimism and reject delayed old-owner or old-proxy callbacks.
9. An absent or ambiguous matching Device disables the row and performs no
   Node, PulseAudio, WAPI, cached-ID, or name-only fallback.
10. Indicator activation focuses one accessible GTK window; its standard menu
    contains no embedded slider; closing the window leaves the indicator and
    service running, while Quit exits only the client.
11. Initial service absence creates no popup. One persistent post-ready outage
    and its recovery produce at most one replaceable notification each;
    transient health and volume activity produce none.
12. The control package contains one v2 desktop client with an exact dependency
    on its matching service build. It does not install a second v1 runtime UI
    or auto-start the backend over D-Bus.
13. Focused unit tests cover scheduler coalescing, final deduplication,
    confirmation replacement, timeout rollback, owner invalidation, and atomic
    muted volume. An isolated D-Bus integration test covers schema, object
    lifecycle, property ordering, and read-side-effect absence.
14. A manual Xfce smoke test verifies the indicator host, accessible window,
    notification replacement, exact PipeWire object match, and confirmed
    readback on one receiver before broader deployment. It does not create a
    reusable capture harness unless an observed recurring failure meets the
    tooling gate.

## Out of scope

This contract does not add zones, stereo pairs, multiroom or AirPlay 2,
playback transport controls, MPRIS, output-default selection, tray scrolling,
configuration mutation, service restart controls, direct WAPI controls,
system-wide optimistic OSD state, or automatic receiver recovery. Each would
require its own evidence and design gate rather than an extension hidden in
the desktop client.
