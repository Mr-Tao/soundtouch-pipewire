# Direct output v2 contract

Status: design gate for the first v2 implementation slice.

This contract deliberately replaces the v1 direct-output recovery model. The
v1 daemon, private gate/source-marker protocol, topology coordinator, MPRIS
router, and v1 controller operations remain frozen references; they are not
prerequisites for this slice.

## Scope

The first slice supports one discovered SoundTouch receiver as one ordinary
PipeWire audio device and sink. It provides:

- exact RAOP-to-SoundTouch identity matching;
- a standard PipeWire Device with one Profile and one Route;
- direct RAOP playback with numeric software gain fixed at unity;
- SoundTouch WAPI volume and mute read, write, event refresh, and readback;
- stable sink presence while the receiver remains discovered and identified.

Zones, stereo pairs, AirPlay 2, MPRIS, GUI operations, persistence outside
normal WirePlumber Route policy, quarantine, and automatic topology recovery
are out of scope.

The caller supplies the bounded RAOP latency using the project-wide
250..10000 ms range; latency policy is not a second runtime authority.

## Authority and public state

- A fresh matching `/info` response establishes physical identity.
- A fresh successful `/volume` response is the hardware observation. Its
  `actualvolume` is the current value to publish; `targetvolume` may differ
  while Bose is moving and is diagnostic, not an equality gate.
- The Device Route is PipeWire's sole authoritative confirmed-state and
  hardware-transaction interface. WirePlumber may save and restore it normally.
- Node Props are the ordinary desktop volume/mute request ingress and mirror
  the confirmed Route for clients. They are not a second current-state or
  persistence authority: every admitted request enters the same Route/WAPI
  transaction path, and only WAPI readback updates confirmed state.
- WAPI is the only hardware-volume writer. The RAOP module never emits an RTSP
  volume write and numeric software gain remains 1.0.
- The volume transaction core has exactly three state roles:
  - **desired** is volatile, latest-wins intent. Each explicit Route request
    creates a monotonically increasing generation and may replace either field
    while retaining the other field from the latest desired intent;
  - **requested** is the immutable full volume-and-mute tuple and generation of
    the current POST; and
  - **confirmed** is a successful, causally fresh GET observation containing
    `actualvolume` and mute. Confirmed state is the sole source of published
    Route and Node current state.
  There is no public pending or optimistic state. While an operation is in
  flight, the core retains only the latest desired intent, the active requested
  tuple if a POST exists, and at most one confirmed-but-deferred observation.
  Every causally fresh successful WAPI GET updates the internal confirmed
  baseline. If that observation starts a successor POST, it replaces the
  deferred tuple without publishing; otherwise it becomes the published
  current Route, even when it differs from the request.

An observed receiver update must not be rejected merely because it differs
from an earlier or restored Route. Restoring a saved Route is a valid hardware
request, not proof that the receiver already has that value.

A Route SetParam without volume or mute Props is a no-op. A valid Route with
Props is one request regardless of its `save` value; `save` remains
WirePlumber's persistence concern.
Every explicit `save` value on such a request also re-serializes the published
Route, including repeated `save=false`. This lets WirePlumber classify its
initial selected Route as active without converting that policy notification
into a hardware write; the volume core remains the sole no-op authority.
Partial Props inherit missing fields from the latest desired intent, then the
active requested tuple, then a valid confirmed baseline; only the core owns
this overlay. Published state is never a fallback for constructing a write:
after baseline invalidation it remains client-visible but cannot initialize a
hardware request.

## Startup

1. Match the RAOP identity to a fresh SoundTouch `/info` device ID.
2. Read a valid `actualvolume` and mute value successfully.
3. Publish the Device, Route, and sink initialized from that observation. A
   differing `targetvolume` does not block publication or start a retry; a
   later WAPI event may refresh the current value.
   Arm Node Props request handling only after the verified Node has emitted a
   complete canonical Props tuple matching that fresh observation and a core
   sync barrier has ordered the initial mirror. Missing, malformed, mismatched,
   or full-scale default Props fail closed and never initialize hardware. Every
   Props event before that barrier is evaluated, so a later mismatch or invalid
   event cancels startup even after an earlier matching canonical event.
4. During WirePlumber's initial Route selection, fill each missing persisted
   field from the currently published Route. In particular, missing
   `channelVolumes` and `mute` inherit the observed tuple; no device-global
   default is allowed to replace either field.
5. A stored `channelVolumes` or `mute` value wins field by field. If
   WirePlumber restores a tuple different from the observation, process it as
   an ordinary Route request.

No cached, missing, malformed, or default value may initialize hardware
volume. A validated restored Route may increase or decrease volume; the
safety property is absence of blind or uninitialized writes, not a blanket
ban on increases. The receiver's expected zero-to-ten playback convenience
remains receiver behavior and is not classified as a fault.

## Volume and mute transaction

For one accepted Route request:

1. Create a new desired generation and retain only its latest per-field intent;
   there is no gesture queue. Serialize at most one WAPI operation per receiver.
2. With a valid confirmed baseline and no operation in flight, resolve the full
   tuple and send volume and mute atomically in one WAPI `/volume` request
   immediately. This first POST has no preflight GET, debounce, or timer. The
   immutable requested tuple records that desired generation until the POST's
   mandatory GET completes.
3. Perform a fresh `/volume` readback after every POST result, including a
   result reported as not delivered. A receiver event during that POST is
   covered by this later mandatory GET.
4. When a readback starts no successor POST, publish its `actualvolume` and
   mute as the current Route and mirrored Node state, and clear any deferred
   tuple. When a readback starts a successor POST, update the internal baseline
   and replace the single deferred confirmed tuple without publishing an
   intermediate Route/Node value. A valid terminal result different from the
   request or with `targetvolume != actualvolume` ends the transaction without
   degradation or correction.

A desktop Node Props change is admitted only from the canonical outer event
that contains full uniform `channelVolumes`, `mute`, scalar unity `volume`,
unity `softVolumes`, and `softMute` equal to `mute`. The following mirror event
without scalar `volume` is ignored, so one desktop action cannot become two
hardware requests. Partial, non-finite, non-unity, inconsistent, or ramped
Props are not hardware intent.

The public Device Route accepts one or the expected number of finite, uniform
`channelVolumes` values in the inclusive range `[0.0, 1.0]`. It maps the
continuous desktop value to the nearest integer hardware percentage across
the full range. Non-uniform channels remain invalid because the receiver has
one scalar hardware volume and must not silently discard desktop balance.

The canonical Node Props ingress remains stricter. PulseAudio-compatible
clients encode volume in 16.16 linear units before PipeWire publishes cubic
Props values. An otherwise canonical Node value may therefore differ from an
integer percentage's exact cubic value by at most one Pulse quantum in the
linear domain and still maps to that hardware percentage. Larger fractional
Node values remain invalid, and a Node value of 100 still requires exact
full-scale `1.0` on every channel, so this mirror path cannot become a second
continuous-volume authority.

For external volume contract 2, the RAOP module's Props callback is only a
stateless, idempotent canonical-tuple gate. It does not enter the legacy RAOP
volume state machine, reassert an older logical tuple, or send RTSP volume.
Only one complete current-event tuple is applied: scalar `volume` exactly at
unity, finite uniform `channelVolumes`, `mute`, equally sized unity
`softVolumes`, matching `softMute`, and no duplicate or ramp fields. The
applied Pod copies that event's logical values and gains without consulting
module state or history. Partial, malformed, non-unity, or scalar-less
follower events are semantic no-ops and can never be promoted into hardware
intent; unrelated Props return without intervention. Because PipeWire 1.6.8
applies the outer Pod after its callback returns, the module technically
implements each managed-event no-op with a reentrant empty Props write. The
stream's `in_set_param` guard vetoes the outer Pod without reconstructing any
previous tuple. A failed canonical apply or veto fails the local stream closed.
Contracts 3 and 5 and all non-external volume modes retain their existing
Props behavior.

Each companion-authored Node mirror reserves one exact tuple before SetParam
and retires it only after a core sync barrier. Its matching canonical event is
an echo and never enters the Route path; a differing canonical event is a new
desktop request. While that one token is outstanding, a newer confirmed WAPI
observation with no successor POST updates Route immediately and only the
latest desired Node mirror is coalesced. There is no timer, generation, retry,
quarantine, or corrective loop in this echo guard.

Volume changes while muted are sent with mute preserved. A Route request that
arrives during an operation supersedes the desired intent but never causes an
uncertain write to be replayed. No autonomous corrective volume-up/down loop
is allowed.

After a write timeout or other unknown delivery result, no later write is
allowed until the mandatory fresh `/volume` GET establishes a new confirmed
baseline. A newer desired generation may replace older desired intent while
that GET runs and may then produce one new POST. If the GET fails, the active
requested tuple and all desired intent, including any newer generation, are
discarded. Nothing remains to replay after reconnect or rediscovery.

Even when the transport reports that a POST was not delivered, the failed
request ends and is never replayed. Its single GET refreshes the baseline;
only after that readback may a distinct newer desired generation produce one
POST.

A valid WAPI `volumeUpdated` event requires a GET that starts after the event.
An event during POST is covered by the mandatory later GET. An event during an
in-flight GET latches one causally newer GET requirement: the current GET may
not update confirmed, deferred, or public state and cannot start a successor
POST. Discard its tuple, or ignore its failure, and start exactly one new GET.
Only failure of that causally later GET is terminal and may degrade health or
discard intent. Multiple events before that causally newer GET starts coalesce
without a timer. If no newer request starts a successor POST after the causally
sufficient result, it immediately becomes the current Route and client-visible
state without a receiver write.

## Failure and lifecycle

A failed or timed-out readback, event disconnect, or temporary WAPI
unreachability changes control health to degraded but retains the Device and
sink. If an operation chain has a deferred confirmed readback, a terminal GET
failure publishes that latest tuple once together with degraded health and
clears it; otherwise the last published observation remains visible. Neither
case starts another action or retry. A terminal GET failure retires the active
requested tuple and discards all desired intent, including generations newer
than that requested tuple. A later partial Route may inherit only from intent
created by that new explicit Route generation and from a newly valid confirmed
baseline, never from the ended uncertain write or stale published state. A
valid readback different from the request is successful observation, not
degraded health. None of these states quarantines or withdraws the sink or
starts a polling recovery loop.

An event-channel disconnect immediately invalidates the writable confirmed
baseline, discards unstarted desired intent, and retains public objects and
their last genuinely confirmed values. It does not cancel an in-flight POST or
its mandatory GET, retire that active requested tuple early, discard a
genuinely confirmed deferred observation, or discard an unsatisfied causal
later-GET requirement. A causal latch created before disconnect still starts
its one later GET, but cannot restore write authority while disconnected.
The mandatory GET still resolves the uncertain POST and may publish its
genuine observation, but while disconnected that result remains invalid as
future write authority. Reconnect, a receiver event, or rediscovery may start
one GET only; it cannot replay or autonomously construct a write. A successful
GET started by such recovery restores the writable baseline.

A new explicit Route request creates a new desired generation. If no valid
baseline exists, a GET explicitly started after invalidation may serve it. A
GET already in flight from before invalidation cannot become authoritative
merely because the Route arrived: the Route requires one causally later GET
before it may POST. A Route accepted while POST is in flight may rely on the
mandatory GET because that GET necessarily starts later.

Route dispatch across PipeWire and owner contexts is fenced by an atomic output
volume epoch. Invalidation advances the epoch before invalidating the volume
core. A queued Route carrying an older epoch is discarded before it can create
desired intent or a GET/POST; a genuinely new Route accepted afterward carries
the new epoch.

Output generation recreation, receiver identity change, and shutdown clear all
desired, requested, deferred, and causal-event state. Local object recreation
may seed only from valid confirmed state; it does not preserve speculation.

Recovery is event-driven. A new explicit Route request, a fresh WAPI event, a
WAPI reconnect, or receiver rediscovery may start one new bounded operation.
Reconnect, event, and rediscovery recovery are GET-only unless a new explicit
Route generation exists.

A single mDNS REMOVE, browse failure, RAOP connect failure, or WAPI timeout is
not proof of disappearance. It retains the sink and waits for rediscovery or
another named external event. A changed endpoint announcement is likewise not
identity evidence: this first slice retains the existing output and requires
an explicit restart before validating and adopting the new endpoint. It does
not run a second post-publication identity/migration state machine.

Withdrawal is permitted only after a fresh contradictory identity result, an
explicit policy removal, or user/service shutdown. This slice implements the
startup identity gate and shutdown path; it has no dynamic policy reload or
post-publication contradictory-identity probe. Local PipeWire object loss may
recreate only the local objects while the receiver identity remains valid.

The v2 RAOP transport uses activation-driven recovery. A connection, RTSP
handshake, or session timeout failure cleans only that receiver session and
keeps the Device, Route, and Node published. Time alone cannot start another
attempt. The failed activation is rearmed only after PipeWire closes that
playback activation: close ends the generation while preserving its
failure latch, and the following playback-open edge clears the old latch and
authorizes one new bounded attempt when Format is present. Format changes alone
do not rearm recovery. Activation mode module-authors `node.pause-on-idle=true`
so these playback edges cannot be suppressed through nested stream properties.
This behavior is an explicit v2 module option and does not change the legacy
RAOP reconnect policy used by v1 or ordinary PipeWire RAOP sinks.

## Acceptance criteria

The first implementation slice is complete only when automated tests prove:

1. With empty WirePlumber state, initial WAPI `(37, muted)` publishes Route
   `(37, muted)` and survives initial Route selection without a hardware
   write.
2. After saving the same stable Route as `(37, unmuted)`, recreating it while
   WAPI reports `(37, muted)` restores the stored mute with exactly one write
   and one readback.
3. A restored or client Route `(42, muted)` causes one write and one readback;
   readback `(41, muted)` becomes the visible Route while mute stays active.
4. A physical/app/remote-control event followed by readback updates Route and
   client-visible state without an echo write.
5. One PulseAudio-compatible desktop volume or mute action produces one WAPI
   POST and one GET even though the Node emits a canonical event plus a follower
   mirror. A differing readback remains the visible Route and Node state; the
   follower cannot replay the request.
6. An unknown write result performs at most one resolving GET and never replays
   the write. No later POST occurs until a fresh baseline exists.
7. A rapid `0 -> 4 -> 12 -> 16 -> 20` request chain publishes no intermediate
   Route/Node value and publishes only the terminal readback. If the last GET
   fails after confirming `16`, it publishes only `(16, degraded)`, discards
   all remaining desired generations, and starts no autonomous action.
8. Failed readbacks retain the sink, emit one degraded result, and do not retry
   until a named external event occurs.
9. A transient mDNS REMOVE or changed endpoint announcement retains the sink;
   a mismatching fresh `/info` prevents initial publication, and user/service
   shutdown withdraws published objects. Dynamic endpoint migration and policy
   reload are not part of this slice.
10. No path emits an uninitialized full-scale value, RTSP volume, or software
   gain.
11. Existing v1 tests remain a frozen baseline but do not define v2 behavior.
12. A valid baseline causes the first explicit Route to POST immediately while
    public Route and Node remain confirmed-only; latest-wins preserves atomic
    mute, and a differing valid readback is successful confirmed state.
13. An event during GET forces one causally later GET before publication or a
    successor POST; repeated events coalesce. An event during POST is covered
    by the mandatory later GET.
14. After terminal GET failure, reconnect, event, and rediscovery perform GET
    only and cannot replay discarded intent. A new explicit generation,
    including the same numeric tuple, may write after restoring a fresh
    baseline.
15. Output recreation clears speculative state, and no write tuple is ever
    completed from stale published-only state.
16. Success or failure of a GET overtaken by an event starts exactly one later
    GET without publishing, degrading, or discarding requested/new desired
    state. Only failure of the later GET is terminal.
17. A pre-invalidation GET cannot become write authority because a Route
    arrives later, and a Route queued across the output invalidation epoch is
    discarded before it reaches the volume core. A genuinely new Route and a
    GET already started after invalidation remain admissible.
18. Disconnect preserves an unsatisfied causal later-GET requirement but
    removes its authority-restoring power until reconnect, a new event, or a
    new explicit Route authorizes post-invalidation observation.

After automated validation, a live canary must prove actual RAOP playback,
Route volume, mute, muted volume adjustment, receiver-originated updates, and
stable sink presence. Live playback uses hardware volume zero unless the user
explicitly authorizes another level.

For this criterion, “muted volume adjustment” means an admitted request, one
WAPI POST carrying the requested volume with mute preserved, and the mandatory
causally fresh GET. The receiver may retain a different numeric value,
including zero; that valid readback is terminal successful confirmed state and
must be published without correction, retry, or degradation. POST completion
itself is not state evidence.
