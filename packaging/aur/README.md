# Arch/AUR packaging

Status: **v0.1.0-67 desktop-integration candidate; complete package and live
desktop validation is required before publication**.

The split package coexists with the official Arch packages. Both binary
packages have empty `provides`, `conflicts`, and `replaces` arrays and do not
overwrite files owned by `pipewire`, `pipewire-zeroconf`, or
`pulseaudio-rtp`.

## Installed layout

`soundtouch-pipewire` contains the persistent multi-receiver v2 service, the
frozen v1 command for rollback/debugging, and audio integration:

- `/usr/bin/soundtouch-pipewire`
- `/usr/share/dbus-1/interfaces/io.github.Mr_Tao.SoundTouchPipeWire1.xml`
- `/usr/share/dbus-1/interfaces/io.github.Mr_Tao.SoundTouchPipeWire2.xml`
- `/usr/lib/soundtouch-pipewire/pipewire-0.3/libpipewire-module-soundtouch-raop-sink.so`
- `/usr/lib/soundtouch-pipewire/pipewire-0.3/libpipewire-module-soundtouch-zone-sink.so`
- `/usr/lib/systemd/user/soundtouch-pipewire.service`
- `/usr/share/doc/soundtouch-pipewire/soundtouch-pipewire.conf.example`
- `/usr/share/licenses/soundtouch-pipewire/`

`soundtouch-pipewire-control` depends on that exact service package revision
and contains the v2 GTK StatusNotifierItem client. It reads the service's
read-only status API and submits explicit volume/mute changes only through the
matching standard PipeWire Device Route:

- `/usr/bin/soundtouch-pipewire-control`
- `/usr/share/applications/io.github.Mr_Tao.SoundTouchPipeWire.Control.desktop`
- `/usr/share/metainfo/io.github.Mr_Tao.SoundTouchPipeWire.Control.metainfo.xml`
- `/usr/share/icons/hicolor/scalable/apps/io.github.Mr_Tao.SoundTouchPipeWire.Control.svg`
- `/usr/share/locale/cs/LC_MESSAGES/soundtouch-pipewire-control.mo`
- `/usr/share/licenses/soundtouch-pipewire-control/`
- `/etc/xdg/autostart/io.github.Mr_Tao.SoundTouchPipeWire.Control-autostart.desktop`

The user unit sets `PIPEWIRE_MODULE_DIR` only for the companion process:

```text
/usr/lib/soundtouch-pipewire/pipewire-0.3:/usr/lib/pipewire-0.3
```

It does not modify the environment of the normal PipeWire daemon.

## Local source and release-candidate build

Install `pacman-contrib` as well as the packages in `makedepends`; the local
source helper uses `updpkgsums`.

Patches 0002 through 0011 must first be exported from their validated PipeWire
1.6.8 working trees. Patch 0002 provides the guarded external-volume contract;
patch 0003 adds the authenticated DACP listener, callback properties, and
session recovery; patch 0004 adds the opt-in contract-3 transport safety gate,
its sequenced release command, and the required RTP command forwarding; patch
0005 adds the private virtual-zone sink, strict external-volume normalization
and sequenced arm/disarm control; patch 0006 adds the RTSP-confirmed,
rotatable RAOP source marker used to distinguish the companion's AirPlay
session from a foreign one, plus a module-enforced proof deadline whose final
check, gate opening and closing barriers are serialized under the RTP
data-loop lock; patch 0007 adds the nonce-bound explicit demand state,
which makes registry Link demand authoritative and cancels pending RTSP work
before acknowledging idle; patch 0008 sends and acknowledges the initial RAOP
progress metadata before publishing the random transport marker. This provides
the required local ordering barrier, although SoundTouch firmware may still
omit the accepted marker from its WAPI view. Patch 0009 adds contract 5's
paired Device Route revision/adoption fencing and bounded pre-Route Props
quarantine. Patch 0010 adds the contract-2-only
`raop.reconnect.mode=activation` policy used by the v2 direct-output path. It
keeps the Node published after RAOP transport failure and permits one new
connection attempt only after the failed playback activation closes and the
next playback-open edge arrives; Format changes alone do not rearm it. The
module also authors `node.pause-on-idle=true` so idle/active playback edges
remain observable. The default `automatic` mode retains the existing timed
reconnects.
Patch 0011 makes external volume contract 2's Props callback a stateless
canonical-tuple gate. It reapplies only a complete scalar-bearing tuple whose
software gains are already canonical, copies logical `channelVolumes` and
`mute` only from that event, and ignores malformed, non-unity, partial, and
scalar-less follower events. It cannot enter the legacy RAOP volume state
machine or emit RTSP volume. Those managed inputs are semantic no-ops; on
PipeWire 1.6.8 a reentrant empty Props write technically vetoes them before the
adapter can apply the outer Pod, while unrelated Props return untouched. A
failed apply fails the local stream closed. Contracts 3 and 5 and other
volume-control modes remain unchanged.
The private PipeWire module source remains pinned to 1.6.8. Runtime, build,
and test dependencies accept stock PipeWire versions from 1.6.8 up to, but
excluding, 1.7 (Arch epoch 1). The build guard checks this compatibility range
independently of the source archive version. PipeWire 1.6.9 was verified with
the pkgrel-67 modules in the existing isolated Route and stream tests; later
1.6.x releases rely on the upstream series ABI policy and rolling Arch CI,
not individual hardware acceptance. Extending the range requires revalidation.
Updating the stock runtime does not backport upstream RAOP fixes into the
private module; that remains a separate source update.

The runtime requires WirePlumber 0.5.15 or newer; the isolated policy test
verifies save/restore across Device removal and republication in one
WirePlumber process. Contracts 3 and 5 reject a separate AES67 sender loop.
Once all eleven exist, commit all tracked changes and run:

```sh
cd packaging/aur
./prepare-local-source.sh
SOUNDTOUCH_PIPEWIRE_ALLOW_DRAFT_BUILD=1 \
  makepkg -Csf --noconfirm --skippgpcheck --log
```

The helper creates a reproducible archive of the companion, controller, and
license source, updates checksums, and regenerates `.SRCINFO`. Root
`.gitattributes` excludes the packaging tree and itself from that archive;
fixed tar timestamps and a tree-object export prevent packaging-only commits
from changing its bytes. `prepare()` fetches the locked Cargo dependency graph;
the later build and tests force Cargo offline so a missing dependency fails
instead of silently reaching the network. The environment override permits a
local release-candidate build while the explicit public-release gate remains
closed.

## Continuous integration

The GitHub Actions workflow builds both split packages on rolling Arch for
pushes, pull requests, manual runs, and a weekly compatibility check. It uses
an unprivileged builder and the existing package `check()` path: companion
tests, isolated PipeWire/WirePlumber Route and stream tests, seven private
module tests, locked Rust tests, formatting, Clippy, and desktop/AppStream
validation. It also checks the release gate and rejects stale source checksums
or `.SRCINFO` after generating the canonical source archive. Commit source
changes first, run `prepare-local-source.sh`, and commit its metadata updates.
No household speakers or desktop user services are used. A green CI result
does not establish physical-speaker acceptance or verify the published release
asset; publication still requires the release checklist.

The package `check()` function runs every Meson suite through
`tools/run-meson-tests-clean-env.sh`. The helper starts Meson with an
allowlisted environment and private temporary HOME and XDG directories because
Meson otherwise records its inherited environment in `meson-logs`. Continue to
use a clean Arch environment for the complete build: the helper protects Meson
test logs, not arbitrary logging by every build tool.

Pacman has no system-wide equivalent of Gentoo's per-license
`ACCEPT_LICENSE`. The daemon package therefore emits a non-interactive
post-install reminder, installs the exact SoundTouch Connection supplement,
and leaves SoundTouch API access fail-closed until each desktop user explicitly
runs the acceptance command. The root package transaction never records
acceptance on a user's behalf. The project code itself remains available under
MIT without this runtime acceptance.

Before installing, inspect the package:

```sh
pacman -Qip \
  ./soundtouch-pipewire-0.1.0-67-x86_64.pkg.tar.zst \
  ./soundtouch-pipewire-control-0.1.0-67-x86_64.pkg.tar.zst \
  ./soundtouch-pipewire-debug-0.1.0-67-x86_64.pkg.tar.zst
namcap PKGBUILD \
  ./soundtouch-pipewire-0.1.0-67-x86_64.pkg.tar.zst \
  ./soundtouch-pipewire-control-0.1.0-67-x86_64.pkg.tar.zst \
  ./soundtouch-pipewire-debug-0.1.0-67-x86_64.pkg.tar.zst
```

Confirm that `Provides`, `Conflicts With`, and `Replaces` are all `None` and
that the three file lists do not overlap each other or the official PipeWire
packages.

The private PipeWire build explicitly enables Avahi because the DACP callback
listener announces an authenticated `_dacp._tcp` service for each active RAOP
session. `check()` runs the RAOP volume-contract, DACP listener, contract-5
explicit-demand/safety-gate, activation-reconnect, source-marker and zone
volume/arm tests from the package build tree. It also stages both uniquely
named modules in an isolated PipeWire instance and runs
the companion's RAOP and zone lifecycle tests. These test binaries are not
installed. The zone lifecycle uses `pw-cat` from the declared
`pipewire-audio` dependency as its real client stream; none of the tests
requires access to a household speaker. The same `check()` also runs the
controller's locked Rust tests and Clippy checks, validates formatting, and
validates its desktop and AppStream metadata.

During an active session, receiver volume callbacks remain subject to the
companion's fresh SoundTouch `/volume` preflight and serialized write rules.
Playback callbacks are routed only to a uniquely affiliated MPRIS player; an
ambiguous or absent player association fails closed.

Build and test logs can contain inherited environment variables. Run release
builds in a clean environment or clean chroot, keep the clean Meson test helper
enabled, retain only sanitized logs, and never publish raw logs containing
credentials or session tokens.

## First start

Read and explicitly accept the installed SoundTouch Connection supplement:

```sh
soundtouch-pipewire eula show
soundtouch-pipewire eula accept STPW-SOUNDTOUCH-SUPPLEMENT-1
```

Copy and edit the example configuration, then enable the user service:

```sh
install -Dm600 \
  /usr/share/doc/soundtouch-pipewire/soundtouch-pipewire.conf.example \
  "${XDG_CONFIG_HOME:-$HOME/.config}/soundtouch-pipewire/config.ini"
```

Without matching acceptance, `service-v2` exits with `EX_CONFIG` (78).
Usage errors, lock conflicts, and configuration/supplement failures are listed in
`RestartPreventExitStatus`, preventing an operator-action restart loop.

The installed service runs the persistent `service-v2` path. It owns one
discovery client and one PipeWire context, with one independent output actor
for every admitted receiver. For a single-receiver canary, leave that service
stopped and invoke the explicit manual entry point instead; the v1 daemon,
service, and canary use the same instance lock:

```sh
soundtouch-pipewire doctor
soundtouch-pipewire direct-v2 --device 001122aabbcc
```

The selected receiver must be admitted by the same configuration policy. Do
not run the service or canary until `doctor` confirms that the stock SoundTouch
RAOP sink is excluded.

Before the first start, exclude SoundTouch receivers from the stock RAOP
discovery module. The migration command accepts only the exact canonical
configuration, is a dry run by default, and makes a timestamped backup when
applied:

```sh
soundtouch-pipewire migrate-stock-discovery
soundtouch-pipewire migrate-stock-discovery --apply
systemctl --user restart pipewire.service
soundtouch-pipewire doctor
```

Do not enable the companion until `doctor` confirms that the private module is
available and no stock SoundTouch RAOP sink remains. Coexistence is deliberately
blocked: a stock sink can expose a stale or 100% PipeWire value unrelated to the
receiver's physical volume.

The optional per-device `raop-latency-ms` setting defaults to `1500` (plus
PipeWire's fixed 250 ms transport margin). A `500` ms canary can reduce the
audible return delay after unmute, at the cost of less network-jitter margin.
The accepted range is `250` through `10000`; this knob changes transport
latency, not volume authority.

```sh
systemctl --user enable --now soundtouch-pipewire.service
```

The optional desktop package starts its indicator through XDG autostart. Close
hides its window; **Quit** exits only the client. It does not activate, stop, or
restart the backend over D-Bus. To disable autostart for one user, place a
same-named desktop file with `Hidden=true` in that user's XDG autostart
directory.

```sh
sudo pacman -U \
  ./soundtouch-pipewire-0.1.0-67-x86_64.pkg.tar.zst \
  ./soundtouch-pipewire-control-0.1.0-67-x86_64.pkg.tar.zst
soundtouch-pipewire-control
```

## Rollback

```sh
systemctl --user disable --now soundtouch-pipewire.service
```

The controller can be removed independently. Removing the service package first
requires removing its exact-revision controller dependency. Uninstalling both
packages removes only the controller, companion, two private modules, desktop
metadata, and packaged documentation. Per-user configuration, acceptance, and
cache remain available for manual inspection/removal. The official PipeWire
module and zeroconf package are untouched. To restore the previous discovery
behavior, stop the companion, replace `raop-discover.conf` with the timestamped
backup created by `migrate-stock-discovery --apply`, and restart
`pipewire.service`.

## AUR release

Do not copy this directory into an AUR repository until every item in
`RELEASE-CHECKLIST.md` is complete and `./check-release-gate.sh` succeeds.
The exact locally generated source archive must be uploaded as the immutable
GitHub release asset referenced by `PKGBUILD`; do not substitute GitHub's
automatically generated archive, whose bytes and checksum differ.
