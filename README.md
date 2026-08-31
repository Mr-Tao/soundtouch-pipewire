# soundtouch-pipewire

`soundtouch-pipewire` is a fail-closed companion for Bose SoundTouch RAOP
outputs. It reads the physical receiver volume before publishing a PipeWire
sink, owns hardware volume through the SoundTouch Web API, and uses a separately
named patched RAOP module plus a private virtual-zone module that cannot
replace PipeWire's system files.

The project source is licensed under the MIT License. Using its Bose SoundTouch
Web API connection additionally requires each end user to review and accept the
supplemental terms in [`licenses/EULA.txt`](licenses/EULA.txt); those terms do
not restrict copying, modification, or redistribution of the MIT-licensed code.
Third-party notices are collected in [`licenses/`](licenses/).

## Build

```sh
meson setup build
meson compile -C build
./tools/run-meson-tests-clean-env.sh build
```

The test helper starts Meson with an allowlisted environment and private
temporary HOME and XDG directories. Meson records its inherited environment in
`meson-logs`, so do not replace the helper with a direct `meson test` invocation
when credentials are present in the development session.

Runtime dependencies are PipeWire with its audio support package,
WirePlumber 0.5.15 or newer, GLib/GIO, Avahi, libsoup 3, libxml2 and
json-glib. WirePlumber's standard device-profile and device-route policy owns
selection and persistence of the companion's paired Device Route. The two
private PipeWire modules described in
[`docs/private-module-contract.md`](docs/private-module-contract.md) are
packaged together: the RAOP module is a hard runtime dependency, and the zone
module is required whenever a virtual zone sink is published.

Saved SoundTouch zones, SoundTouch 10 stereo pairs, the versioned D-Bus
control API, and the optional Rust/GTK 4 controller are described in
[`docs/multiroom-control.md`](docs/multiroom-control.md). Build the controller
with `-Dcontrol_app=enabled`. It is disabled by default so headless package
builds do not acquire an undeclared Cargo/GTK dependency. The Arch packaging
enables it explicitly and emits a separate `soundtouch-pipewire-control`
binary package with an exact dependency on the matching service package
revision. The controller itself remains a frozen v1 tool.

## Persistent direct-output v2 service

The installed user service runs `service-v2`. One process owns one discovery
client and one PipeWire context, with an independent direct-v2 actor for every
admitted receiver. A transient resolver timeout or REMOVE retains every
already published sink. If Avahi itself fails, discovery is restarted while
published outputs remain present.

The frozen v1 `daemon`, `service-v2`, and the single-receiver `direct-v2`
canary share one instance lock and cannot manage receivers concurrently. The
manual canary therefore requires stopping the installed service first:

```sh
soundtouch-pipewire direct-v2 --device 020000000001
```

The canary waits for that exact discovered SoundTouch identity, obtains a
fresh `/info` and `/volume`, and only then publishes its Device, Route, and
sink. Transient discovery, WAPI, or RAOP failures retain the published sink;
they do not start an autonomous retry loop or withdraw it. The design and live
canary criteria are defined in
[`docs/v2-direct-output-contract.md`](docs/v2-direct-output-contract.md).
A changed endpoint is not adopted while the output is published; restart the
manual canary to validate and use a new receiver address.

Persistent multi-receiver ownership, discovery recovery, packaging cutover,
and rollback are defined in
[`docs/v2-service-contract.md`](docs/v2-service-contract.md). The legacy D-Bus
controller, zones, stereo pairs, MPRIS router, and virtual-zone operations are
not connected to `service-v2`; PipeWire Device Routes are its supported control
surface.

The legacy `status` command and GTK controller do not describe `service-v2`.
On startup, after exclusive-lock acquisition, v2 removes any stale v1 runtime
status so those tools fail unavailable instead of reporting obsolete state.

## Legacy v1 safety model (frozen reference)

1. `_raop._tcp` and `_soundtouch._tcp` must resolve to the same IP.
2. The 12-hex prefix before `@` in the RAOP instance must equal `/info`
   `deviceID`. The hostname is never used as identity.
3. The `gabbo` WebSocket is connected, then `/info` is revalidated before
   `/volume` is read. This exact check is repeated after every reconnect.
4. `targetvolume` and `actualvolume` must converge before the sink is
   published.
5. A sparse PipeWire registry announcement is only a provisional name match.
   The bound node's full `pw_node_info` must prove the device, route, external
   volume contract, restore policy, and a fresh random publication ID before
   volume Props are subscribed or the sink is accepted.
6. Every publication consumes a single-use `/info` token followed by one
   stable `/volume` result and one private-module add attempt. A failed,
   unstable, stale, or rejected attempt must start again at `/info`.
7. A per-MAC cache may record confirmed values for diagnostics, but it never
   supplies initial gain, authorizes a hardware write, or publishes a stale
   sink. Cache records are stored atomically with mode 0600.

The default policy is `reject`; no volume request is replayed after a failure.

## Required first start

Do not start the companion beside PipeWire's stock SoundTouch RAOP sinks. The
stock sink can still be selected by an application and does not implement the
hardware-volume contract.

```sh
mkdir -p ~/.config/soundtouch-pipewire
cp /usr/share/doc/soundtouch-pipewire/soundtouch-pipewire.conf.example \
  ~/.config/soundtouch-pipewire/config.ini
soundtouch-pipewire eula show
soundtouch-pipewire eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1
soundtouch-pipewire migrate-stock-discovery
soundtouch-pipewire migrate-stock-discovery --apply
systemctl --user restart pipewire.service pipewire-pulse.service wireplumber.service
soundtouch-pipewire doctor
```

Review the migration dry run before `--apply`. It accepts only the exact known
RAOP discovery drop-in, keeps non-SoundTouch AirPlay receivers, and creates a
timestamped backup. `doctor` performs a PipeWire registry barrier and fails if
any stock `Bose-SM2-*` RAOP sink remains. The daemon enforces the same check at
startup and exits if such a node appears later.

The configuration is
`$XDG_CONFIG_HOME/soundtouch-pipewire/config.ini` (normally
`~/.config/soundtouch-pipewire/config.ini`). The safe example opts in synthetic
device IDs individually; replace them with the physical `/info` device IDs.
Receiver admission is a three-state policy. In a `[device MAC]` section,
`enabled=true` explicitly allows that receiver, `enabled=false` explicitly
blocks it, and omitting `enabled` leaves it in `auto`. With
`manage-all-verified=false`, only explicitly allowed receivers are published.
With `manage-all-verified=true`, `auto` receivers which pass complete RAOP,
fresh `/info`, and fresh `/volume` identity checks are also published; an
explicit block still wins. The event channel is connected after publication.
“Verified” here means that live technical identity proof, not a remembered
user approval.

The legacy GTK controller can edit the global mode and each receiver's
three-state policy only through the frozen v1 daemon. It is not a control plane
for `service-v2`. V1 writes use the exact on-disk configuration digest,
preserve unrelated keys and comments, and fail on a stale digest. A successful
write changes only the configured policy: the effective daemon policy and
published sinks remain unchanged until the service is restarted. GUI writes
are disabled when the daemon was started with an explicit `--config`, even if
that file is otherwise writable. `soundtouch-pipewire doctor` reports the
effective mode, config path, and section counts; it can also run while the
daemon is active without mistaking that daemon's own private nodes for a
second unsafe instance.
Each device may set `raop-latency-ms`. Its default is `1500`; accepted values
are `250` through `10000`. PipeWire adds a fixed 250 ms transport margin, so
the default appears as roughly 1750 ms of process latency. Reducing the value
can shorten the audible return after unmute, but also reduces tolerance for
network jitter and may cause dropouts. Start a receiver-specific canary at
`500` before trying `250`. This setting does not weaken the hardware-volume
confirmation or guarded-unmute rules. A change takes effect when the sink is
republished, normally after restarting `soundtouch-pipewire.service`.
Receivers which ignore numeric decreases while muted may also opt in to
`muted-volume-down-key=true`. The default is `false`. With the option enabled,
small local decreases of at most five percentage points use receiver-native
`VOLUME_DOWN` key ticks, each separated by a new identity and physical-volume
confirmation. Larger changes remain local shadows. This workaround is
deliberately receiver-specific because `/key` behavior is a firmware property,
not part of the RAOP discovery contract.

## Legacy v1 zone activation (frozen reference)

The first safe multiroom playback path promotes an already owned direct RAOP
session. Start playback on exactly one saved-zone member's normal private
SoundTouch output, then invoke **Activate** for that zone. The daemon requires
that exact demanded sink, its current publication generation, an open healthy
transport gate, and the random source marker confirmed both by RTSP and fresh
SoundTouch `AIRPLAY` state. If the preset names a preferred master, it must be
that playing member; without a preference, the one unambiguous owned member
becomes the effective master.

Activation while every member is idle is deliberately rejected. Live receiver
testing proved that an idle `/setZone` can make SoundTouch firmware resume a
remembered local source, so an apparently idle preflight is not evidence that
the operation will remain silent. An unrelated active source, two owned direct
sessions, or a changed marker or publication also fails closed. Receiver-volume
drift fails closed except for one audited Bose startup side effect: one
non-master follower may move from stable `0,unmuted` to stable `10,unmuted`
across two fresh all-member reads at least one second apart. This exception
does not issue a receiver or PipeWire volume write; all other values, mute
changes, master changes, and multiple changed followers remain errors.

After `/setZone`, the physical master sink remains the sole PipeWire audio
route and Bose distributes its stream to the followers. No virtual zone sink
is published for this mode. A follower normally withdraws its RAOP service
while grouped; the daemon therefore retires only that follower's direct sink
and retains its previously identity-bound WAPI control plane for topology and
hardware-volume verification. `AudioState` reports `promoted-direct` while the
route is owned and `promoted-dissolving` after playback demand ends.
That control-only retention currently depends on the identity having been
verified earlier in the same daemon lifetime. Restarting while a Bose zone is
already active and advertises no follower RAOP service may therefore require
the zone to be dissolved externally before every direct sink returns.

The last direct link does not dissolve the Bose zone immediately. It first
completes the existing guarded direct-sink teardown without increasing volume
or unmuting any receiver, then requires an all-member topology/source snapshot
which started after that falling edge and reports every receiver inactive.
Only then is a non-cancellable, audited `dissolve-zone` operation queued in the
normal global FIFO; the executor repeats the inactive-source check immediately
before its first Bose mutation and then validates the same falling-edge
PipeWire demand generation directly on the PipeWire loop. A relink, even if
its daemon callback is still queued or it already unlinked again, revokes that
operation. Spotify, AirPlay, or a stale snapshot likewise postpones cleanup
instead of causing operation churn. Once fresh topology confirms
dissolution, a returned follower RAOP service may publish a new direct sink
from a new identity and volume preflight. Failure or ambiguous receiver state
remains contained instead of replaying the dissolve or opening audio
automatically.

## Legacy v1 volume transaction safety (frozen reference)

PipeWire UI volume is translated to the receiver's `/volume` Web API; the
private RAOP module keeps numeric software gain at unity and never sends RTSP
volume. Contract 3 combines canonical mute with a private sequenced transport
gate, so an activation, reconnect, mute transition, or transport error cannot
emit audio before the exact hardware state is reconfirmed. The gate has a
per-publication nonce and a monotonic generation; stale release commands are
rejected by the module. An active RAOP reconnect advances that generation
before replacing its source marker. Before a demanded virtual zone may release
that gate, the module also rotates a random RTSP metadata marker and the companion
requires a subsequent `/now_playing` response to report exact `AIRPLAY` plus
that marker. A foreign AirPlay session is therefore not mistaken for the
companion merely because Bose gives both sources the same generic source name.
The proof also retains the exact route-arm and gate generation: any session
rebind invalidates it before a fresh hardware-volume read can release the
successor session. The release command itself carries that exact confirmed
marker sequence and value plus the proof's absolute `CLOCK_BOOTTIME` deadline.
Every hardware-volume proof expires strictly five seconds after its GET was
issued, including suspend time; a zone uses the earlier of that and its source
proof deadline. The module validates the deadline and opens the gate while
serialized under the RTP data-loop lock, and drains the same lock before a
successful close or transport teardown. It rejects a separate AES67 sender
loop that could bypass this boundary. Suspend or a delayed client/server
handoff therefore cannot revive an old proof. Another same-gate marker cannot
inherit it. In a verified Bose zone, an exact marker echo from any member is
harmless; only the physical master may initiate a new proof, while a
mismatching follower report fails closed.
SoundTouch firmware may also change hardware volume or mute while retiring an
AirPlay source. A direct physical sink therefore captures its exact confirmed
receiver tuple before an acknowledged DISARM, adopts only the forward closed
same-publication gate produced by that command, and retains a no-release
reservation through the source-marker `NONE` tombstone and a new quiet proof.
Any safely restorable teardown-side divergence is repaired under that closed
gate; an unsafe increase or ambiguous change is contained instead. The
physical receiver tuple is kept separately from a muted desktop volume shadow,
so restoration does not move the visible logical volume. A quick
unlink/relink reuses the same reservation but still requires the old `NONE`
followed by a newly confirmed source marker before the successor session can
be proved.
Zone volume, mute and demand transitions share a non-wrapping PipeWire input
generation. The proof captures it, and the guarded apply/readback/release
transaction rejects a generation already advanced on the PipeWire thread even
when its main-loop event is still queued. A differing preceding adapter tuple
is likewise treated as input rather than tolerated during guarded release.
`Core.Sync` is nevertheless not a global transaction across ordinary clients:
a separately connected mixer can have a request processed after the final
barrier but before the module handles release. Closing that remaining
inter-client window requires a module-owned input token checked atomically with
release.
A sink is not exposed merely because its registry markers match: its first
follower Props must prove the fresh initial volume/mute and unity software
gain, after which the companion seeds and confirms the canonical adapter Props
and waits for a PipeWire drain barrier. Callbacks before that barrier and later
follower replays cannot become hardware writes. Each accepted write is
serialized and confirmed by a fresh GET.
Bose applies
`muteenabled` before volume and automatically unmutes on increases, so the
companion uses these rules:

- every debounced raw desktop volume/mute intent, and every phase of a
  multi-phase Bose operation, performs `/info` exact-device verification and
  only then starts its own fresh gabbo-epoch `/volume` GET; a periodic GET
  already in flight cannot authorize the write, and a newer event invalidates
  the whole preflight;
- a stable preflight GET refreshes the internal safety baseline but cannot
  replace a newer optimistic desktop value; only an unsolicited receiver
  change, the matching POST confirmation, or an explicit rollback updates the
  PipeWire node;
- after a successful POST, stale, still-converging, or otherwise unclassified
  confirmation reads are retried without replacing the optimistic desktop
  value; confirmation is bounded by both request count and a short deadline,
  after which receiver writes are quarantined and the still-visible sink stays
  inert behind its closed transport gate instead of rolling the node back to an
  unproven receiver tuple;
- while hardware mute is freshly confirmed, an actual local volume-down whose
  target is below the fresh physical baseline keeps the local shadow and binary
  `softMute` closed. By default it makes one best-effort WAPI POST of
  `(target volume, mute=true)`: an exact confirmation advances the physical
  baseline, while a safely ignored unchanged result retains the local shadow.
  With the receiver-specific `muted-volume-down-key=true` option, a small
  decrease instead uses one native `VOLUME_DOWN` press/release pair at a time.
  Every pair is followed by a fresh stable GET and every additional tick starts
  with another exact `/info` check. Because the receiver closes each HTTP
  connection, the normal path waits for the press result before sending the
  final release on a new connection. If the press has not completed within
  200 ms, a fail-safe release leaves on the second connection before the stuck
  press is cancelled. The final idempotent release after the press callback
  establishes the terminal receiver state. It is retried once when necessary,
  and the uncertain-press path adds a second sealing release even after the
  first final release succeeds. Completion waits for a fail-safe release when
  one was needed, but only a post-press final release can confirm cleanup. An
  orderly daemon shutdown withdraws the sink, drains in-flight cleanup through
  a separately refcounted lifetime tracker independently of discovery-table
  membership, and exits nonzero if release remains uncertain. Cancelling a
  removed speaker also cancels its in-flight press without cancelling
  mandatory release cleanup. An uncertain pair is never replayed.
  Progress preserves the optimistic desktop value; reaching or safely passing
  the target adopts the confirmed quieter value. No progress is retried only
  within the bounded confirmation window. A concurrent increase stops the tick
  sequence and leaves the target as a local shadow. A confirmation reporting
  `mute=false` quarantines receiver writes and closes the sink's transport
  immediately;
- volume-up while muted is always local-shadow-only and is never sent to the
  receiver, even if its final value remains below the physical baseline. A
  volume-down whose final value is not below that baseline is shadow-only too.
  Reconciliation continues to refresh the physical baseline without replacing
  the visible shadow, which is applied by the guarded transaction on an
  explicit unmute. A scalar change that spuriously drops mute—whether in one
  Props record or as two records within the debounce window—is normalized back
  to a muted volume-only intent, matching ordinary desktop sink behavior;
- while that logical muted shadow exists, a receiver-side `mute=false` without
  a matching PipeWire unmute is indistinguishable from an unsafe reconnect
  unmute and is therefore write-quarantined behind the closed gate. The
  automatic recovery proof described below may safely clear that state; the
  uncertain intent is never replayed;
- if mute and a volume key coalesce while the receiver is still audible, the
  receiver is first muted at its unchanged physical volume; only after exact
  confirmation does the requested volume become the local muted shadow;
- on an explicit unmute, the module closes a fresh private transport-gate
  generation before accepting the public canonical transition. The companion
  therefore leaves the public node unmuted instead of briefly reasserting
  `mute=true`, avoiding the old one-way OSD flicker while audio remains blocked.
  It sends the final `(shadow volume, mute=false)` receiver tuple once and
  releases only that exact gate sequence and nonce together with the current
  confirmed source-marker sequence and value after a fresh GET confirms the
  tuple. A newer activation, reconnect, mute, error, volume intent, marker, or
  mismatching GET makes the captured proof stale and keeps the gate closed;
- a non-shadow hardware transaction requesting increase + final muted never
  sends the higher value; an audible receiver is only muted at its current
  volume and an already-muted receiver is rolled back;
- a disconnect, timeout, or control loss while a POST may already have reached
  the receiver immediately closes the transport, invalidates stale-cache
  eligibility, and quarantines further receiver writes. The physical sink
  remains visible while automatic recovery starts again from gabbo-bound
  identity and fresh physical volume state; it never replays the uncertain
  intent.

Classic RAOP remote-control commands are serialized through the same safety
pipeline. Every volume command starts with a new exact `/info` check and a
fresh `/volume` snapshot. Commands received before that snapshot are evaluated
as one ordered prefix: a receiver-side change that has already reached the
same cumulative result is accepted without a duplicate write, including an
even number of mute toggles and mixed volume-up/volume-down bursts. Commands
arriving after the GET started remain queued for a later barrier. Ambiguous
concurrent receiver changes are rebased only within a bounded lifetime and
retry count; they are never silently discarded or blindly replayed.

Play, pause, play/pause, stop, next and previous commands are routed to an
already-running MPRIS player. The router never auto-starts an application. It
selects only one unambiguous player whose current status and capabilities
match the command, remembers a successful per-speaker owner affinity, and
revalidates that D-Bus owner immediately before dispatch. Ambiguity, owner
replacement, capability loss, timeout or queue exhaustion is a no-op rather
than permission to control a different application.

## Legacy v1 automatic recovery (frozen reference)

Transport, HTTP or XML failures that provide no trustworthy `/info` identity
are recoverable, while a successfully parsed nonmatching `deviceID` remains a
terminal safety failure. Event-channel recovery uses five short exponential
retries and then continues every 30 seconds for as long as the same mDNS
endpoint exists. A recovered channel must again pass matching `/info` and a
fresh stable `/volume` read before its sink is published.

An already published physical sink follows a narrower fault policy. Transient
receiver, source, topology, lineage, backpressure, or proof uncertainty keeps
the same sink visible and preserves its publication generation, but only after
the daemon synchronously holds its non-error transport gate closed. No receiver
write is quarantined when no write outcome is in doubt. If a hardware write may
already have reached the receiver, the same visible-but-gated state is retained
and all further hardware writes are quarantined. The uncertain intent is
discarded rather than replayed. In both cases `Health` reports `recovering` or
`write-quarantined`, and topology and hardware controls stay disabled.

Recovery is automatic. Each attempt starts with a fresh `/info` identity check
bound to the current event session, then requires five identical stable
`/volume` proofs spanning at least three seconds, with no gap greater than one
second. The same physical sink gate is synchronously re-held and revalidated
for every proof. Failures restart the proof window with exponential retry from
250 ms up to 30 seconds. Success clears the fault and write quarantine and
updates the canonical node tuple. When the physical sink is directly demanded,
`Health` nevertheless remains `recovering` and controls remain disabled until
a new request-bound `/now_playing` challenge proves the exact source marker and
PipeWire acknowledges opening that same gate generation. Recovery never treats
its timer, the five volume proofs, or an earlier source observation as
permission to emit audio.

Only a local PipeWire module, node, or unholdable safety-gate failure recreates
the physical sink. Actual endpoint or RAOP loss, an identity mismatch, service
shutdown, and intentional promoted-zone lifecycle retirement withdraw it.
Saved virtual-zone sinks keep their existing topology-owned publication policy:
this physical-sink retention rule does not make an unproved zone sink visible.

For a lone routine `/volume` timeout outside this fault state, in-place retry
still requires the daemon to synchronously hold the exact current non-error
PipeWire safety gate and bind the read to the current confirmed source marker.
The per-speaker status field `volume_read_retry_gated` is true while this
in-place recovery is holding the published sink inert. It returns to false only
after a current fresh stable `/volume` result or removal of the sink.

During the separate bounded direct-sink activation barrier, an active RAOP
cleanup or reconnect may legitimately close and advance the private transport
gate again while demand remains present. The daemon accepts only a compatible
forward closed generation read back synchronously from the same publication.
It invalidates the old release, source-marker, and receiver proof and requires
a fresh `none → confirmed → /volume` proof before releasing the successor; the
original absolute activation deadline is never extended. Any concurrent
receiver write, restoration, reason or publication change remains fail-closed.
SoundTouch may briefly return the preceding inactive source from a
request-bound `/now_playing` read even after the new RAOP marker was confirmed.
An inactive or stopped source therefore remains unproved behind the closed
gate. A non-AirPlay `BUFFERING_STATE` receives one 750 ms, event-driven
transition grace; an AirPlay transition must arrive within it or the sink is
contained. Active foreign playback is rejected immediately. Neither the grace
nor a markerless AirPlay event opens the gate. If SoundTouch enters AirPlay but
temporarily omits the marker, the direct guard coalesces that receiver burst for
500 ms before rotating the marker once under the exact unchanged closed gate.
It waits for the exact locally confirmed successor before issuing another
request-bound `/now_playing` GET. If a markerless event or response crosses that
read, the guard coalesces once more and repeats the GET once for the same
successor. Neither wait changes the marker or extends the absolute activation
deadline. Only an exact marker event or request-bound snapshot is accepted as
receiver ownership proof; a second crossing, a markerless final response, or
any demand, publication, event-session, gate-token, or marker-lineage drift
remains fail-closed. The same post-proof hardware-volume quiet window is still
required before release. Virtual-zone promotion retains the same exact-marker
requirement.
Contained gate mismatches report their phase, safe sequence relation, gate
classes, and whether the publication changed. Opaque publication and source
identities are not included; the control application's row tooltip exposes the
full diagnostic even when its visible subtitle is ellipsized.

An idle per-speaker PipeWire module or node failure recreates only that local
sink. The healthy WAPI event channel and reconciliation timer remain alive;
the replacement receives a new generation cookie and must pass a new
`/info → /volume` publication barrier. A backend-wide PipeWire contract
violation remains fatal. An uncertain POST instead keeps the existing physical
sink gated and enters the automatic identity-and-volume recovery above.

An identical source-marker activation failure repeated three times within 30
seconds opens a per-speaker activation breaker. The third failed sink stays
published but transport-gated and inert, and periodic volume reconciliation
cannot create another publication attempt. After 30 seconds the daemon enters
half-open state: it withdraws that sink and starts exactly one fresh
`/info → /volume` publication barrier. The same failure immediately reopens
the breaker and doubles the next delay to 60, 120, 240, and then at most 300
seconds. Removing the last client link expedites the pending probe; routing
away before the third failure clears the abandoned history. Only a
successfully confirmed current source marker fully closes a half-open breaker
and clears its backoff.

The status JSON exposes `health`, `fault_active`,
`fault_recovery_release_pending`, `fault_disposition`,
`fault_recovery_attempts`, `fault_recovery_stable_proofs`,
`write_quarantined`, `volume_read_retry_gated`, `pipewire_demanded`,
`pipewire_activation_breaker_open`,
`pipewire_activation_breaker_state`, `pipewire_activation_half_open`,
`pipewire_activation_backoff_ms`,
`pipewire_activation_retry_delay_ms`,
`pipewire_activation_failure_count`, and, while retained,
`pipewire_activation_failure_reason`. The last field preserves the module's
bounded printable diagnostic and is removed when the history is reset.

All REST and WebSocket requests use the already verified local IP directly;
the WAPI session explicitly disables the system proxy resolver.

If mDNS withdraws or replaces an endpoint while a POST is outstanding or being
confirmed, the write target is no longer provable. The daemon therefore
withdraws every managed sink and exits with non-restartable `EX_CONFIG` (78).
Any later fatal error is also promoted to 78 while a write quarantine exists,
so an automatic service restart cannot erase containment. Recovery is an
explicit operator restart; the new process never replays the uncertain write
and republishes only after new discovery, exact-device identity verification,
and a stable fresh `/volume` read.

Identical 2-second reconciliation reads update only in-memory freshness.
PipeWire Props and the optional diagnostic cache are written only on change
(the cache also receives a rate-limited freshness checkpoint). A failed or
unstable routine read immediately makes hardware controls read-only; a fresh
stable GET is required before another write. Once the physical-sink fault
policy is active, its stronger five-proof recovery barrier applies instead.

## CLI

```text
soundtouch-pipewire daemon [--config PATH]
soundtouch-pipewire service-v2 [--config PATH]
soundtouch-pipewire direct-v2 [--config PATH] --device MAC
soundtouch-pipewire status [--json]
soundtouch-pipewire doctor [--config PATH] [--json]
soundtouch-pipewire eula show
soundtouch-pipewire eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1
soundtouch-pipewire migrate-stock-discovery [--apply]
```

`migrate-stock-discovery` is a dry run unless `--apply` is passed. It only
recognizes the exact canonical discovery config and creates a timestamped
backup before changing it. Unknown custom configurations are never rewritten.

Discovery is intentionally IPv4-only in this prototype. This avoids publishing
an unroutable link-local IPv6 endpoint without a scope identifier. RAOP and
SoundTouch advertisements must still match on IP address, interface, protocol,
and the verified physical MAC. Resolvers stay active so service-data changes
withdraw and rebuild the endpoint even when Avahi collapses REMOVE/NEW events.
A resolver timeout invalidates the stale service data but keeps that persistent
subscription alive. `service-v2` retains any already published output; a later
Avahi `FOUND` result is the recovery event, with no hardware write caused by
the timeout.
A pending resolver is cancelled before REMOVE processing, keyed by interface,
protocol, name, type, and domain. Avahi client/browser failure withdraws all
discovery records, while `service-v2` retains its receiver actors and public
outputs. It retries discovery every three seconds and reconciles only from
fresh announcements.
