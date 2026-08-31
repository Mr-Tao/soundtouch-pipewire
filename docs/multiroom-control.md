# Multiroom and stereo control

The multiroom control plane is deliberately separate from PipeWire's ordinary
volume and stream-routing interfaces. PipeWire represents audio nodes and
links; Bose zones and SoundTouch 10 LEFT/RIGHT groups are receiver state. The
daemon therefore owns that receiver state and exposes a versioned session
D-Bus API. The optional GTK application is only a client of this API.

## Desired and observed state

Saved stereo pairs and zones are desired state. They are stored in
`$XDG_CONFIG_HOME/soundtouch-pipewire/presets.json` (normally
`~/.config/soundtouch-pipewire/presets.json`) as strict schema version 1.
Writes use a private temporary file, `fsync`, atomic replacement and mode
`0600`. A malformed file, unknown schema member, duplicate identity, dangling
reference or revision conflict leaves the previous in-memory and on-disk state
unchanged.

Stable identities are kept separate from display names:

- a physical speaker is a normalized 12-character hexadecimal `deviceID`;
- a stereo pair and a zone use canonical UUIDs;
- a zone member is either `(speaker, deviceID)` or
  `(stereo-pair, pair UUID)`;
- LEFT and RIGHT are explicit persistent roles;
- the Bose group ID, group master, zone master and live membership are
  volatile observations and are never persisted as desired state.

The daemon reconciles all eligible speakers using fresh `/info`,
`/capabilities`, `/supportedURLs`, `/volume`, `/getZone`, `/getGroup` and
`/now_playing` reads. Reconciliation is also queued after discovery,
availability, zone, group and now-playing changes. Bursts coalesce, while an
event arriving during a running reconciliation records one later dirty pass.

An observed topology is exported only from the current cross-speaker snapshot.
An inconsistent external topology remains visible for diagnosis but cannot be
imported. Import captures the exact consistent snapshot at submission time, so
a queued operation cannot silently import a newer topology under the same
object path.

## D-Bus API

The API version is 1:

```text
bus       io.github.Mr_Tao.SoundTouchPipeWire1
root      /io/github/Mr_Tao/SoundTouchPipeWire1
manager   io.github.Mr_Tao.SoundTouchPipeWire1.Manager
```

The root also implements `org.freedesktop.DBus.ObjectManager`. Managed objects
implement `Speaker`, `ZonePreset`, `StereoPair`, `ObservedTopology` or
`Operation`; the installed introspection XML is
`data/io.github.Mr_Tao.SoundTouchPipeWire1.xml`.

All mutations return an `Operation` immediately. Operations use one global
FIFO, including daemon-initiated reconciliation, so two topology writes never
race each other. A queued client operation may be cancelled; a running
hardware operation is allowed to reach fresh verification or compensating
cleanup and is not cancelled halfway through a receiver mutation.

Shutdown cancels work that has not crossed the first receiver mutation and
allows a short bounded drain. Once an executor has handed off any mutation,
shutdown instead waits for fresh verification or compensating cleanup to
finish; detaching that state would make the physical topology unknowable. The
user unit's 90-second stop timeout is an outer emergency boundary, not a normal
transaction deadline.

Every request carries a canonical UUID. Repeating the same UUID with the same
arguments returns the retained operation; reusing it with different arguments
fails. This idempotency is process-local and lasts only while the operation is
retained. The service retains at least 64 operations and every terminal
operation for at least five minutes, permits at most 128 nonterminal
operations, and does not promise idempotency across daemon restarts.

Clients must treat an ObjectManager owner change as a new generation. They
must recreate both the ObjectManager and root Manager proxies, verify that
both have the same unique owner and require `ApiVersion == 1` before enabling
mutations. An operation path such as `operations/o_1` is not stable across
owner generations; it must be matched together with its `RequestId`.

For `Speaker`, `Available=true` says that the physical RAOP sink remains
published; it does not by itself authorize receiver or topology mutation.
`Health=recovering` means that a transient uncertainty has retained that sink
behind a closed transport gate. `Health=write-quarantined` and
`WriteQuarantined=true` additionally mean that a receiver write had an unknown
outcome. Clients keep the speaker row and its configuration visible in both
states, but enable hardware and topology controls only after health returns to
`active`. These additive meanings remain within `ApiVersion == 1`.

`ZonePreset` exposes topology and audio runtime as separate dimensions.
`State` and `StatusMessage` remain topology-owned: they describe the desired
and observed Bose zone, not whether PipeWire can currently carry audio.
`SinkNodeName`, `AudioState`, and `AudioError` describe the private PipeWire
publication, route/gate pipeline and serialized hardware-volume transaction.
`AudioError` is human-readable diagnostic text and may also explain a
nonterminal waiting or rollback state; clients must not parse it as an enum.

`AudioState` is an open string enum. Version 1 currently emits:

- `unpublished`: no private zone sink is currently published;
- `idle`: the sink is published without client playback demand;
- `demand-waiting`: demand exists but fresh topology, identity or volume
  preconditions are not complete;
- `gate-waiting`: the transport route is prepared or armed but still awaits
  the exact receiver, private-source-marker and gate proof before release;
- `routed`: the verified route and released transport are active;
- `promoted-direct`: an already-owned direct RAOP master is the sole PipeWire
  route and Bose is distributing that exact stream to the verified zone;
- `promoted-dissolving`: direct playback demand ended and guarded transport
  teardown is retaining the lifecycle proof until an audited Bose-zone
  dissolve can run;
- `volume-writing`: a serialized receiver-volume transaction is applying its
  forward writes;
- `volume-rolling-back`: a failed or cancelled volume transaction is running
  compensating writes;
- `volume-verifying`: forward or compensating writes are complete but a fresh
  all-member readback is still being verified; and
- `failed`: the audio path failed closed; `AudioError` carries the current
  diagnostic when available.

These properties are additive and do not change `ApiVersion == 1`. A new
client must tolerate their absence on an older v1 daemon and display an
unknown future `AudioState` token generically instead of disabling the whole
control API. Older clients may ignore the new properties.

## Safety and conflict handling

Before changing Bose topology, the controller resolves only speakers that are
currently identity-verified, event-connected, `active`, and non-quarantined.
Activation additionally starts with a published private RAOP sink and healthy
gate for every participant. After Bose accepts a zone, a follower is allowed
to become control-only: SoundTouch firmware normally withdraws its RAOP
advertisement while leaving the already identity-bound WAPI endpoint alive.
That expected transition retires only the follower's direct sink; topology and
hardware volume verification continue over WAPI. Loss of WAPI, or loss of the
master's RAOP transport, remains a hard availability failure. A mutating
topology request is rejected while any receiver volume write is pending. Once
a topology mutation starts, newly requested volume writes wait at their next
identity barrier until topology verification finishes. Read-only
reconciliation requested during a volume transaction is coalesced and
dispatched only after the volume pipeline becomes idle, so it cannot race a
still-converging receiver tuple.

Transient uncertainty on a physical speaker retains its visible sink behind a
closed transport gate. When a write outcome is unknown, only that member's
receiver writes are quarantined; the other guarded members remain blocked but
are not poisoned by association. Automatic recovery requires a fresh identity
check and five unchanged stable volume proofs over at least three seconds,
revalidating the closed gate for every proof. Only a local PipeWire-object
failure recreates a sink, while actual endpoint/RAOP loss, identity mismatch,
service shutdown, or intentional promoted lifecycle retirement withdraws it.
For a directly demanded physical sink, those receiver proofs are not release
authorization: health remains `recovering` and controls remain disabled until
a new request-bound `/now_playing` challenge proves the exact source marker and
PipeWire acknowledges opening the matching gate generation.

An intent authored on a virtual zone sink has a 15-second lifetime measured
with Linux `CLOCK_BOOTTIME`, so time spent suspended counts toward expiry.
Freshness ordering still uses the monotonic callback barrier. An intent that
expires while preflight or another volume pipeline is blocked is rejected and
the last verified tuple is restored; it cannot be replayed after resume.

Each topology operation performs a fresh multi-speaker preflight. The
`protected` conflict policy refuses to take over an unrelated active zone,
stereo group or source. The schema retains `take-over-on-activation` for a
future handoff transaction, but version 1 currently rejects the same conflict
even when take-over was requested, and the UI keeps that action disabled.
Skipping the idle preflight is not a safe substitute: a desktop AirPlay link
must first complete its guarded PipeWire teardown, while receiver-local sources
need an attributed stop and fresh stable verification. A successful HTTP
response is not completion: the executor always performs fresh GET
verification, including after an ambiguous response, and runs bounded
compensating cleanup on partial failure.

Zone requests sent by the controller to the selected master omit
`senderIsMaster`. That attribute belongs to requests which the master forwards
to a member. `/getZone` verification is reporter-aware: the master may omit
`senderIPAddress` and must account for every slave, while a slave names the
master as sender and may report only its own membership. Unknown members,
duplicate identities and mismatched member addresses are always rejected.

Explicit zone activation first selects an activation-source lease. The daemon
currently issues only `PROMOTE_OWNED_RAOP`: exactly one member must already
have a demanded direct private sink whose current publication, healthy open
gate, confirmed random marker, fresh `AIRPLAY` track, canonical PipeWire tuple
and receiver-volume controller all agree. Two owned sessions are ambiguous and
are rejected. Every follower must also have a demand-initialized but idle
PipeWire transport; a follower link appearing while the activation guard is
held contains the operation. An explicit preferred master must name the owned
member; otherwise it becomes the operation's effective master. The idle lease
remains part of the generic executor contract, but this daemon does not issue
it: an idle `/setZone` was observed to resume firmware-remembered playback and
cannot satisfy the no-surprise-audio policy.

Source selection consumes the controller request's fresh, per-member
`/now_playing` and `/volume` snapshots directly. Their normalized receiver
identities and array order must match the selected peers. WebSocket
`nowPlayingUpdated` fields are notification caches, not request-scoped proof,
so a missing, delayed or stale notification cannot veto an otherwise exact
fresh snapshot and is never rewritten from an HTTP response. Conversely, the
fresh snapshot cannot replace daemon-local publication, demand, gate, marker
or controller-volume lineage; both halves must agree. Selection failures name
a stable predicate key and member index for diagnosis, but never include the
private marker value or gate nonce.

Activation retains every participant's fresh preflight volume/mute tuple.
Before `/setZone`, the daemon reserves every participant's receiver-write
pipeline. The executor first performs a second fresh source preflight at the
mutation boundary. Without yielding afterward, guard preparation rechecks the
leased marker and all daemon-local publication lineage, requires the executor
baseline, canonical PipeWire node and receiver-volume controller to show the
same tuple, and synchronously holds the exact private RAOP transport gate. It
does not consult the WebSocket notification cache. A partial hold does not
mutate topology: acknowledged gates stay closed, every participant gets a new
physical-volume read, and a member whose private sink cannot be proven enters
guarded physical-sink recovery. Failure of that member's local gate or node
instead recreates only its PipeWire object.

After an owned `/setZone`, the executor freshly verifies the requested
topology and reports every observed receiver tuple to the daemon, even when no
volume drift is visible. Normally a promoted activation requires every tuple
to remain exactly at its immutable baseline. There is one deliberately narrow
exception for a Bose startup side effect: exactly one non-master follower may
move from stable `0/0,unmuted` to stable `10/10,unmuted`. The executor accepts
it only when the same full candidate vector appears in two fresh all-member
snapshots separated by at least one second; `9`, `11`, mute drift, a master
change or more than one changed follower is rejected. The accepted final
vector and follower index are explicit result metadata, separate from the
immutable rollback baseline.

The daemon independently derives the same exact candidate while the original
controller and local PipeWire node still show zero. It records every member's
receiver-event, volume, operation and topology-event epoch under a five-second
`CLOCK_BOOTTIME` deadline. This proposal performs no WAPI or PipeWire write:
the receiver has already made the change. The first successful provider phase
commits the final vector for subsequent cleanup; a failure before that point
uses the immutable baseline. Any later epoch, deadline, source, gate,
transport, controller or node drift rejects the proposal. An exactly
concurrent external `0 -> 10` change before the fence is observationally
indistinguishable from the firmware startup floor; this residual ambiguity is
bounded to that one exact tuple and all later external events invalidate it.

All other drift enters compensating cleanup. Guarded cleanup retains the
general monotonic restoration machinery: each phase starts a new
`/info -> /volume` preflight, requires the physical tuple to equal its phase
baseline, and synchronously reissues the idempotent PipeWire hold immediately
before each WAPI mutation. The canonical PipeWire node is pinned to the
reserved pre-activation tuple. Every numeric decrease uses receiver-native
`VOLUME_DOWN` in steps of at most five percentage points, with a fresh
preflight and exact gate re-hold before each click. Recovery never increases or
unmutes a receiver: those directions could amplify Spotify or another
receiver-local source which the RAOP packet gate cannot silence.

The controller then performs another all-member topology, volume and
now-playing verification. The master must retain the exact direct publication,
demand epoch and source marker. A normal follower may have lost only RAOP and
its gate, but must retain the same verified WAPI endpoint and exact fresh
receiver tuple. A startup-floor follower must retain its guarded RAOP
transport through this final proof while its controller and applied PipeWire
node both remain at zero. Only after every member passes does the daemon commit
the follower's controller to ten without changing that node. Every participant
reporting an active source must report the leased `AIRPLAY` marker. Runtime
adoption happens before the reservation is released. The master's existing
physical sink stays the sole PipeWire route, and no virtual zone sink is
published for the promoted mode. Direct follower sinks are retired during
adoption rather than waiting for mDNS withdrawal, and all later publish paths
share the same promoted-follower suppression check. A canonical mute may close
the master's private gate without revoking ownership; the exact marker,
publication and demand epochs, healthy mute gate, and confirmed muted
receiver/PipeWire tuple must still agree.

A failure after an owned `/setZone` retains the original operation error and,
when complete identity-checked snapshots exist, first attempts monotonic
hardware restoration. The controller always performs compensating dissolution,
reads fresh post-cleanup tuples and requests the same monotonic restoration
again because dissolution can itself change receiver volume. Only verified
success, or verified cleanup followed by verified volume recovery, releases the
guard with `RECOVER`. Missing snapshots, an active source other than the exact
leased marker, changed master transport or tuple, write or cleanup failure,
publication failure, or shutdown with an uncertain receiver state releases it
with `CONTAIN`; the daemon drops the reservation and retains each available
participant's physical sink behind its closed gate. A member whose write
outcome is actually unknown is write-quarantined; the other members enter
transient recovery. Automatic fresh proofs recover them independently without
reopening transport from the failed operation.

An idempotent activation never acquires owned-mutation cleanup hooks and never
dissolves a pre-existing zone it does not own. It must still match the selected
activation-source lease; an unowned external source is rejected. The executor
never writes `/volume` directly; all receiver recovery remains in the daemon's
serialized controller.

A stereo pair is atomic: both capable online speakers and consistent LEFT and
RIGHT observations are required. A zone may be activated with complete
logical members that are currently available; missing members are reported as
`degraded` rather than silently rewritten out of the preset. A physical member
of a stereo group is never admitted as an independent zone member.

When the promoted master's last direct PipeWire link disappears, the normal
direct-sink DISARM and receiver proof run first. The promoted-zone lifecycle
token deliberately survives the demand-epoch change and intervening
`nowPlayingUpdated` values; the closed transport means those observations
cannot authorize audio or cleanup. After a successfully recovered teardown, a
reporter-aware verification must start beyond the falling-edge serial and
monotonic-time barriers and prove the saved zone active, consistent and free of
every external source. Only that fresh inactive proof queues an internal,
non-cancellable `dissolve-zone` operation in the same audited FIFO as client
work. The executor reads every participant's `now_playing` once more
immediately before the non-cancellable mutation. At that same boundary it
reads the physical master's current demand tuple synchronously on the PipeWire
loop and requires `demanded=false` at the exact falling-edge generation stored
by the lifecycle token. A queued relink callback or a complete relink/unlink
cycle therefore revokes the old operation. Duplicate nonterminal dissolves for
that zone coalesce. Freshly verified dissolution clears the
promoted state and a returned follower RAOP service must pass a new identity
and volume preflight before its direct sink is republished. An uncertain
teardown is contained and does not silently mutate Bose topology.

Control-only follower retention currently requires a combined RAOP/WAPI
identity proof from the same daemon lifetime. If the daemon starts while a Bose
zone is already active and a follower exposes only WAPI, it cannot yet
bootstrap that endpoint from `/info`; automatic cleanup may therefore remain
unavailable until the zone is dissolved externally and normal discovery
resumes.

The schema already carries `resume_policy` and `auto_heal` so clients do not
need another persistence migration. Version 1 currently applies topology only
through explicit operations and event-driven read-only reconciliation; it does
not automatically resume playback or repair receiver topology merely because
those stored policy fields are enabled.

## Receiver admission policy

Receiver admission is configuration, not Bose topology. The Manager therefore
exports it separately from saved presets and observed zones:

- `ManageAllVerified` is the effective policy used by this daemon process;
- `ConfiguredManageAllVerified` is the last value read from disk;
- `DevicePolicies` maps the last-read configured device IDs to `auto`, `allow`,
  or `block`;
- `ConfigDigest` is the lowercase SHA-256 of those exact on-disk bytes;
- `ConfigWritable` permits controller writes only when the daemon was started
  with its implicit default configuration path;
- `ConfigurationRestartRequired` reports that the current on-disk bytes differ
  from the exact configuration bytes loaded by this daemon process; and
- `ConfigurationError` reports a configuration read or validation failure.

The service refreshes this configured snapshot before every mutation and
again after a mutation attempt. It does not currently monitor arbitrary
external editor changes; those are discovered by the next mutation or daemon
restart. A comment-only rewrite can therefore require a restart according to
the byte-level digest even when admission policy values are unchanged.

The count properties distinguish all configured sections, explicit allows and
explicit blocks. A managed Speaker also reports its effective `PolicyMode`.
`PolicyReason=explicitly-allowed` records the explicit configuration even
during preflight. An automatic speaker reports `verified-auto` only while its
identity, current volume and private sink are verified and available; the
reason is empty while that speaker is provisional or retained offline.

`SetManageAllVerified(request_id, expected_digest, value)` and
`SetDevicePolicy(request_id, expected_digest, device_id, mode)` return normal
serialized Operation objects with kinds `set-manage-all-verified` and
`set-device-policy`. The latter accepts only `auto`, `allow`, or `block`.
`auto` removes only the section's `enabled` key, preserving its other
per-device settings. A stale digest, malformed candidate configuration, or
explicit `--config` fails before replacing the file. Successful replacement
preserves comments and unrelated keys, uses mode 0600, and synchronizes both
the file and its parent directory. A rare parent-directory synchronization
failure happens after the atomic rename: the operation reports the durability
error and republishes the bytes actually visible on disk rather than claiming
that no change occurred.

Configuration mutations deliberately do not alter the running admission set
or withdraw sinks. The daemon reloads the new policy only after restart. This
keeps a policy edit from becoming an unexpected audible routing change and
lets the controller show its impact before restarting the service.

## GTK controller

The optional Rust/GTK 4 client is built with:

```sh
meson setup build -Dcontrol_app=enabled
meson compile -C build
```

It preserves unavailable members while editing, filters new stereo-pair
choices by live capability data, requires confirmations for delete, dissolve
and take-over actions, and follows the returned `Operation` instead of
optimistically changing local state. If the daemon disappears, has an
incompatible API version, or its Manager and ObjectManager owners disagree,
the client keeps the snapshot read-only and disables mutations.
Each mutation is also bound to the unique D-Bus owner generation from which
its inputs were rendered. A stale dialog or row is rejected rather than
allowing its path, revision, or request to cross a daemon restart.

Its Receiver Access view keeps the global automatic-admission switch separate
from topology defaults. Enabling the switch requires confirmation that current
and future compatible receivers on the local network may be published after
restart. Disabling it previews currently published `auto` receivers which will
disappear. Per-device controls expose `auto`, explicit allow and explicit
block without treating the technical identity check as prior user approval.
After a successful edit the UI offers to restart
`soundtouch-pipewire.service` through the user systemd D-Bus manager; it waits
for a different daemon owner to return with no pending byte-level
configuration change instead of optimistically applying the new snapshot. An
accepted restart that does not reach that state within two minutes is reported
with commands for inspecting and restarting the user service manually.

The GTK client does not replace a desktop mixer. It can launch a configured
mixer for ordinary stream routing, while SoundTouch-specific zone and stereo
state remains in this application.

On Arch Linux the controller is shipped separately as
`soundtouch-pipewire-control`. It depends on the exact matching
`soundtouch-pipewire` package revision. Its operations target only the frozen
v1 daemon; the service package can run headlessly without GTK or the
controller.

## Audio data plane

There are two deliberately distinct audio modes. The implemented activation
path promotes one already-owned direct RAOP session: the physical master sink
remains the sole PipeWire route, Bose distributes that stream, and the saved
zone reports `promoted-direct` without publishing a virtual zone sink. The
private `libpipewire-module-soundtouch-zone-sink` path remains available for a
separately proven active topology and publishes a stable virtual sink with a
hidden playback stream. It does not yet activate an idle saved topology by
itself.

Saved but inactive or unavailable zones, inconsistent zones, and zones
reporting an unowned active external source remain `unpublished`; a virtual
sink is withdrawn when fresh verification no longer proves a safe topology. A
physical speaker retained for fault recovery does not relax this rule: it is
not eligible to publish, preserve, or mutate a virtual zone route until its
health returns to `active`. A fresh snapshot may preserve an existing demanded
virtual route only when every
active reporter is `AIRPLAY`, all report the exact marker already proven for
that route, and the current master, participant set, arm identity and gate
generation still match. Such a snapshot never establishes ownership or
republishes an absent sink. The same restriction applies to read-only
reconciliation and to an already matching idempotent topology.

Public volume and mute are logical hardware controls; scalar and software
volumes reaching the adapter are normalized to unity. The playback stream has
no autoconnect target and is passive but initially inactive; the PipeWire core
derives passive links to a selected private RAOP sink from that trusted
module-owned node property. Application links can indicate playback demand
without being drained; until the zone is armed, queued capture buffers provide
backpressure instead of deliberately discarding the beginning of the stream.
Arm explicitly activates playback before capture so the passive route
propagates only this verified activation.

Publication and link activation must remain fail-closed:

1. explicitly establish and freshly verify the Bose topology;
2. start from a fresh confirmed hardware volume and mute tuple;
3. load the private zone module with a fresh publication identity;
4. detect client demand without linking the hidden playback stream;
5. close and prime the selected private RAOP transport;
6. configure the hidden playback output and RAOP input with an exact stereo DSP
   `PortConfig` (`F32P`, two channels, positions `FL FR`), then wait for the
   corresponding generation-bound ports;
7. create and structurally verify explicit `FL`/`FR`-matched links while the
   RAOP transport gate remains closed; the core derives their passive behavior
   from the private playback node and `INIT` is valid while both zone streams
   are deliberately inactive;
8. freshly confirm the exact hardware tuple, send the next exact zone-arm
   sequence and nonce, and wait for both zone nodes to acknowledge
   `armed=true`, both transport links to reach `PAUSED` or `ACTIVE`, and a core
   synchronization barrier;
9. revalidate that the same target publication and non-error RAOP gate
   generation are still closed, rotate the module-generated random source
   marker, wait for its RTSP acknowledgement, then issue and drain a new
   PipeWire core barrier after that confirmation;
10. start a new receiver `/now_playing` read and require exact `AIRPLAY` plus
    the confirmed marker in `track`; then invalidate every older volume read,
    freshly confirm the exact hardware tuple, and require the exact
    non-wrapping PipeWire input generation captured by the proof. The guarded
    backend transaction accepts no outstanding authored echo, applies and
    reads back one exact canonical tuple, drains a core barrier, rechecks
    demand, route, arm, gate, marker and the earlier of the suspend-inclusive
    source and receiver-volume proof deadlines, and releases only that same
    closed gate sequence and nonce together with the confirmed marker sequence
    and value and that absolute deadline. A receiver-volume proof expires
    strictly five seconds after its GET was issued. A preceding adapter tuple
    is tolerated here only when it already equals the confirmed tuple; a
    differing one advances the input generation and rejects the proof. The
    module checks `CLOCK_BOOTTIME` and opens the gate while serialized under
    the pinned RTP data-loop lock; closing and teardown drain that same lock.
    Contract 3 rejects a separate AES67 sender loop which could bypass this
    boundary. The first queued audio may flow only after all three
    acknowledgements.

A newer zone volume, mute or client-demand transition between the ownership
proof and its final receiver-volume read invalidates both. Its input generation
advances on the PipeWire thread before the main-loop callback, so an already
queued event also blocks apply and release. A skipped generation fails closed.
The current gate stays closed, the volume transaction stops the route, and any
later route must complete a new marker challenge before audio can flow.

The random marker is also carried by `nowPlayingUpdated`. An exact current
marker from the physical master or any fresh verified zone member is a no-op.
Only the physical master may start a new challenge; a mismatching or malformed
follower report and every non-AirPlay source immediately disarm and unroute the
zone. A delayed GET callback cannot restore ownership after demand, route, arm,
gate, physical sink, zone publication or challenge generation changes. The
RAOP module also advances the gate generation before an active session reset
clears or replaces its marker. The daemon invalidates the old ownership proof
on that publication and requires a new marker challenge before it can start an
authorizing volume read, so neither an old nor a fresh post-reset read can
release the reconnecting session under the preceding proof.

`Core.Sync` orders the companion's PipeWire connection but is not a global
transaction across clients. The exact-generation guard closes changes already
observed or server-ordered before the final barrier, and the module deadline
closes delayed/suspended command handling. An ordinary separately connected
mixer can still have a request processed after that `Done` and before release.
Closing this final inter-client scheduling window needs a module-owned input
token checked at the same server-side linearization point as RAOP release.
Direct client links to the selected physical sink remain conflicts, and live
release testing deliberately includes rapid volume, mute and demand changes
around the post-proof boundary.

Deactivation reverses the safety boundary: acknowledge the next exact
`armed=false` generation first, then remove the explicit links and drain the
core, and only afterward dissolve the Bose zone, switch its master or unload
the virtual sink. A direct application link to the selected physical RAOP sink
while a virtual route owns it is a conflict and must fail closed.

The zone module alone does not authorize hardware changes, select a target or
make a receiver audible. Those actions remain companion-owned transactions.
Automatic idle-to-zone activation is not enabled. Explicit activation can
promote an already-owned direct session as described above; the virtual path
still uses the same serialized coordinator only after topology and source
ownership have been established independently.
