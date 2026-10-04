// SPDX-License-Identifier: MIT

use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::rc::{Rc, Weak};
use std::time::Duration;

use gio::prelude::*;
use gtk::prelude::*;

use crate::dbus::StatusClient;
use crate::i18n::{self, tr};
use crate::model::{DiscoveryHealth, Lifecycle, ReceiverSnapshot, ServiceSnapshot, ServiceState};
use crate::notifications::{NotificationEvent, NotificationState};
use crate::optimistic::{Dispatch, Failure, Scheduler, Update};
use crate::pipewire::{MatchToken, PipeWireClient};
use crate::sni::{IndicatorState, StatusNotifier};

const APPLICATION_ID: &str = "io.github.Mr_Tao.SoundTouchPipeWire.Control";
const ICON_NAME: &str = "io.github.Mr_Tao.SoundTouchPipeWire.Control";
const NOTIFICATION_ID: &str = "service-status";

pub fn run() -> glib::ExitCode {
    i18n::init();
    gio::resources_register_include!("soundtouch-pipewire-control.gresource")
        .expect("failed to register application resources");

    let application = gtk::Application::builder()
        .application_id(APPLICATION_ID)
        .flags(gio::ApplicationFlags::HANDLES_COMMAND_LINE)
        .build();
    let controller = Rc::new(RefCell::new(None::<Rc<AppState>>));

    let startup_controller = controller.clone();
    application.connect_startup(move |application| {
        install_css();
        let state = AppState::new(application);
        state.install_actions();
        state.start_clients();
        startup_controller.replace(Some(state));
    });

    let activate_controller = controller.clone();
    application.connect_activate(move |_| {
        if let Some(state) = activate_controller.borrow().as_ref() {
            state.present();
        }
    });

    application.connect_command_line(move |application, command_line| {
        let background = command_line
            .arguments()
            .iter()
            .skip(1)
            .any(|argument| argument == "--background");
        if !background {
            application.activate();
        }
        0.into()
    });

    application.run()
}

fn install_css() {
    let Some(display) = gtk::gdk::Display::default() else {
        return;
    };
    let provider = gtk::CssProvider::new();
    provider.load_from_resource("/io/github/Mr_Tao/SoundTouchPipeWire/Control/style.css");
    gtk::style_context_add_provider_for_display(
        &display,
        &provider,
        gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );
}

fn now_ms() -> u64 {
    (glib::monotonic_time().max(0) as u64) / 1_000
}

struct AppState {
    application: gtk::Application,
    _hold: gio::ApplicationHoldGuard,
    window: RefCell<Option<gtk::ApplicationWindow>>,
    service_label: gtk::Label,
    receiver_list: gtk::ListBox,
    rows: RefCell<HashMap<String, Rc<ReceiverRow>>>,
    snapshot: RefCell<ServiceSnapshot>,
    status_client: RefCell<Option<Rc<StatusClient>>>,
    pipewire: RefCell<Option<Rc<PipeWireClient>>>,
    notifier: RefCell<Option<Rc<StatusNotifier>>>,
    notification_state: RefCell<NotificationState>,
    notification_source: RefCell<Option<glib::SourceId>>,
}

impl AppState {
    fn new(application: &gtk::Application) -> Rc<Self> {
        let service_label = gtk::Label::new(Some(&tr("SoundTouch service is unavailable")));
        service_label.set_xalign(0.0);
        service_label.set_wrap(true);
        service_label.add_css_class("status-banner");
        let receiver_list = gtk::ListBox::new();
        receiver_list.set_selection_mode(gtk::SelectionMode::None);
        receiver_list.add_css_class("boxed-list");
        receiver_list.set_placeholder(Some(&gtk::Label::new(Some(&tr(
            "No v2 receivers are currently published.",
        )))));

        Rc::new(Self {
            application: application.clone(),
            _hold: application.hold(),
            window: RefCell::new(None),
            service_label,
            receiver_list,
            rows: RefCell::new(HashMap::new()),
            snapshot: RefCell::new(ServiceSnapshot::absent(0)),
            status_client: RefCell::new(None),
            pipewire: RefCell::new(None),
            notifier: RefCell::new(None),
            notification_state: RefCell::new(NotificationState::default()),
            notification_source: RefCell::new(None),
        })
    }

    fn install_actions(self: &Rc<Self>) {
        let open = gio::SimpleAction::new("open", None);
        let weak = Rc::downgrade(self);
        open.connect_activate(move |_, _| {
            if let Some(state) = weak.upgrade() {
                state.present();
            }
        });
        self.application.add_action(&open);

        let about = gio::SimpleAction::new("about", None);
        let weak = Rc::downgrade(self);
        about.connect_activate(move |_, _| {
            if let Some(state) = weak.upgrade() {
                state.present_about();
            }
        });
        self.application.add_action(&about);

        let quit = gio::SimpleAction::new("quit", None);
        let application = self.application.clone();
        quit.connect_activate(move |_, _| application.quit());
        self.application.add_action(&quit);
        self.application
            .set_accels_for_action("app.quit", &["<Control>q"]);
    }

    fn start_clients(self: &Rc<Self>) {
        let weak = Rc::downgrade(self);
        let pipewire = PipeWireClient::new(move |_| {
            if let Some(state) = weak.upgrade() {
                state.graph_changed();
            }
        });
        self.pipewire.replace(Some(pipewire));

        let weak = Rc::downgrade(self);
        let status_client = StatusClient::new(move |snapshot| {
            if let Some(state) = weak.upgrade() {
                state.apply_snapshot(snapshot);
            }
        });
        status_client.start();
        self.status_client.replace(Some(status_client));

        let weak_open = Rc::downgrade(self);
        let weak_about = Rc::downgrade(self);
        let application = self.application.clone();
        match StatusNotifier::new(
            &self.application,
            move || {
                if let Some(state) = weak_open.upgrade() {
                    state.present();
                }
            },
            move || {
                if let Some(state) = weak_about.upgrade() {
                    state.present_about();
                }
            },
            move || application.quit(),
        ) {
            Ok(notifier) => {
                self.notifier.replace(Some(notifier));
            }
            Err(error) => eprintln!("could not export StatusNotifierItem: {error}"),
        };
    }

    fn present(self: &Rc<Self>) {
        let window = if let Some(window) = self.window.borrow().clone() {
            window
        } else {
            let content = gtk::Box::new(gtk::Orientation::Vertical, 12);
            content.set_margin_top(16);
            content.set_margin_bottom(16);
            content.set_margin_start(16);
            content.set_margin_end(16);

            let title = gtk::Label::new(Some(&tr("SoundTouch Receivers")));
            title.set_xalign(0.0);
            title.add_css_class("title-2");
            content.append(&title);
            content.append(&self.service_label);
            let scrolled = gtk::ScrolledWindow::builder()
                .min_content_width(460)
                .min_content_height(220)
                .vexpand(true)
                .child(&self.receiver_list)
                .build();
            content.append(&scrolled);

            let window = gtk::ApplicationWindow::builder()
                .application(&self.application)
                .title(tr("SoundTouch Volume"))
                .icon_name(ICON_NAME)
                .default_width(520)
                .default_height(320)
                .child(&content)
                .build();
            window.connect_close_request(|window| {
                window.set_visible(false);
                glib::Propagation::Stop
            });
            self.window.replace(Some(window.clone()));
            window
        };
        window.present();
    }

    fn present_about(&self) {
        let dialog = gtk::AboutDialog::builder()
            .program_name(tr("SoundTouch Volume"))
            .logo_icon_name(ICON_NAME)
            .version(env!("CARGO_PKG_VERSION"))
            .comments(tr(
                "Status and direct PipeWire Device Route control for SoundTouch receivers.",
            ))
            .build();
        if let Some(window) = self.window.borrow().as_ref() {
            dialog.set_transient_for(Some(window));
        }
        dialog.present();
    }

    fn apply_snapshot(self: &Rc<Self>, snapshot: ServiceSnapshot) {
        let current_paths = snapshot
            .receivers
            .iter()
            .map(|receiver| receiver.path.clone())
            .collect::<HashSet<_>>();
        let removed = self
            .rows
            .borrow()
            .keys()
            .filter(|path| !current_paths.contains(*path))
            .cloned()
            .collect::<Vec<_>>();
        for path in removed {
            if let Some(row) = self.rows.borrow_mut().remove(&path) {
                row.invalidate();
                self.receiver_list.remove(&row.root);
            }
        }

        for receiver in &snapshot.receivers {
            let service_mutating = snapshot.discovery_health != DiscoveryHealth::Unknown
                && !matches!(snapshot.state, ServiceState::Incompatible { .. });
            let match_token = self
                .pipewire
                .borrow()
                .as_ref()
                .and_then(|pipewire| pipewire.match_token(receiver).ok());
            if let Some(row) = self.rows.borrow().get(&receiver.path).cloned() {
                row.apply_snapshot(
                    snapshot.owner_epoch,
                    receiver.clone(),
                    match_token,
                    service_mutating,
                );
            } else {
                let row = ReceiverRow::new(
                    Rc::downgrade(self),
                    snapshot.owner_epoch,
                    receiver.clone(),
                    match_token,
                    service_mutating,
                );
                self.receiver_list.append(&row.root);
                self.rows.borrow_mut().insert(receiver.path.clone(), row);
            }
        }
        self.snapshot.replace(snapshot);
        self.refresh_summary();
        self.process_notifications(now_ms());
    }

    fn graph_changed(&self) {
        for row in self.rows.borrow().values() {
            row.graph_changed();
        }
        self.refresh_summary();
    }

    fn refresh_summary(&self) {
        let snapshot = self.snapshot.borrow();
        let mutable = self
            .rows
            .borrow()
            .values()
            .filter(|row| row.enabled.get())
            .count();
        let (label, indicator_state) = match &snapshot.state {
            ServiceState::Absent => (
                tr("SoundTouch service is unavailable"),
                IndicatorState::Unavailable,
            ),
            ServiceState::Connecting => (
                tr("Connecting to SoundTouch service…"),
                IndicatorState::Degraded,
            ),
            ServiceState::Incompatible { .. } => (
                tr("The SoundTouch service API is incompatible"),
                IndicatorState::Unavailable,
            ),
            ServiceState::Degraded => (
                if snapshot.detail.is_empty() {
                    tr("SoundTouch discovery is degraded")
                } else {
                    snapshot.detail.clone()
                },
                IndicatorState::Degraded,
            ),
            ServiceState::Ready => {
                let receiver_degraded = snapshot
                    .receivers
                    .iter()
                    .any(|receiver| receiver.lifecycle != Lifecycle::Active);
                let available = format!(
                    "{} {}",
                    tr("Available SoundTouch receivers:"),
                    snapshot.receivers.len()
                );
                let label = if receiver_degraded {
                    format!("{} · {available}", tr("Degraded"))
                } else {
                    available
                };
                let state = if mutable == snapshot.receivers.len() && !receiver_degraded {
                    IndicatorState::Online
                } else {
                    IndicatorState::Degraded
                };
                (label, state)
            }
        };
        self.service_label.set_label(&label);

        let tooltip = format!("{label}; {mutable} {}", tr("controllable"));
        if let Some(notifier) = self.notifier.borrow().as_ref() {
            notifier.update(indicator_state, &tooltip);
        }
    }

    fn dispatch(self: &Rc<Self>, row: &Rc<ReceiverRow>, dispatch: Dispatch) {
        let Some(pipewire) = self.pipewire.borrow().clone() else {
            row.sync_completed(dispatch.id, false);
            return;
        };
        let receiver = row.snapshot.borrow().clone();
        let owner_epoch = row.owner_epoch.get();
        let intent_epoch = row.intent_epoch.get();
        let Some(match_token) = *row.match_token.borrow() else {
            row.sync_completed(dispatch.id, false);
            return;
        };
        let weak = Rc::downgrade(row);
        pipewire.dispatch(&receiver, match_token, dispatch.tuple, move |result| {
            let Some(row) = weak.upgrade() else {
                return;
            };
            if row.owner_epoch.get() == owner_epoch
                && row.intent_epoch.get() == intent_epoch
                && *row.match_token.borrow() == Some(match_token)
            {
                row.sync_completed(dispatch.id, result.is_ok());
            }
        });
    }

    fn process_notifications(self: &Rc<Self>, now: u64) {
        if let Some(source) = self.notification_source.borrow_mut().take() {
            source.remove();
        }
        let snapshot = self.snapshot.borrow();
        let event = self.notification_state.borrow_mut().update(
            snapshot.compatible_ready(),
            snapshot.owner.is_some(),
            now,
        );
        drop(snapshot);
        if let Some(event) = event {
            let notification = match event {
                NotificationEvent::ServiceLost => {
                    let notification =
                        gio::Notification::new(&tr("SoundTouch service unavailable"));
                    notification.set_body(Some(&tr(
                        "The desktop client lost its compatible v2 service connection.",
                    )));
                    notification
                }
                NotificationEvent::ServiceRecovered => {
                    let notification = gio::Notification::new(&tr("SoundTouch service restored"));
                    notification
                        .set_body(Some(&tr("The compatible v2 service is available again.")));
                    notification
                }
            };
            self.application
                .send_notification(Some(NOTIFICATION_ID), &notification);
        }
        let wakeup = self.notification_state.borrow().next_wakeup_ms();
        if let Some(wakeup) = wakeup {
            let weak = Rc::downgrade(self);
            let delay = Duration::from_millis(wakeup.saturating_sub(now).max(1));
            let source = glib::timeout_add_local_once(delay, move || {
                if let Some(state) = weak.upgrade() {
                    state.notification_source.borrow_mut().take();
                    state.process_notifications(now_ms());
                }
            });
            self.notification_source.replace(Some(source));
        }
    }
}

struct ReceiverRow {
    app: Weak<AppState>,
    root: gtk::ListBoxRow,
    name: gtk::Label,
    status: gtk::Label,
    volume: gtk::Scale,
    mute: gtk::CheckButton,
    failure: gtk::Label,
    snapshot: RefCell<ReceiverSnapshot>,
    scheduler: RefCell<Scheduler>,
    owner_epoch: Cell<u64>,
    intent_epoch: Cell<u64>,
    match_token: RefCell<Option<MatchToken>>,
    enabled: Cell<bool>,
    service_mutating: Cell<bool>,
    updating_widgets: Cell<bool>,
    pointer_active: Cell<bool>,
    wake_source: RefCell<Option<glib::SourceId>>,
    gesture_source: RefCell<Option<glib::SourceId>>,
}

impl ReceiverRow {
    fn new(
        app: Weak<AppState>,
        owner_epoch: u64,
        snapshot: ReceiverSnapshot,
        match_token: Option<MatchToken>,
        service_mutating: bool,
    ) -> Rc<Self> {
        let content = gtk::Box::new(gtk::Orientation::Vertical, 8);
        content.set_margin_top(12);
        content.set_margin_bottom(12);
        content.set_margin_start(12);
        content.set_margin_end(12);
        let header = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        let name = gtk::Label::new(Some(&snapshot.display_name));
        name.set_xalign(0.0);
        name.set_hexpand(true);
        name.add_css_class("heading");
        header.append(&name);
        let status = gtk::Label::new(None);
        status.add_css_class("dim-label");
        header.append(&status);
        content.append(&header);

        let controls = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        let volume = gtk::Scale::with_range(gtk::Orientation::Horizontal, 0.0, 100.0, 1.0);
        volume.set_hexpand(true);
        volume.set_draw_value(true);
        volume.set_value(snapshot.confirmed.volume as f64);
        volume.set_tooltip_text(Some(&tr("Hardware volume, 0 to 100")));
        controls.append(&volume);
        let mute = gtk::CheckButton::with_label(&tr("Mute"));
        mute.set_active(snapshot.confirmed.muted);
        controls.append(&mute);
        content.append(&controls);

        let failure = gtk::Label::new(None);
        failure.set_xalign(0.0);
        failure.set_wrap(true);
        failure.add_css_class("error");
        failure.set_visible(false);
        content.append(&failure);
        let root = gtk::ListBoxRow::builder().child(&content).build();

        let row = Rc::new(Self {
            app,
            root,
            name,
            status,
            volume,
            mute,
            failure,
            snapshot: RefCell::new(snapshot.clone()),
            scheduler: RefCell::new(Scheduler::new(
                snapshot.confirmed,
                snapshot.confirmed_revision,
            )),
            owner_epoch: Cell::new(owner_epoch),
            intent_epoch: Cell::new(1),
            match_token: RefCell::new(match_token),
            enabled: Cell::new(false),
            service_mutating: Cell::new(service_mutating),
            updating_widgets: Cell::new(false),
            pointer_active: Cell::new(false),
            wake_source: RefCell::new(None),
            gesture_source: RefCell::new(None),
        });
        row.connect_controls();
        row.refresh_widgets();
        row.refresh_enabled();
        row
    }

    fn connect_controls(self: &Rc<Self>) {
        let weak = Rc::downgrade(self);
        self.volume.connect_value_changed(move |scale| {
            let Some(row) = weak.upgrade() else {
                return;
            };
            if row.updating_widgets.get() || !row.enabled.get() {
                return;
            }
            row.failure.set_visible(false);
            let update = row
                .scheduler
                .borrow_mut()
                .edit_volume(scale.value().round() as u32, now_ms());
            row.process_update(update);
            if !row.pointer_active.get() {
                row.schedule_gesture_end();
            }
        });

        let click = gtk::GestureClick::new();
        let weak = Rc::downgrade(self);
        click.connect_pressed(move |_, _, _, _| {
            if let Some(row) = weak.upgrade() {
                row.pointer_active.set(true);
                row.scheduler.borrow_mut().begin_gesture();
                row.cancel_gesture_end();
            }
        });
        let weak = Rc::downgrade(self);
        click.connect_released(move |_, _, _, _| {
            if let Some(row) = weak.upgrade() {
                row.pointer_active.set(false);
                row.finish_gesture();
            }
        });
        let weak = Rc::downgrade(self);
        click.connect_cancel(move |_, _| {
            if let Some(row) = weak.upgrade() {
                row.pointer_active.set(false);
                row.finish_gesture();
            }
        });
        self.volume.add_controller(click);

        let focus = gtk::EventControllerFocus::new();
        let weak = Rc::downgrade(self);
        focus.connect_leave(move |_| {
            if let Some(row) = weak.upgrade() {
                row.pointer_active.set(false);
                row.finish_gesture();
            }
        });
        self.volume.add_controller(focus);

        let weak = Rc::downgrade(self);
        self.mute.connect_toggled(move |toggle| {
            let Some(row) = weak.upgrade() else {
                return;
            };
            if row.updating_widgets.get() || !row.enabled.get() {
                return;
            }
            row.failure.set_visible(false);
            let edit = row
                .scheduler
                .borrow_mut()
                .edit_mute(toggle.is_active(), now_ms());
            row.process_update(edit);
            row.finish_gesture();
        });
    }

    fn apply_snapshot(
        self: &Rc<Self>,
        owner_epoch: u64,
        snapshot: ReceiverSnapshot,
        match_token: Option<MatchToken>,
        service_mutating: bool,
    ) {
        let previous = self.snapshot.borrow().clone();
        let identity_changed = self.owner_epoch.get() != owner_epoch
            || previous.device_id != snapshot.device_id
            || previous.pipewire_device_name != snapshot.pipewire_device_name
            || previous.pipewire_node_name != snapshot.pipewire_node_name
            || !snapshot.control_available
            || snapshot.lifecycle == Lifecycle::Unknown
            || !service_mutating
            || *self.match_token.borrow() != match_token;
        self.owner_epoch.set(owner_epoch);
        self.match_token.replace(match_token);
        self.service_mutating.set(service_mutating);
        self.snapshot.replace(snapshot.clone());
        self.name.set_label(&snapshot.display_name);
        if identity_changed {
            self.bump_intent_epoch();
            self.scheduler.replace(Scheduler::new(
                snapshot.confirmed,
                snapshot.confirmed_revision,
            ));
            self.cancel_timers();
        } else {
            let update = self.scheduler.borrow_mut().apply_confirmation(
                snapshot.confirmed,
                snapshot.confirmed_revision,
                now_ms(),
            );
            self.process_update(update);
        }
        self.refresh_widgets();
        self.refresh_enabled();
    }

    fn graph_changed(&self) {
        let next_match = self
            .app
            .upgrade()
            .and_then(|app| app.pipewire.borrow().clone())
            .and_then(|pipewire| pipewire.match_token(&self.snapshot.borrow()).ok());
        if *self.match_token.borrow() != next_match {
            self.match_token.replace(next_match);
            self.invalidate();
        }
        self.refresh_enabled();
    }

    fn refresh_enabled(&self) {
        let snapshot = self.snapshot.borrow();
        let exact_match = self.match_token.borrow().is_some();
        let enabled = self.service_mutating.get() && snapshot.mutating_available() && exact_match;
        self.enabled.set(enabled);
        self.volume.set_sensitive(enabled);
        self.mute.set_sensitive(enabled);
        let status = if !snapshot.control_available {
            tr("Unavailable")
        } else if snapshot.lifecycle == Lifecycle::Unknown {
            tr("Unknown service state")
        } else if !exact_match {
            tr("PipeWire match unavailable")
        } else if snapshot.control_busy {
            tr("Busy")
        } else if snapshot.lifecycle == Lifecycle::Degraded {
            tr("Degraded")
        } else {
            tr("Online")
        };
        self.status.set_label(&status);
        self.root
            .set_tooltip_text((!snapshot.detail.is_empty()).then_some(snapshot.detail.as_str()));
        let volume_label = format!(
            "{} — {}",
            tr("Hardware volume, 0 to 100"),
            snapshot.display_name
        );
        let mute_label = format!("{} — {}", tr("Mute"), snapshot.display_name);
        self.root.update_property(&[
            gtk::accessible::Property::Label(&snapshot.display_name),
            gtk::accessible::Property::Description(&snapshot.detail),
        ]);
        self.volume
            .update_property(&[gtk::accessible::Property::Label(&volume_label)]);
        self.mute
            .update_property(&[gtk::accessible::Property::Label(&mute_label)]);
    }

    fn finish_gesture(self: &Rc<Self>) {
        self.cancel_gesture_end();
        let update = self.scheduler.borrow_mut().end_gesture(now_ms());
        self.process_update(update);
    }

    fn schedule_gesture_end(self: &Rc<Self>) {
        self.cancel_gesture_end();
        let weak = Rc::downgrade(self);
        let source = glib::timeout_add_local_once(Duration::from_millis(200), move || {
            if let Some(row) = weak.upgrade() {
                row.gesture_source.borrow_mut().take();
                row.finish_gesture();
            }
        });
        self.gesture_source.replace(Some(source));
    }

    fn cancel_gesture_end(&self) {
        if let Some(source) = self.gesture_source.borrow_mut().take() {
            source.remove();
        }
    }

    fn process_update(self: &Rc<Self>, update: Update) {
        self.refresh_widgets();
        if let Some(failure) = update.failure {
            let message = match failure {
                Failure::Dispatch => tr("The volume request could not be dispatched."),
                Failure::Timeout => tr("The receiver did not confirm the final request."),
            };
            self.failure.set_label(&message);
            self.failure.set_visible(true);
        }
        if let Some(dispatch) = update.dispatch
            && let Some(app) = self.app.upgrade()
        {
            app.dispatch(self, dispatch);
        }
        self.schedule_wakeup();
    }

    fn sync_completed(self: &Rc<Self>, submission_id: u64, success: bool) {
        let update = self
            .scheduler
            .borrow_mut()
            .sync_completed(submission_id, success, now_ms());
        self.process_update(update);
    }

    fn schedule_wakeup(self: &Rc<Self>) {
        if let Some(source) = self.wake_source.borrow_mut().take() {
            source.remove();
        }
        let now = now_ms();
        let wakeup = self.scheduler.borrow().next_wakeup_ms(now);
        if let Some(wakeup) = wakeup {
            let weak = Rc::downgrade(self);
            let source = glib::timeout_add_local_once(
                Duration::from_millis(wakeup.saturating_sub(now).max(1)),
                move || {
                    if let Some(row) = weak.upgrade() {
                        row.wake_source.borrow_mut().take();
                        let update = row.scheduler.borrow_mut().tick(now_ms());
                        row.process_update(update);
                    }
                },
            );
            self.wake_source.replace(Some(source));
        }
    }

    fn refresh_widgets(&self) {
        let display = self.scheduler.borrow().display();
        self.updating_widgets.set(true);
        self.volume.set_value(display.volume as f64);
        self.mute.set_active(display.muted);
        self.updating_widgets.set(false);
    }

    fn invalidate(&self) {
        self.bump_intent_epoch();
        self.scheduler.borrow_mut().invalidate();
        self.cancel_timers();
        self.refresh_widgets();
    }

    fn cancel_timers(&self) {
        self.pointer_active.set(false);
        self.cancel_gesture_end();
        if let Some(source) = self.wake_source.borrow_mut().take() {
            source.remove();
        }
    }

    fn bump_intent_epoch(&self) {
        self.intent_epoch
            .set(self.intent_epoch.get().wrapping_add(1));
    }
}
