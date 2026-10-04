# SoundTouch Volume

This directory contains the GTK 4 desktop client for the persistent
`soundtouch-pipewire` v2 service. The installed binary, application ID,
desktop ID, icon, and gettext domain remain:

- binary `soundtouch-pipewire-control`;
- application and icon ID `io.github.Mr_Tao.SoundTouchPipeWire.Control`;
- desktop ID `io.github.Mr_Tao.SoundTouchPipeWire.Control.desktop`; and
- gettext domain `soundtouch-pipewire-control`.

The application exports a standard StatusNotifierItem and a static
`com.canonical.dbusmenu` menu with Open, About, and Quit. Activating the item
focuses one compact accessible window. Closing the window hides it; Quit exits
only this desktop client. `--background` starts the single application instance
without presenting the window and is used by the XDG autostart entry.

## Authority and safety boundary

The client reads only the v2 session D-Bus status mirror:

- name `io.github.Mr_Tao.SoundTouchPipeWire2`;
- root `/io/github/Mr_Tao/SoundTouchPipeWire2`;
- API version exactly `2`.

It watches the well-known name without activation, resolves one unique owner,
and pins Manager, ObjectManager, and Receiver proxies to that owner. Owner
replacement, object removal, incompatible state, or loss of control capability
discards local optimism and delayed callbacks.

Volume and mute are submitted only as one complete standard PipeWire Device
Route tuple. The client requires exactly one Device and one Node matching the
service-exported names and normalized `soundtouch.device-id`. It never writes
Node Props, PulseAudio, WAPI, or a private D-Bus method. A local PipeWire sync is
dispatch evidence only; a newer D-Bus `ConfirmedRevision` is confirmation.

The per-receiver scheduler provides immediate local presentation, retains at
most one sync in flight and one latest pending tuple, separates dispatch starts
by at least 200 ms, and times out the final gesture after three seconds without
retrying or correcting the receiver. Clean tuple fields are rebased from the
latest confirmed observation immediately before dispatch.

`ControlBusy` is display-only. Unknown lifecycle or discovery values are shown
as degraded and are non-mutating. Initial service absence does not notify. A
service-owner loss persisting for five seconds after a compatible ready
snapshot and its eventual recovery share one stable `GNotification` ID.

## Build and test

The crate requires Rust 1.92, GTK/GIO/GLib 0.22 / GTK-rs 0.11, WirePlumber 0.5,
gettext, and `glib-compile-resources`. `wireplumber.rs` is pinned to the exact
published commit recorded in `Cargo.toml` and `Cargo.lock`.

```sh
cargo fmt --check
cargo check --locked
cargo test --locked
cargo clippy --locked --all-targets -- -D warnings
```

When enabled by the parent Meson project, `control/meson.build` performs the
locked release build and installs the binary, desktop file, AppStream metadata,
icon, translations, and the user-disableable autostart file under
`sysconfdir/xdg/autostart`. A user may disable autostart with a same-basename
desktop file containing `Hidden=true` in their XDG configuration directory.

Manual Xfce validation remains required for panel hosting, accessible window
behavior, notification replacement, exact live PipeWire matching, and confirmed
receiver readback. Automated tests do not contact speakers or the live desktop.
