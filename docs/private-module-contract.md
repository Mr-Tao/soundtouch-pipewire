# Private PipeWire module contracts

## Private RAOP sink

The daemon deliberately does not overwrite PipeWire's system RAOP module. It
loads `libpipewire-module-soundtouch-raop-sink.so` from its process-local
`PIPEWIRE_MODULE_DIR` only after SoundTouch Web API identity and volume
preflight succeeds.

The private module is the reviewed PipeWire RAOP sink plus the generic external
volume-control patch, the sequenced transport safety gate, and explicit
controller demand. It must accept:

- `raop.volume.contract = 5`
- `raop.volume.control = external`
- `raop.volume.initial = 0.0..1.0`
- `raop.volume.initial.mute = true|false`
- `raop.latency.ms = 250..10000`

For the current two-channel SoundTouch endpoints the companion also supplies
`audio.channels=2` and an explicit `audio.position=[ FL FR ]` module argument.
The position must not be left to the generic auxiliary-channel fallback:
desktop mixers and route negotiation must see an ordinary stereo sink even
when the network receiver has no ALSA-style port or route object.

The companion sets `raop.latency.ms` from the receiver's optional
`raop-latency-ms` policy. The default remains 1500 ms. The RAOP module adds a
fixed 250 ms transport margin and may raise the result further if the receiver
reports a larger `Audio-Latency`. This value is a scheduling/jitter tradeoff,
not part of the volume-safety identity contract, and a configuration change
takes effect only when the sink is republished.

In `external` mode the module must install a complete safe startup Props floor
atomically with `pw_stream_connect()`: its initial `channelVolumes`/`mute` use
the fresh WAPI value, `softVolumes` remain 1.0 for every channel, canonical
`mute` is mirrored into binary `softMute`, and no RTSP volume request is ever
emitted. This floor prevents an unsafe transient while the Route policy starts;
it is not persisted desired state and cannot outrank a restored Device Route.
Numeric gain remains unity and hardware volume remains authoritative.
Contracts 3 and 5 add a
separate transport gate, so the public canonical mute may follow a desktop
unmute immediately while packets remain blocked until the receiver confirms
the exact audible tuple. Contract 5 additionally makes client demand, rather
than the presence of a cached PipeWire `Format`, authoritative for starting or
restarting the network transport. Persisted desired volume/mute belongs to the
paired Device Route; startup Props are not restored into canonical state.

The created node must copy these properties so the daemon can validate it
before returning it to the desktop. The `raop.*` values are module-owned
overrides; the publication ID is a fresh collision-resistant per-attempt
companion value (ownership correlation, not authentication against a hostile
local PipeWire client):

- `raop.device`, `raop.ip`, and `raop.port`
- `raop.volume.contract=5`
- `raop.volume.control=external`
- `device.id=<bound paired Device global ID>`
- `card.profile.device=0` and `device.routes=1`
- `raop.route.revision.initial=1`
- `raop.publication.generation=<nonzero decimal publication generation>`
- `soundtouch.publication-generation=<same generation>`
- `soundtouch.publication-id=<fresh UUID supplied for this add attempt>`
- `raop.demand.contract=1`
- `raop.demand.control=node-command`
- `raop.demand.nonce=<the safety-gate nonce>`
- `raop.demand.sequence=<decimal uint64, initially zero>`
- `raop.demanded=false|true`, initially `false`
- `raop.safety.gate.state=closed|open`
- `raop.safety.gate.sequence=<nonzero decimal uint64>`
- `raop.safety.gate.nonce=<nonzero 16-digit lower-case hexadecimal uint64>`
- `raop.safety.gate.reasons=none|activation[,mute][,error]`
- `raop.source.marker.contract=1`
- `raop.source.marker.state=none|pending|confirmed|error`
- `raop.source.marker.sequence=<nonzero decimal uint64>`
- `raop.source.marker.value=stpw1:<32 lower-case hexadecimal digits>` for
  `pending` and `confirmed`, or an empty-string tombstone otherwise
- `raop.safety.gate.route.revision=<nonzero adopted Route revision>`
- `raop.source.marker.error=<1..160 printable ASCII bytes>`, present only for
  `error` (optional only for compatibility with an older contract-1 module)

The backend derives physical client demand from input `Link` objects, including
structurally verified companion-owned zone routes, but only while the physical
node is prepared (`IDLE`) or processing (`RUNNING`). A dormant client can
retain `INIT` links when the desktop moves it with the default sink; while the
target remains `SUSPENDED`, those links carry no negotiated `Format` and must
not start an activation candidate. The backend assigns every false/true edge a
monotonic local generation so the daemon can reject dropped, reordered, or
contradictory callbacks. If links already appeared and disappeared while the
sink was still being validated, the first callback may be a collapsed later
generation; the module itself nevertheless remains idle at sequence zero until
the daemon has first received the closed gate and empty source-marker baseline.

The daemon then synchronously mirrors each accepted demand state into the
module with an exact successor command:

```json
{"command.id":"raop-demand-state","state":"demanded","sequence":"1","nonce":"0123456789abcdef"}
```

The command sequence is the module-owned demand sequence, independently of the
backend's Link generation. The module accepts only the same tuple as an
idempotent retry or the exact next sequence with the same nonce; stale,
contradictory, skipped, or foreign-nonce commands cannot advance state. A
positive acknowledgement is published while the safety gate remains closed,
and RTSP work starts only when both a `Format` and explicit positive demand are
present. On an idle transition the module first closes and drains the packet
gate, revokes reconnect authority, and synchronously cancels an incomplete
connect/OPTIONS/ANNOUNCE/SETUP exchange or begins teardown of a ready session.
Only then does it publish the exact idle acknowledgement. A delayed or replayed
`Format` event therefore cannot create a receiver session after the final
client link has gone away.

A quick idle-to-demanded transition can overtake the `TEARDOWN` reply from the
previous ready session. The new ARM close and the old session's later
cleanup/reconnect can therefore publish more than one forward closed gate
generation before the new source marker is confirmed. During this bounded
rearm phase the companion synchronously reads back and adopts only compatible
forward gates with the same nonce and reasons. A closed token covers an
unmuted receiver baseline even when it conservatively retains the `mute`
reason, but a muted receiver baseline always requires that reason. Once the
candidate or guard owns a token, a later reason change is not normally
adopted. The exception is the serialized canonical mute sequence observed at
direct activation. The companion synchronously reads back and holds an exact
same-nonce, one-generation transition from closed `activation` to closed
`activation,mute`, then waits for the matching same-scalar canonical
`mute=true` event. After that pair, one immediate unmute may advance the
already closed `activation,mute` token by exactly one generation and pair with
the same-scalar canonical `mute=false` event. The activation proof cannot
advance or release while either pair is incomplete. A paired event rebases the
candidate or direct guard's canonical target, but retains its original
absolute deadline and restarts the full five-GET, three-second receiver quiet
proof under the successor token. A GET begun before that rebase is discarded
regardless of whether it succeeds or fails. A missing, mismatched, or later
additional toggle reaches the existing bounded fail-closed path. This
preserves the observed mute/unmute sequence without exposing audio or
misclassifying arbitrary same-reason transport generations. A direct
deactivation starting from an open unmuted gate remains stricter and rejects
a newly added `mute` reason as possible concurrent user intent. The companion
still requires the old marker's `none` tombstone before accepting a new
`confirmed` marker. That new marker ends that rearm round.

An unmuted numeric Props event may likewise overtake direct activation after
the companion has captured the receiver baseline. A muted event is admitted
only as a strict decrease of at most five percentage points on a receiver
whose policy explicitly enables native muted `VOLUME_DOWN`; exact repeats,
increases and larger shadow-style jumps remain fail-closed. Either form is
admitted only for a single physical direct-sink candidate or guard with no
receiver write in flight, no incomplete mute handoff, and a synchronous hold
of the exact same closed activation token. The event rebases the canonical
target without changing the token or extending the original absolute
deadline. A receiver GET started before the rebase is discarded. A current GET
then supplies the physical baseline: decreases retain the forward-only
native-key recovery path, while an unmuted increase is authorized only when
the last admitted canonical event was itself an increase. That increase still
requires a fresh identity and volume preflight plus another exact synchronous
gate hold immediately before the absolute receiver POST. Neither direction
releases transport; the final tuple must be confirmed and complete the full
five-GET, three-second quiet proof first. Numeric changes during a real
topology transaction, mixing the numeric and mute handoff protocols within one
direct guard (including in the same event), gate drift, cancellation,
restoration already in progress, and uncertain writes remain fail-closed.

The sole numeric-token exception is the daemon's own replay of an idle Device
Route after demand starts. At idle adoption it retains the exact stable tuple,
`reconcile_receiver` decision, sink/publication/revision, and the full current
Route observation token only when desired authority is present and no Route is
pending. The replay is marked internally and is admitted only for the same
demand generation and daemon demand epoch, with an exact current Route token
and candidate. Its synchronous gate hold may equal the candidate token or its
single same-nonce, closed, same-reason, error-free ARM successor; both tokens
must carry the adopted Route revision and the cached token must equal one of
them. A stale or invalid marked replay is consumed as stale daemon work without
becoming receiver intent, while an unmarked external event retains the normal
fail-closed behavior. The retained replay is process-local and is cleared at
enqueue and every publication/fault reset, so restart supplies no authority to
replay an older Route. A Route with `reconcile_receiver=false` is never retained
for hardware reconciliation.

One narrower receiver-ordering rule applies when that exact replay starts from
physical `0,false` and its canonical target is unmuted and above 10. If the
owned direct source first reports Bose's exact `10,false` RECORD startup floor,
the companion may rebase only the physical restoration baseline; the Device
Route and public node remain at the canonical target. A preflight may instead
be the first current read to report that floor. If a request-current floor was
already observed, one crossed preflight result of exactly `0,false` may be
discarded without caching it, but only at the unchanged volume epoch and under
the same demand, sink, source marker, source proof, closed gate, adopted Route
revision, and full Route observation token. The companion then repeats the
identity-to-volume preflight once. A second zero, mute, any other scalar,
epoch or lineage change, a nonzero baseline, target at or below 10, unmarked
event, cancellation, control, or write consumes the allowance and follows the
ordinary fail-closed path. The final target still requires exactly confirmed
restoration and the normal quiet proof before gate release.

The same bounded rearm is also valid when an already-demanded recording
session is cleaned up or reconnects while its direct activation guard is still
pending. Defensively, this also covers PipeWire reporting a compatible
reclosure after a release token without first delivering the matching open
acknowledgement: the new closed generation proves that the old token cannot
authorize the successor session. The normal backend queues that open state
first, so this is a fail-safe for a coalesced or missed property generation,
not the primary reconnect path.
The companion discards that release state and all marker/receiver proof, then
requires another `none` tombstone, a new `confirmed` marker, a marker- and
gate-bound `/volume` read, and the normal stable proof before releasing the new
token. It never extends the original absolute activation deadline, and it does
not enter rearm during a receiver write, restoration, cancellation, or abort.
An open gate, error reason, nonce change, any other reason change,
muted-baseline change, missing tombstone, or unproved marker is fail-closed.

The per-publication nonce is stable for the lifetime of one sink. Every
activation, reconnect, public mute transition, or transport error closes the
gate before the affected packet can be sent. A closure gets a new monotonic
sequence, except that adding an already-held reason is idempotent. An error
reason is latched and cannot be released. Closing and opening state changes are
published on the node; the data-loop gate opens only after the open state has
been published successfully.

An active RAOP session cleanup is the deliberate exception to idempotent
activation holds: it always advances the closed gate generation before
clearing or replacing the source marker. A release token proven for one
recording session therefore cannot authorize its reconnecting successor.

PipeWire registry globals intentionally expose only a small property subset.
The daemon therefore treats an exact pending node name as provisional, binds
the first candidate, and validates the full `pw_node_info` before subscribing
to Props. The info must match every identity field above, the expected node
name/media class, the mirrored `soundtouch.*` route and contract fields, and
the fresh publication UUID. A second same-name global or any later identity
mutation is fatal.

The daemon does not mark the sink ready at registry bind time. It waits up to
two seconds for verified node info and a two-object `SPA_PARAM_Props`
handshake. PipeWire's audio adapter exposes both its canonical outer Props and
internal follower Props. Their enumeration indices are cursors and can change
after a parameter update, so the daemon never treats an index as object
identity. It classifies each scalar-volume record as canonical and only a
complete non-scalar record with the expected channel counts, matching
canonical/soft mute, and unity soft volumes as the follower delimiter. Two scalar
records without such a follower between them are ambiguous and fail closed.

Before ready, the daemon only requires complete, safe canonical and follower
tuples with unity software gain. It does not seed canonical Props from WAPI or
require startup equality. The paired Device Route is the desired/persisted
volume and mute state; receiver WAPI remains observed confirmation.

On a fresh Route with no stored Props, WirePlumber 0.5.15 obtains the initial
volume from `device.routes.default-sink-volume` but supplies desired
`mute=false`; there is no corresponding default-mute Device property. This is
the standard Route selection behavior for contract 5. It remains inert while
the sink is idle, and only active playback demand may reconcile that desired
tuple to the receiver. A Route selection SetParam with no `Props` is a no-op;
partial Props overlay the latest queued, adopting, or committed desired tuple.

The packaged runtime requires WirePlumber 0.5.15 or newer. The isolated policy
test loads `sm-settings`, `sm-objects`, `support.standard-event-source`,
`policy.device.profile`, and `policy.device.routes` in one WirePlumber process;
it does not load default-node or linking policy. A client Route with `save=true`
persists its volume and mute properties. Removing and republishing the Device
under that same process restores the saved tuple through a current Route
SetParam whose `save` field is false. This verifies Device lifecycle restore,
not persistence across a WirePlumber restart.

After publication only a per-event canonical scalar record can generate
hardware-volume intent. Follower and incomplete non-scalar tuples are ignored
because the follower can retain and replay the startup volume after the
canonical control changes. Consecutive scalar records, malformed Props, a
complete but non-finite or mismatched baseline, and software attenuation all
fail closed and are never emitted as user intent. Failure to locate or
initialize the module, any property mismatch, or later module/node
disappearance also fails closed; the daemon never substitutes the stock module.
If a client supplies finite channel values whose mean resolves to the already
observed hardware percentage and leaves mute unchanged, the backend rewrites
every canonical channel to that percentage's exact cubic representation
locally and emits no hardware-volume callback. A different rounded percentage
or mute state remains real receiver intent and follows the guarded hardware
transaction path.

When a muted node receives an explicit canonical unmute, the module closes a
new gate generation before accepting the public transition. The companion
does not write `mute=true` back to the public node, so desktop OSD state does
not bounce. It captures that exact sequence and nonce with a new identity-bound
WAPI read, serializes the receiver write, and requires a fresh stable GET to
confirm the exact final unmuted volume tuple. Only then may it send one
`SPA_NODE_COMMAND_User` command whose `SPA_COMMAND_NODE_extra` string is:

```json
{"command.id":"raop-safety-gate-release","sequence":"42","nonce":"0123456789abcdef","route.revision":"3","publication.generation":"9","demand.sequence":"7","marker.sequence":"17","marker.value":"stpw1:0123456789abcdef0123456789abcdef","proof.deadline.boottime-usec":"123456789"}
```

The module rejects a stale sequence, a mismatched nonce, a public mute, an
inactive stream, a source marker whose confirmed sequence or value differs
from the command, an absent or expired absolute `CLOCK_BOOTTIME` proof
deadline, or any error-latched generation. It checks the deadline immediately
before opening the gate, with `now < deadline` as the strict condition. The
check and atomic open are serialized under the pinned RTP data-loop lock;
successful close, hold and teardown transitions close first and drain that
same lock before publishing or mutating transport state. Contracts 3 and 5
reject `aes67.driver-group`, whose separate PTP sender loop would bypass this
single-loop boundary. For a demanded virtual zone, the companion passes the
exact marker and the earlier of the source-proof and receiver-volume
deadlines. Every receiver-volume proof expires strictly five seconds after its
`/volume` GET was issued according to `CLOCK_BOOTTIME`, so suspend time and
HTTP callback delay count toward its age. For ordinary physical-sink recovery,
the daemon first receives an exact confirmed marker from the live RAOP session
and only then issues the receiver-volume GET. The resulting proof carries that
request-time marker snapshot to the backend, which atomically requires the
closed gate and current module marker to remain exact before sending the
command. A missing, pending or superseded marker is an expected not-ready
result: the gate stays closed without withdrawing an otherwise healthy idle
sink. Both ordinary and zone paths also refuse to release a gate generation
captured before the confirming receiver-volume read. A command is therefore an
acknowledgement of one exact receiver/public-state proof, not a general
permission to pass audio.

Before a companion-owned topology or receiver-volume transaction may mutate
the hardware, the companion can synchronously add an activation hold to the
current publication:

```json
{"command.id":"raop-safety-gate-hold","nonce":"0123456789abcdef"}
```

The module ignores a malformed command or a nonce from another publication.
For the current nonce it adds the activation reason before returning from the
node command. An already-present activation hold is idempotent and retains its
sequence; otherwise the hold closes a new generation. The companion waits for
a PipeWire core barrier and accepts the command only when readback has the same
nonce, is closed without an error reason, contains the activation reason, and
has exactly that monotonic generation behavior. Release and hold commands are
serialized; overlapping commands are rejected.

The source marker identifies that private transport locally and, when the
receiver echoes it, binds the transport to the receiver's `/now_playing` view.
After `RECORD` succeeds and before recording starts, the module generates 128
random bits, advances from `none` to `pending`, and sends
binary `application/x-dmap-tagged` metadata. That request carries its own
`RTP-Info: rtptime=<current RTP timestamp>` header; the request-local header
does not mutate or borrow the RTSP session's shared header table. Both DMAP
`minm` and `asal` contain the exact `stpw1:` marker; `asar` is
`soundtouch-pipewire`. Only an HTTP 200 response advances the same sequence
and value to `confirmed` and allows recording to start. A timeout, malformed
lifecycle, failed request or failed property publication advances to `error`,
publishes the bounded exact failure in `raop.source.marker.error`, and latches
the transport gate closed.
The error tuple and diagnostic remain stable through session cleanup so the
companion cannot lose the initiating failure to a coalesced teardown update.
Session cleanup advances a non-error marker to `none` and publishes its value
as an empty-string tombstone. This is deliberate: client-node export merges a
full current property dictionary into the server-owned node and cannot carry a
deleted dynamic key, so a missing tombstone would retain the preceding
nonempty marker next to `state=none`. Contract-1 readers accept either the
absent legacy value or the empty tombstone, and still reject every nonempty
value outside `pending` and `confirmed`.
The module patch and companion parser are therefore a paired private rollout.
An older contract-1 companion fails closed when it sees the new empty
tombstone and may replace the sink, but cannot use that tuple to open
transport. Upgrading the companion and module together preserves the intended
in-place transition.
The companion accepts an absent diagnostic on an older contract-1 module but
strictly rejects an empty, non-ASCII, non-printable, oversized, or
wrong-state diagnostic.

While an armed zone route is held behind the gate, the companion may rotate a
confirmed live marker with:

```json
{"command.id":"raop-source-marker-rotate","marker.sequence":"42","gate.sequence":"17","gate.nonce":"0123456789abcdef"}
```

All three tokens must equal the current module-owned properties. The module
advances to a fresh random `pending` value, requires another DMAP HTTP 200 and
publishes the exact successor as `confirmed`; stale, concurrent and unsafe
commands are rejected. The companion waits for that exact transition, then
issues and drains a second PipeWire core barrier after confirmation before it
starts a new receiver `/now_playing` GET. Ownership is accepted only when
`source` is exactly `AIRPLAY` and `track` is exactly the new marker. The proof
retains the exact route-arm and gate generation as well as the marker. An exact
WebSocket echo from the master or any fresh verified zone member is a no-op.
Only the physical master may initiate a replacement proof; a mismatching
follower, malformed data or a non-AirPlay event fails closed. A response
superseded by a newer mismatching event, or by any route, arm, gate,
publication or generation change, cannot authorize release. A topology
snapshot may preserve this existing exact-own route but can never establish
ownership or republish an absent sink.

The same exact rotate command is available to a direct physical-sink guard
when the receiver has entered markerless AirPlay and the original confirmed
marker, demand epoch, publication, and closed gate are still current. The
companion permits at most one rotation per activation. It coalesces the
receiver transition for 500 ms, then waits for the exact locally confirmed
successor before issuing a request-bound receiver read. A markerless event or
response that crosses that read is coalesced once more before one final read
for the same successor. Neither wait changes the marker or extends the
absolute activation deadline. Only an exact receiver marker event or
request-bound snapshot establishes ownership; another crossing, a markerless
final response, or any demand, publication, event-session, closed-gate-token,
or marker-lineage drift is fail-closed. The ordinary post-proof receiver-volume
quiet window and deadline-bearing module release remain mandatory. Zone
routing retains the same exact-marker requirement.

Runtime follower enumeration may still show its startup `softMute`; the
canonical scalar adapter record is the live audioconvert data-path state, while
the contract-5 transport gate and explicit-demand state are the final
packet-level and session-lifetime authorities.

For a published physical sink, uncertainty is not itself a publication
lifecycle event. Receiver/source/topology proof loss retains the same node and
publication generation behind a synchronously acknowledged closed gate. An
unknown receiver-write outcome additionally quarantines subsequent hardware
writes. The companion may clear either state only after a fresh current-session
identity check and five identical stable receiver-volume proofs spanning at
least three seconds, with at most one second between proofs; each proof must
also re-hold and revalidate the same safe gate. A local module, node, or gate
failure recreates only the PipeWire object. Actual endpoint or RAOP loss,
identity mismatch, service shutdown, and intentional promoted lifecycle
retirement are the boundaries which withdraw a physical publication.
If that sink is directly demanded, the volume barrier may restore its canonical
tuple but does not authorize gate release. The companion must keep health in
recovery and require a new request-bound exact source-marker challenge followed
by an acknowledgement that the same gate generation opened.

This rule is deliberately specific to physical sinks. It does not change the
topology-owned publication and withdrawal rules of the private zone sink below.

At backend startup a synchronized registry scan also rejects every stock
SoundTouch RAOP node (`raop_sink.Bose-SM2-*`). User-settable contract properties
cannot make that stock node safe. A matching node appearing later is a
backend-wide fatal safety failure: all private sinks are withdrawn and the
service exits.

## Private zone sink

The companion loads
`libpipewire-module-soundtouch-zone-sink.so` only for a saved zone whose
explicitly activated hardware topology and stable receiver volume tuple have
both been freshly verified. A saved but inactive, unavailable or inconsistent
zone is not advertised as an output, and a previously published sink is
withdrawn when fresh verification no longer finds it safely active. The module
receives:

- `zone.id=<canonical UUID>`;
- `zone.publication-id=<fresh ASCII identity>`;
- `zone.volume.initial=0.0..1.0`;
- `zone.volume.initial.mute=true|false`;
- a fixed common channel layout, currently stereo `FL FR`; and
- a Unicode-capable `node.description`.

The public node is `Audio/Sink`, has the stable ASCII name
`soundtouch_zone.<32-lowercase-UUID-hex>` and role `sink`. Its paired hidden
node is `Stream/Output/Audio`, appends `.playback`, has role `playback`, is
passive but initially inactive, and has neither an autoconnect target nor
target restoration. The core derives passive outgoing transport links from
that trusted module-owned node property. Both
nodes must have the same zone UUID, fresh publication identity and these exact
module-owned properties:

- `soundtouch.zone.contract=1`;
- `soundtouch.zone.volume.contract=1`;
- `soundtouch.zone.volume.control=external`;
- `soundtouch.zone.arm.contract=1`;
- `soundtouch.zone.arm.control=node-command`;
- `soundtouch.zone.arm.nonce=<16 lower-case hexadecimal digits>`;
- `soundtouch.zone.arm.sequence=<acknowledged decimal sequence>`; and
- `soundtouch.zone.armed=false|true`.

The public sink starts disarmed with sequence 0 and both internal streams
inactive. Client links can therefore express demand without the module
consuming their first audio buffers. While disarmed, a failed playback trigger
leaves capture buffers queued and applies backpressure; it does not
deliberately drain and discard the beginning of the stream. An accepted arm
generation explicitly activates the passive playback side before capture,
allowing the core-derived passive transport route to activate its target
without external policy.
Disarm first closes capture, then playback, and flushes both.

Public channel volumes and mute are logical receiver controls. Every channel
volume must be finite, in range and equal because SoundTouch exposes one zone
volume. The module always rewrites scalar volume and all `softVolumes` to
unity and `softMute` to false. Duplicate, asymmetric, malformed, non-finite,
out-of-range, ramped or otherwise forged managed Props are rejected and the
last complete safe tuple is reinstalled. These invariants prevent the zone
node from becoming a second software attenuator; they do not authorize a
receiver write.

The companion assigns one nonzero, non-wrapping input generation to every
accepted canonical volume/mute change and every actual client-demand
transition. The PipeWire-thread callback carries that generation before the
main loop may act on the input. A source challenge captures the exact
generation; proof, its post-proof receiver-volume read and release all require
both the processed main-loop generation and the current PipeWire-thread
generation to remain equal. A missing, skipped or exhausted generation fails
closed. Consequently a `true -> false -> true` demand cycle cannot be mistaken
for an unchanged client session merely because its final boolean is the same.

Arming is an explicit sequenced `SPA_NODE_COMMAND_User` command to the public
node. The `SPA_COMMAND_NODE_extra` string is exactly:

```json
{"command.id":"soundtouch-zone-arm-state","state":"armed","sequence":"1","nonce":"0123456789abcdef"}
```

The sequence must be exactly the published sequence plus one and the nonce
must match the current publication. The same already-acknowledged state and
generation is an idempotent retry; stale contradictory, skipped-generation,
mismatched-nonce, malformed and unknown commands are rejected. The companion
does not treat command submission as completion: it waits for both nodes to
publish the exact nonce, sequence and armed state and then drains a PipeWire
core synchronization barrier.

Before arming, the companion must freshly verify the Bose topology and hardware
volume. It configures both route endpoints with the exact stereo DSP
`PortConfig` (`F32P`, two channels, positions `FL FR`), waits for those
generation-bound ports, and creates and verifies explicit channel-matched links
from the hidden playback ports to one private RAOP sink while its transport
gate is closed. It then arms the zone, revalidates that the same RAOP
publication is still held by a closed, non-error gate, and keeps that gate
closed while performing the source-ownership and post-proof volume sequence
below. Queued application audio therefore cannot reach an unverified receiver.

Each transport link is non-lingering and identity-bound to the zone UUID, zone
publication ID, route generation, target RAOP publication ID, target event
cookie, channel and both endpoint port serials. Its passive behavior is
derived by the PipeWire core from the trusted playback node's
`node.passive=true`; the default client link factory strips and does not
republish a requested `link.passive` hint, so that property is not part of the
registry validation contract. The companion accepts the route only after the
registry global and bound `pw_link_info` independently match the retained
identity properties, each `audio.channel` is exactly `FL` or `FR` at both
ends, neither link has failed or unlinked, and a core barrier drains. Links
may remain `INIT` while both streams are deliberately inactive. After arm
activates playback and capture, the companion waits for both links to reach
`PAUSED` or `ACTIVE`, drains another core barrier and revalidates the same
closed RAOP gate. It then rotates and confirms the private source marker,
requires a fresh exact `/now_playing` match, invalidates every volume read
which began before that proof, and starts a new receiver-volume read. Only
that post-proof hardware tuple may release the same closed gate. Its backend
transaction starts only with an empty authored-echo queue, applies the
confirmed tuple, drains its authored barrier and performs an explicit
request-sequence-bound `enum_params` Props readback. An ordinary confirmed
apply may tolerate one replay of the immediately preceding full canonical
tuple because its exact final readback remains the only authority. Guarded
release is stricter: it tolerates that replay only when the preceding tuple
already equals the freshly confirmed tuple. A different preceding tuple is
classified as input, advances the generation and rejects the old proof even
if the adapter later reads back the confirmed tuple. Every other callback is
input or ambiguity. The transaction then revalidates the input generation,
demand, route, arm, gate, source marker and the earlier of the
suspend-inclusive source and receiver-volume proof deadlines before submitting
the deadline-bearing release command; the module checks that same absolute
deadline and opens the gate while serialized under the RTP data-loop lock. A
newer zone volume, mute or demand intent invalidates the proof and its
authorizing volume read before the intent is processed; the route stays gated
until the volume transaction stops it and a later route completes a new
challenge. A duplicate, asymmetric, direct-client or generation-mismatched
route is a conflict, not an alternative topology.

This generation and `Core.Sync` sequence is not a global server transaction
across PipeWire clients. An ordinary mixer or stream client can have a request
processed after the companion's final `Done` but before the module handles the
release command. The module-enforced deadline closes delayed or suspended
handoff, but not this distinct inter-client ordering window. Eliminating it
requires a module-owned input token checked at the same server-side
linearization point as RAOP release. Until that module-owned input token exists,
the guarded
release path must remain private, direct application links to its selected
physical target are conflicts, and the release-race canary in the packaging
checklist remains mandatory.

Teardown is the reverse safety boundary. The companion first sends the next
exact generation with `state=disarmed` and waits for acknowledgement. The
module deactivates capture and flushes both internal streams. Only then may the
companion destroy the explicit transport links, drain the core barrier, switch
the selected RAOP master, dissolve the zone or unload the module. If state
publication fails after an attempted arm, the module closes and flushes
capture before failing the contract.
