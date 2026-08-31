# SoundTouch PipeWire Controller

This directory contains the optional GTK 4 controller for
`soundtouch-pipewire`. It is intentionally a separate Cargo project: the
controller talks only to the daemon's versioned session D-Bus ObjectManager
API and does not link to PipeWire, WirePlumber, or the SoundTouch protocol
implementation.

The UI is a reconnect-safe controller for the versioned control API. It lists
speakers, saved zones, stereo pairs, externally observed topology, and recent
operations. It can:

- reconcile authoritative speaker state;
- create and edit zones from currently available speakers and stereo pairs,
  including preferred-master, conflict, resume, and automatic-healing policy;
- activate or dissolve zones; the reserved take-over action remains disabled
  until active-source and topology handoff is implemented safely;
- create and edit stereo-pair presets with explicit LEFT and RIGHT roles, then
  create or dissolve the pair on the speakers;
- import externally observed zones and stereo pairs;
- configure automatic receiver admission and per-device access policies;
- edit daemon-wide default policies; and
- delete presets or cancel operations that are still queued.

Zone activation currently promotes an already playing, companion-owned direct
SoundTouch output. Exactly one zone member must be playing through its private
RAOP sink; an explicit preferred master must be that member. Activating an
entirely idle zone is rejected because Bose firmware can resume a remembered
receiver-local source during `/setZone`. While promoted playback is active the
master's normal sink remains the audio output; the zone row reports its
separate lifecycle state and does not acquire a virtual sink node. After that
direct stream ends, automatic cleanup waits for a fresh all-member inactive
source snapshot, then rechecks both that snapshot and the exact falling-edge
PipeWire demand generation immediately before dissolving the Bose zone.

Every mutating call receives a fresh canonical UUID. Updates and deletes use
the revision from the latest D-Bus snapshot. The UI never changes a preset or
runtime state optimistically: it follows the returned `Operation` object and
waits for ObjectManager/property updates from the daemon. Deletes, dissolves,
and take-over actions require explicit confirmation. Ordinary zone activation
does not. Mutation inputs and cancellations carry the unique D-Bus owner
generation of the row or editor that produced them. The client rejects a
generation mismatch and sends accepted calls directly to that unique owner,
so a stale path or revision cannot cross a daemon restart.

## Build and test

```sh
cargo build --locked --release
cargo test --locked
cargo clippy --locked --all-targets -- -D warnings
```

The build requires Rust, GTK 4 development files, GLib/GIO, gettext, and
`glib-compile-resources`.

When the optional controller is enabled by the top-level Meson project,
`control/meson.build` performs the same locked release build in the Meson build
directory and installs the binary, desktop file, AppStream metadata, icon, and
compiled gettext catalogs. The CSS is embedded in the binary as a GResource.
The Cargo build receives the configured Meson locale directory, so non-`/usr`
installation prefixes work as expected.

The Arch package builds the controller in the same package base as the daemon
but installs it into the separate `soundtouch-pipewire-control` binary package.
Meson install tags keep the two file manifests disjoint; gettext catalogs use
Meson's standard `i18n` tag.

## D-Bus contract

- Bus name: `io.github.Mr_Tao.SoundTouchPipeWire1`
- ObjectManager root: `/io/github/Mr_Tao/SoundTouchPipeWire1`
- Manager interface: `io.github.Mr_Tao.SoundTouchPipeWire1.Manager`, exported
  directly on the ObjectManager root
- Recognized interfaces: `Speaker`, `ZonePreset`, `StereoPair`,
  `ObservedTopology`, and `Operation`, under the bus-name namespace.

The client creates an explicit root Manager proxy alongside the
ObjectManagerClient because an ObjectManager server cannot include its own root
as a managed child. Both proxies use `DO_NOT_AUTO_START`. Opening the
controller therefore never starts the daemon; only an explicit user action
invokes a mutating method. A missing daemon is shown as an offline state and
the same clients follow later owner changes.

Available physical speakers and logical stereo pairs are offered in the zone
editor. An unavailable member already stored in an edited preset remains
visible so it can be removed without silently losing information. Stereo-pair
creation always exposes separate LEFT and RIGHT selectors and refuses the same
speaker in both roles.

A physical speaker also remains visible while its existing sink is safely
retained behind a closed transport gate. `Health=recovering` denotes transient
proof uncertainty; `Health=write-quarantined` together with
`WriteQuarantined=true` denotes an unknown receiver-write outcome. The row and
read-only configuration actions remain available, but hardware and topology
actions involving that speaker are disabled until health returns to `active`.
`Available=true` alone is therefore not an authorization signal. These are
additive `Speaker` property meanings and do not bump API version 1.
For a directly demanded sink, health remains `recovering` after the receiver
tuple has stabilized until a new exact source challenge completes and the
transport reports the matching gate open.

Zone rows keep topology health separate from audio-path health. The
`ZonePreset` properties `State` and `StatusMessage` remain topology-owned.
`SinkNodeName`, additive `AudioState`, and additive `AudioError` report the
private PipeWire sink, route/gate progress, and hardware-volume transaction.
The current open-enum `AudioState` tokens are `unpublished`, `idle`,
`demand-waiting`, `gate-waiting`, `routed`, `volume-writing`,
`promoted-direct`, `promoted-dissolving`, `volume-rolling-back`,
`volume-verifying`, and `failed`. `AudioError` is display-only diagnostic text
and is not parsed as protocol state.

This addition keeps API version 1. The controller treats a missing
`AudioState`/`AudioError` from an older v1 daemon as unavailable information,
and displays an unknown future state token verbatim. Neither case disables
otherwise compatible controls. Topology status and audio status may therefore
both be shown when they describe independent conditions.

### Receiver Access

The separate **Receiver Access** page controls which identity-checked
SoundTouch receivers may be published as PipeWire outputs. The global switch
chooses between automatic admission of verified compatible receivers and an
explicit allowlist. Each known device ID also has an `Automatic`, `Allowed`,
or `Blocked` policy; rows combine currently published `Speaker` objects with
configured IDs that are not currently online.

The client calls a speaker currently published only when its D-Bus state is
online and available, its health is `active`, and receiver writes are not
quarantined. An automatic speaker's `verified-auto` reason is shown as a
current technical verification only in that state; provisional,
fault-recovering, and retained-offline rows remain visible without making that
claim.

Configuration mutations include the latest `ConfigDigest`. The controller
does not update its switch or policy rows optimistically: it restores the
displayed selection immediately, follows the returned `Operation`, and waits
for the daemon's properties to change. Before disabling automatic admission,
the confirmation dialog lists published speakers whose `PolicyMode=auto`
means they will disappear after restart.

The page displays both effective and configured state, the configuration
path, writability and parse errors, explicit policy counts, and whether a
restart is required. The configured values are the daemon's last read of the
file; every mutation refreshes them before comparing its digest, while
arbitrary external edits are otherwise discovered on the next mutation or
restart. Mutations fail closed for an older/incompatible daemon, an explicit
custom configuration, a read-only configuration, an empty digest, or a
configuration error. When a writable default configuration has pending
changes, **Restart Service** calls
`org.freedesktop.systemd1.Manager.RestartUnit` on the session bus and waits for
another unique daemon owner to reconnect with the saved configuration
effective. If that call fails, or the replacement does not arrive within two
minutes, the dialog provides `systemctl --user status` and the equivalent
manual `systemctl --user restart soundtouch-pipewire.service` command.

## Mixer preference

The header can launch `pwvucontrol`, `pavucontrol`, or terminal-based
`wiremix`. Automatic mode tries them in that order. The selected preference is
stored in:

```text
$XDG_CONFIG_HOME/soundtouch-pipewire/control.conf
```

The launcher never passes the command through a shell.

## Translations

Source strings use the `soundtouch-pipewire-control` gettext domain.
`po/POTFILES.in` lists the sources; update the template with an `xgettext`
invocation that includes `--keyword=tr`. At packaging time, compile catalogs
under `/usr/share/locale`.
