use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;
use std::time::Duration;

use gio::prelude::*;
use gtk::prelude::*;

use crate::dbus::{ControlClient, Mutation, SubmittedOperation};
use crate::i18n::{self, tr};
use crate::mixer::{MixerPreference, load_preference, preference_path, resolve, save_preference};
use crate::model::{
    CompatibilityIssue, LogicalMember, ManagedObject, ObjectDetails, ObjectKind,
    ReceiverAccessDetails, ServiceSnapshot, ServiceState, StereoPairDetails, ZoneDetails,
};

const APPLICATION_ID: &str = "io.github.Mr_Tao.SoundTouchPipeWire.Control";
const CONFLICT_POLICIES: &[&str] = &["inherit", "protected", "take-over-on-activation"];
const DEFAULT_CONFLICT_POLICIES: &[&str] = &["protected", "take-over-on-activation"];
const RESUME_POLICIES: &[&str] = &["inherit", "manual", "automatic"];
const DEFAULT_RESUME_POLICIES: &[&str] = &["manual", "automatic"];
const AUTO_HEAL_POLICIES: &[&str] = &["inherit", "disabled", "enabled"];
const DEVICE_POLICY_MODES: &[&str] = &["auto", "allow", "block"];
const SERVICE_RESTART_TIMEOUT: Duration = Duration::from_secs(120);

pub fn run() -> glib::ExitCode {
    i18n::init();
    gio::resources_register_include!("soundtouch-pipewire-control.gresource")
        .expect("failed to register application resources");

    let application = gtk::Application::builder()
        .application_id(APPLICATION_ID)
        .build();
    let controller_anchor = ViewAnchor::default();

    application.connect_startup(|application| {
        install_css();

        let quit = gio::SimpleAction::new("quit", None);
        let application_weak = application.downgrade();
        quit.connect_activate(move |_, _| {
            if let Some(application) = application_weak.upgrade() {
                application.quit();
            }
        });
        application.add_action(&quit);
        application.set_accels_for_action("app.quit", &["<Control>q"]);
    });

    let anchor = controller_anchor.clone();
    application.connect_activate(move |application| {
        if let Some(window) = application.active_window() {
            window.present();
            return;
        }

        let view = ControllerView::new(application);
        let window = view.window.clone();
        let close_anchor = anchor.clone();
        window.connect_close_request(move |_| {
            close_anchor.clear();
            glib::Propagation::Proceed
        });
        anchor.set(view);
        window.present();
    });

    application.connect_shutdown(move |_| controller_anchor.clear());
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

struct PageView {
    kind: ObjectKind,
    list: gtk::ListBox,
}

struct ReceiverAccessView {
    page: gtk::ScrolledWindow,
    configured_switch: gtk::Switch,
    feature_notice: gtk::Label,
    effective_state: gtk::Label,
    pending_state: gtk::Label,
    restart_notice: gtk::Label,
    restart_button: gtk::Button,
    config_path: gtk::Label,
    config_writable: gtk::Label,
    policy_counts: gtk::Label,
    config_error: gtk::Label,
    device_list: gtk::ListBox,
}

#[derive(Clone)]
struct MemberChoice {
    member: LogicalMember,
    label: String,
    available: bool,
}

#[derive(Clone)]
struct SpeakerChoice {
    id: String,
    label: String,
    available: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ReceiverAccessDevice {
    id: String,
    name: String,
    configured_mode: String,
    effective_mode: String,
    policy_reason: String,
    published: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ReceiverAccessImpact {
    id: String,
    name: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct TrackedOperation {
    path: String,
    request_id: String,
    owner_generation: String,
}

struct ObjectPresentation {
    title: String,
    subtitle: String,
    status: Option<String>,
    tooltip: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct HardwareActionAvailability {
    primary: bool,
    take_over: bool,
    dissolve: bool,
}

impl From<SubmittedOperation> for TrackedOperation {
    fn from(operation: SubmittedOperation) -> Self {
        Self {
            path: operation.path,
            request_id: operation.request_id,
            owner_generation: operation.owner_generation,
        }
    }
}

struct ViewAnchor<T> {
    value: Rc<RefCell<Option<Rc<T>>>>,
}

impl<T> ViewAnchor<T> {
    fn set(&self, value: Rc<T>) {
        self.value.replace(Some(value));
    }

    fn clear(&self) {
        self.value.borrow_mut().take();
    }
}

impl<T> Clone for ViewAnchor<T> {
    fn clone(&self) -> Self {
        Self {
            value: self.value.clone(),
        }
    }
}

impl<T> Default for ViewAnchor<T> {
    fn default() -> Self {
        Self {
            value: Rc::new(RefCell::new(None)),
        }
    }
}

impl ReceiverAccessView {
    fn new() -> Self {
        let content = gtk::Box::new(gtk::Orientation::Vertical, 18);
        content.set_margin_top(18);
        content.set_margin_bottom(18);
        content.set_margin_start(18);
        content.set_margin_end(18);

        let title = gtk::Label::new(Some(&tr("Receiver Access")));
        title.set_xalign(0.0);
        title.add_css_class("title-2");
        content.append(&title);

        let feature_notice = gtk::Label::new(None);
        feature_notice.set_xalign(0.0);
        feature_notice.set_wrap(true);
        feature_notice.add_css_class("status-banner");
        feature_notice.add_css_class("warning");
        content.append(&feature_notice);

        let automatic_row = gtk::Box::new(gtk::Orientation::Horizontal, 18);
        automatic_row.add_css_class("access-setting");
        let automatic_labels = gtk::Box::new(gtk::Orientation::Vertical, 4);
        automatic_labels.set_hexpand(true);
        let automatic_title =
            gtk::Label::new(Some(&tr("Automatically add identity-checked speakers")));
        automatic_title.set_xalign(0.0);
        automatic_title.add_css_class("heading");
        automatic_labels.append(&automatic_title);
        let automatic_subtitle = gtk::Label::new(Some(&tr(
            "When off, only explicitly allowed device IDs are published as PipeWire outputs.",
        )));
        automatic_subtitle.set_xalign(0.0);
        automatic_subtitle.set_wrap(true);
        automatic_subtitle.add_css_class("dim-label");
        automatic_labels.append(&automatic_subtitle);
        automatic_row.append(&automatic_labels);
        let configured_switch = gtk::Switch::new();
        configured_switch.set_valign(gtk::Align::Center);
        automatic_row.append(&configured_switch);
        content.append(&automatic_row);

        let state_box = gtk::Box::new(gtk::Orientation::Vertical, 3);
        let effective_state = gtk::Label::new(None);
        effective_state.set_xalign(0.0);
        state_box.append(&effective_state);
        let pending_state = gtk::Label::new(None);
        pending_state.set_xalign(0.0);
        pending_state.add_css_class("dim-label");
        state_box.append(&pending_state);
        content.append(&state_box);

        let restart_box = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        restart_box.add_css_class("restart-notice");
        let restart_notice = gtk::Label::new(None);
        restart_notice.set_xalign(0.0);
        restart_notice.set_wrap(true);
        restart_notice.set_hexpand(true);
        restart_box.append(&restart_notice);
        let restart_button = gtk::Button::with_label(&tr("Restart Service"));
        restart_box.append(&restart_button);
        content.append(&restart_box);

        let details_title = gtk::Label::new(Some(&tr("Configuration")));
        details_title.set_xalign(0.0);
        details_title.add_css_class("title-3");
        content.append(&details_title);

        let details = form_grid();
        let config_path = gtk::Label::new(None);
        config_path.set_xalign(0.0);
        config_path.set_selectable(true);
        config_path.set_wrap(true);
        add_form_row(&details, 0, &tr("Configuration file"), &config_path);
        let config_writable = gtk::Label::new(None);
        config_writable.set_xalign(0.0);
        add_form_row(&details, 1, &tr("GUI changes"), &config_writable);
        let policy_counts = gtk::Label::new(None);
        policy_counts.set_xalign(0.0);
        policy_counts.set_wrap(true);
        add_form_row(&details, 2, &tr("Device policies"), &policy_counts);
        content.append(&details);

        let config_error = gtk::Label::new(None);
        config_error.set_xalign(0.0);
        config_error.set_wrap(true);
        config_error.add_css_class("status-banner");
        config_error.add_css_class("warning");
        content.append(&config_error);

        let devices_title = gtk::Label::new(Some(&tr("Per-speaker policy")));
        devices_title.set_xalign(0.0);
        devices_title.add_css_class("title-3");
        content.append(&devices_title);
        let devices_description = gtk::Label::new(Some(&tr(
            "Automatic follows the global setting. Allowed and Blocked are explicit device-ID overrides.",
        )));
        devices_description.set_xalign(0.0);
        devices_description.set_wrap(true);
        devices_description.add_css_class("dim-label");
        content.append(&devices_description);

        let device_list = gtk::ListBox::new();
        device_list.set_selection_mode(gtk::SelectionMode::None);
        device_list.add_css_class("boxed-list");
        device_list.set_placeholder(Some(&receiver_access_empty_placeholder()));
        content.append(&device_list);

        let page = gtk::ScrolledWindow::builder()
            .vexpand(true)
            .hscrollbar_policy(gtk::PolicyType::Automatic)
            .child(&content)
            .build();

        Self {
            page,
            configured_switch,
            feature_notice,
            effective_state,
            pending_state,
            restart_notice,
            restart_button,
            config_path,
            config_writable,
            policy_counts,
            config_error,
            device_list,
        }
    }
}

struct ControllerView {
    window: gtk::ApplicationWindow,
    banner: gtk::Label,
    operation_status: gtk::Label,
    pages: Vec<PageView>,
    mutation_controls: Vec<gtk::Widget>,
    create_zone_button: gtk::Button,
    create_pair_button: gtk::Button,
    receiver_access: ReceiverAccessView,
    mixer_preference: Cell<MixerPreference>,
    snapshot: RefCell<ServiceSnapshot>,
    tracked_operation: RefCell<Option<TrackedOperation>>,
    operation_status_hide_source: RefCell<Option<glib::SourceId>>,
    service_restart_requested: Cell<bool>,
    service_restart_previous_owner: RefCell<Option<String>>,
    service_restart_timeout_source: RefCell<Option<glib::SourceId>>,
    rendering_receiver_access: Cell<bool>,
    client: RefCell<Option<Rc<ControlClient>>>,
}

impl ControllerView {
    fn new(application: &gtk::Application) -> Rc<Self> {
        let window = gtk::ApplicationWindow::builder()
            .application(application)
            .title(tr("SoundTouch Controller"))
            .default_width(1080)
            .default_height(680)
            .build();
        window.set_icon_name(Some("audio-speakers-symbolic"));

        let header = gtk::HeaderBar::new();
        let title = gtk::Label::new(Some(&tr("SoundTouch Controller")));
        title.add_css_class("title");
        header.set_title_widget(Some(&title));
        window.set_titlebar(Some(&header));

        let refresh_button = gtk::Button::builder()
            .icon_name("view-refresh-symbolic")
            .tooltip_text(tr("Refresh"))
            .build();
        header.pack_start(&refresh_button);

        let reconcile_button = gtk::Button::with_label(&tr("Reconcile"));
        reconcile_button.set_tooltip_text(Some(&tr(
            "Refresh authoritative speaker state and repair safe differences",
        )));
        header.pack_start(&reconcile_button);

        let create_zone_button = gtk::Button::with_label(&tr("New Zone"));
        header.pack_start(&create_zone_button);

        let create_pair_button = gtk::Button::with_label(&tr("New Stereo Pair"));
        header.pack_start(&create_pair_button);

        let open_mixer_button = gtk::Button::builder()
            .label(tr("Open Mixer"))
            .tooltip_text(tr("Open a general PipeWire volume mixer"))
            .build();
        header.pack_end(&open_mixer_button);

        let mixer_labels = [
            tr("Automatic mixer"),
            tr("pwvucontrol"),
            tr("pavucontrol"),
            tr("wiremix (terminal)"),
        ];
        let mixer_label_refs = mixer_labels.iter().map(String::as_str).collect::<Vec<_>>();
        let mixer_dropdown = gtk::DropDown::from_strings(&mixer_label_refs);
        mixer_dropdown.set_tooltip_text(Some(&tr("Preferred general volume mixer")));
        header.pack_end(&mixer_dropdown);

        let defaults_button = gtk::Button::with_label(&tr("Defaults"));
        header.pack_end(&defaults_button);

        let root = gtk::Box::new(gtk::Orientation::Vertical, 0);
        window.set_child(Some(&root));

        let banner = gtk::Label::new(None);
        banner.set_xalign(0.0);
        banner.set_wrap(true);
        banner.add_css_class("status-banner");
        root.append(&banner);

        let operation_status = gtk::Label::new(None);
        operation_status.set_xalign(0.0);
        operation_status.set_wrap(true);
        operation_status.add_css_class("operation-banner");
        operation_status.set_visible(false);
        root.append(&operation_status);
        root.append(&gtk::Separator::new(gtk::Orientation::Horizontal));

        let body = gtk::Box::new(gtk::Orientation::Horizontal, 0);
        body.set_vexpand(true);
        root.append(&body);

        let stack = gtk::Stack::builder()
            .hexpand(true)
            .vexpand(true)
            .transition_type(gtk::StackTransitionType::Crossfade)
            .build();
        let sidebar = gtk::StackSidebar::new();
        sidebar.set_stack(&stack);
        sidebar.set_size_request(190, -1);
        body.append(&sidebar);
        body.append(&gtk::Separator::new(gtk::Orientation::Vertical));
        body.append(&stack);

        let receiver_access = ReceiverAccessView::new();
        let mut pages = Vec::new();
        for kind in ObjectKind::ALL {
            let list = gtk::ListBox::new();
            list.set_selection_mode(gtk::SelectionMode::None);
            list.add_css_class("boxed-list");
            list.set_placeholder(Some(&empty_placeholder(kind)));

            let scrolled = gtk::ScrolledWindow::builder()
                .vexpand(true)
                .hscrollbar_policy(gtk::PolicyType::Automatic)
                .child(&list)
                .build();
            let page_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
            page_box.set_margin_top(18);
            page_box.set_margin_bottom(18);
            page_box.set_margin_start(18);
            page_box.set_margin_end(18);
            page_box.append(&scrolled);

            stack.add_titled(&page_box, Some(kind.interface_suffix()), &page_title(kind));
            pages.push(PageView { kind, list });
            if kind == ObjectKind::Speaker {
                stack.add_titled(
                    &receiver_access.page,
                    Some("ReceiverAccess"),
                    &tr("Receiver Access"),
                );
            }
        }

        let preference_path = preference_path();
        let preference = load_preference(&preference_path);
        mixer_dropdown.set_selected(preference.index());

        let mutation_controls = vec![
            reconcile_button.clone().upcast::<gtk::Widget>(),
            create_zone_button.clone().upcast::<gtk::Widget>(),
            create_pair_button.clone().upcast::<gtk::Widget>(),
            defaults_button.clone().upcast::<gtk::Widget>(),
        ];
        let view = Rc::new(Self {
            window,
            banner,
            operation_status,
            pages,
            mutation_controls,
            create_zone_button: create_zone_button.clone(),
            create_pair_button: create_pair_button.clone(),
            receiver_access,
            mixer_preference: Cell::new(preference),
            snapshot: RefCell::new(ServiceSnapshot::offline()),
            tracked_operation: RefCell::new(None),
            operation_status_hide_source: RefCell::new(None),
            service_restart_requested: Cell::new(false),
            service_restart_previous_owner: RefCell::new(None),
            service_restart_timeout_source: RefCell::new(None),
            rendering_receiver_access: Cell::new(false),
            client: RefCell::new(None),
        });

        let weak = Rc::downgrade(&view);
        refresh_button.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade()
                && let Some(client) = view.client.borrow().as_ref()
            {
                client.refresh();
            }
        });

        let weak = Rc::downgrade(&view);
        reconcile_button.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.submit(Mutation::Reconcile);
            }
        });

        let weak = Rc::downgrade(&view);
        create_zone_button.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.show_zone_editor(None);
            }
        });

        let weak = Rc::downgrade(&view);
        create_pair_button.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.show_pair_editor(None);
            }
        });

        let weak = Rc::downgrade(&view);
        defaults_button.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.show_defaults_editor();
            }
        });

        let weak = Rc::downgrade(&view);
        view.receiver_access
            .configured_switch
            .connect_state_set(move |_, requested| {
                if let Some(view) = weak.upgrade() {
                    if view.rendering_receiver_access.get() {
                        return glib::Propagation::Stop;
                    }
                    view.confirm_manage_all_change(requested);
                }
                glib::Propagation::Stop
            });

        let weak = Rc::downgrade(&view);
        view.receiver_access
            .restart_button
            .connect_clicked(move |_| {
                if let Some(view) = weak.upgrade() {
                    view.restart_service();
                }
            });

        let weak = Rc::downgrade(&view);
        mixer_dropdown.connect_selected_notify(move |dropdown| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            let preference = MixerPreference::from_index(dropdown.selected());
            view.mixer_preference.set(preference);
            if let Err(error) = save_preference(&preference_path, preference) {
                view.show_error(
                    &tr("Could not save the mixer preference"),
                    &error.to_string(),
                );
            }
        });

        let weak = Rc::downgrade(&view);
        open_mixer_button.connect_clicked(move |_| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            let preference = view.mixer_preference.get();
            let Some(command) = resolve(preference) else {
                view.show_error(
                    &tr("No suitable mixer was found"),
                    &tr(
                        "Install pwvucontrol or pavucontrol, or install wiremix together with a terminal emulator.",
                    ),
                );
                return;
            };
            if let Err(error) = command.launch() {
                view.show_error(&tr("Could not open the mixer"), &error.to_string());
            }
        });

        let weak = Rc::downgrade(&view);
        let client = ControlClient::new(move |snapshot| {
            if let Some(view) = weak.upgrade() {
                view.render(snapshot);
            }
        });
        view.client.replace(Some(client.clone()));
        view.render(ServiceSnapshot::offline());
        client.start();

        view
    }

    fn render(self: &Rc<Self>, snapshot: ServiceSnapshot) {
        self.banner.remove_css_class("warning");
        self.banner.remove_css_class("success");
        self.clear_stale_tracked_operation(&snapshot);

        let online = matches!(snapshot.state, ServiceState::Online);
        for control in &self.mutation_controls {
            control.set_sensitive(online);
        }
        if online {
            let can_create_zone = can_create_zone(&snapshot);
            self.create_zone_button.set_sensitive(can_create_zone);
            self.create_zone_button.set_tooltip_text(
                (!can_create_zone)
                    .then(|| tr("No available speaker or stereo-pair member was found."))
                    .as_deref(),
            );

            let can_create_pair = can_create_stereo_pair(&snapshot);
            self.create_pair_button.set_sensitive(can_create_pair);
            self.create_pair_button.set_tooltip_text(
                (!can_create_pair)
                    .then(|| {
                        tr(
                            "At least two available speakers with known stereo-pair capability are required.",
                        )
                    })
                    .as_deref(),
            );
        } else {
            self.create_zone_button.set_tooltip_text(None);
            self.create_pair_button.set_tooltip_text(None);
        }

        match &snapshot.state {
            ServiceState::Online => {
                self.banner
                    .set_text(&tr("Connected to soundtouch-pipewire"));
                self.banner.add_css_class("success");
            }
            ServiceState::Offline => {
                self.banner.set_text(&tr(
                    "soundtouch-pipewire is not running. The controller will reconnect automatically.",
                ));
                self.banner.add_css_class("warning");
            }
            ServiceState::Incompatible(issue) => {
                self.banner.set_text(&compatibility_issue_message(issue));
                self.banner.add_css_class("warning");
            }
            ServiceState::Error(error) => {
                self.banner.set_text(&format!(
                    "{}: {error}",
                    tr("Could not connect to soundtouch-pipewire")
                ));
                self.banner.add_css_class("warning");
            }
        }

        self.update_tracked_operation(&snapshot);
        self.update_service_restart(&snapshot);
        if *self.snapshot.borrow() == snapshot {
            return;
        }
        self.snapshot.replace(snapshot.clone());
        self.render_receiver_access(&snapshot);

        for page in &self.pages {
            while let Some(child) = page.list.first_child() {
                page.list.remove(&child);
            }

            snapshot
                .objects
                .iter()
                .filter(|object| object.kind == page.kind)
                .for_each(|object| page.list.append(&self.object_row(object)));
        }
    }

    fn render_receiver_access(self: &Rc<Self>, snapshot: &ServiceSnapshot) {
        while let Some(child) = self.receiver_access.device_list.first_child() {
            self.receiver_access.device_list.remove(&child);
        }

        let online = matches!(snapshot.state, ServiceState::Online);
        let Some(access) = snapshot.receiver_access.as_ref() else {
            self.rendering_receiver_access.set(true);
            self.receiver_access.configured_switch.set_active(false);
            self.rendering_receiver_access.set(false);
            self.receiver_access.configured_switch.set_sensitive(false);
            self.receiver_access.effective_state.set_text("");
            self.receiver_access.pending_state.set_text("");
            self.receiver_access.restart_notice.set_visible(false);
            self.receiver_access.restart_button.set_visible(false);
            self.receiver_access
                .config_path
                .set_text(&tr("Unavailable"));
            self.receiver_access
                .config_writable
                .set_text(&tr("Unavailable"));
            self.receiver_access
                .policy_counts
                .set_text(&tr("Unavailable"));
            self.receiver_access.config_error.set_visible(false);
            self.receiver_access.feature_notice.set_visible(true);
            let notice = if online {
                tr(
                    "This daemon version does not expose receiver-access settings. Update soundtouch-pipewire to configure them here.",
                )
            } else {
                tr("Receiver-access settings are unavailable while the service is offline.")
            };
            self.receiver_access.feature_notice.set_text(&notice);
            return;
        };

        let mutable = receiver_access_mutations_enabled(snapshot, access);
        self.receiver_access
            .feature_notice
            .set_visible(!access.contract_complete || !access.config_writable || !online);
        if !online {
            self.receiver_access.feature_notice.set_text(&tr(
                "Receiver-access settings are unavailable while the service is offline.",
            ));
        } else if !access.contract_complete {
            self.receiver_access.feature_notice.set_text(&tr(
                "The daemon exposes only part of the receiver-access API. Settings are shown read-only until compatible versions are installed.",
            ));
        } else if !access.config_writable {
            self.receiver_access.feature_notice.set_text(&tr(
                "This service uses a custom or read-only configuration. Edit the shown file manually, then restart the service.",
            ));
        }

        self.rendering_receiver_access.set(true);
        self.receiver_access
            .configured_switch
            .set_active(access.configured_manage_all_verified);
        self.rendering_receiver_access.set(false);
        self.receiver_access
            .configured_switch
            .set_sensitive(mutable);
        self.receiver_access.effective_state.set_text(&format!(
            "{}: {}",
            tr("Effective now"),
            automatic_access_state_label(access.manage_all_verified)
        ));
        let pending = access.restart_required
            || access.manage_all_verified != access.configured_manage_all_verified;
        self.receiver_access.pending_state.set_visible(pending);
        self.receiver_access.pending_state.set_text(&format!(
            "{}: {}",
            tr("Configured after restart"),
            automatic_access_state_label(access.configured_manage_all_verified)
        ));

        self.receiver_access
            .restart_notice
            .set_visible(access.restart_required);
        self.receiver_access.restart_notice.set_text(&tr(
            "Saved receiver-access changes require a service restart.",
        ));
        self.receiver_access
            .restart_button
            .set_visible(access.restart_required);
        self.receiver_access
            .restart_button
            .set_sensitive(mutable && access.restart_required);

        let config_path = if access.config_path.is_empty() {
            tr("Unknown")
        } else {
            access.config_path.clone()
        };
        self.receiver_access.config_path.set_text(&config_path);
        let writable = if access.config_writable {
            tr("Available")
        } else {
            tr("Read-only; edit the file manually")
        };
        self.receiver_access.config_writable.set_text(&writable);
        self.receiver_access.policy_counts.set_text(&format!(
            "{}: {} · {}: {} · {}: {}",
            tr("Total"),
            access.device_policy_count,
            tr("Allowed"),
            access.explicitly_allowed_device_count,
            tr("Blocked"),
            access.explicitly_blocked_device_count,
        ));

        self.receiver_access
            .config_error
            .set_visible(!access.configuration_error.is_empty());
        self.receiver_access.config_error.set_text(&format!(
            "{}: {}",
            tr("Configuration error"),
            access.configuration_error
        ));

        for device in receiver_access_devices(snapshot, access) {
            self.receiver_access
                .device_list
                .append(&self.receiver_access_device_row(&device, mutable));
        }
    }

    fn receiver_access_device_row(
        self: &Rc<Self>,
        device: &ReceiverAccessDevice,
        mutable: bool,
    ) -> gtk::ListBoxRow {
        let row = gtk::ListBoxRow::new();
        row.add_css_class("object-row");
        row.set_activatable(false);

        let content = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        let labels = gtk::Box::new(gtk::Orientation::Vertical, 3);
        labels.set_hexpand(true);
        let title = gtk::Label::new(Some(&device.name));
        title.set_xalign(0.0);
        title.add_css_class("heading");
        labels.append(&title);

        let id = gtk::Label::new(Some(&device.id));
        id.set_xalign(0.0);
        id.set_selectable(true);
        id.add_css_class("dim-label");
        labels.append(&id);

        let mut detail = if device.published {
            format!(
                "{}: {}",
                tr("Effective policy"),
                device_policy_label(&device.effective_mode)
            )
        } else {
            tr("Not currently published")
        };
        if !device.policy_reason.is_empty() {
            detail = append_detail(detail, policy_reason_label(&device.policy_reason));
        }
        let detail = gtk::Label::new(Some(&detail));
        detail.set_xalign(0.0);
        detail.set_wrap(true);
        detail.add_css_class("dim-label");
        labels.append(&detail);
        content.append(&labels);

        let policy_labels = DEVICE_POLICY_MODES
            .iter()
            .map(|mode| device_policy_label(mode))
            .collect::<Vec<_>>();
        let policy_refs = policy_labels.iter().map(String::as_str).collect::<Vec<_>>();
        let dropdown = gtk::DropDown::from_strings(&policy_refs);
        dropdown.set_valign(gtk::Align::Center);
        let configured_index = DEVICE_POLICY_MODES
            .iter()
            .position(|mode| *mode == device.configured_mode);
        if let Some(index) = configured_index {
            dropdown.set_selected(index as u32);
        } else {
            dropdown.set_selected(gtk::INVALID_LIST_POSITION);
            dropdown.set_tooltip_text(Some(&format!(
                "{}: {}",
                tr("Unsupported configured policy"),
                device.configured_mode
            )));
        }
        dropdown.set_sensitive(mutable && configured_index.is_some());

        if let Some(configured_index) = configured_index {
            let weak = Rc::downgrade(self);
            let device_id = device.id.clone();
            let reverting = Rc::new(Cell::new(false));
            let reverting_for_signal = reverting.clone();
            dropdown.connect_selected_notify(move |dropdown| {
                if reverting_for_signal.get() {
                    return;
                }
                let requested_index = dropdown.selected() as usize;
                if requested_index == configured_index {
                    return;
                }
                reverting_for_signal.set(true);
                dropdown.set_selected(configured_index as u32);
                reverting_for_signal.set(false);

                let Some(mode) = DEVICE_POLICY_MODES.get(requested_index) else {
                    return;
                };
                let Some(view) = weak.upgrade() else {
                    return;
                };
                let Some(expected_digest) = view
                    .snapshot
                    .borrow()
                    .receiver_access
                    .as_ref()
                    .filter(|access| {
                        receiver_access_mutations_enabled(&view.snapshot.borrow(), access)
                    })
                    .map(|access| access.config_digest.clone())
                else {
                    view.show_error(
                        &tr("The policy could not be changed"),
                        &tr("Receiver-access configuration is unavailable or read-only."),
                    );
                    return;
                };
                view.submit(Mutation::SetDevicePolicy {
                    expected_digest,
                    device_id: device_id.clone(),
                    mode: (*mode).to_owned(),
                });
            });
        }

        content.append(&dropdown);
        row.set_child(Some(&content));
        row
    }

    fn confirm_manage_all_change(self: &Rc<Self>, requested: bool) {
        let snapshot = self.snapshot.borrow();
        let Some(access) = snapshot.receiver_access.as_ref() else {
            return;
        };
        if requested == access.configured_manage_all_verified {
            return;
        }
        if !receiver_access_mutations_enabled(&snapshot, access) {
            drop(snapshot);
            self.show_error(
                &tr("The setting could not be changed"),
                &tr("Receiver-access configuration is unavailable or read-only."),
            );
            return;
        }

        let expected_digest = access.config_digest.clone();
        let (message, detail, confirm) = if requested {
            (
                tr("Automatically add identity-checked speakers?"),
                tr(
                    "Current and future compatible receivers on the local network may be published after protocol and device-ID checks. This is automatic discovery, not manual approval.",
                ),
                tr("Enable"),
            )
        } else {
            let impacts = automatic_access_impact(&snapshot);
            let detail = receiver_access_disable_detail(&impacts);
            (
                tr("Require explicit receiver approval?"),
                detail,
                tr("Disable Automatic Access"),
            )
        };
        drop(snapshot);
        self.confirm_and_submit(
            &message,
            &detail,
            &confirm,
            Mutation::SetManageAllVerified {
                expected_digest,
                value: requested,
            },
        );
    }

    fn restart_service(self: &Rc<Self>) {
        if self.service_restart_requested.get() {
            return;
        }
        let Some(client) = self.client.borrow().clone() else {
            return;
        };
        let Some(previous_owner) = self.snapshot.borrow().owner_generation.clone() else {
            self.show_error(
                &tr("The service could not be restarted"),
                &tr("The current soundtouch-pipewire owner could not be identified."),
            );
            return;
        };
        self.service_restart_requested.set(true);
        self.service_restart_previous_owner
            .replace(Some(previous_owner.clone()));
        self.cancel_service_restart_timeout();
        self.cancel_operation_status_hide();
        self.operation_status.remove_css_class("success");
        self.operation_status
            .set_text(&tr("Requesting a restart of soundtouch-pipewire…"));
        self.operation_status.set_visible(true);

        let weak = Rc::downgrade(self);
        client.restart_service(&previous_owner, move |result| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            match result {
                Ok(_) => {
                    if !view.service_restart_requested.get() {
                        return;
                    }
                    view.operation_status.set_text(&tr(
                        "Service restart requested. Waiting for soundtouch-pipewire to reconnect…",
                    ));
                    view.schedule_service_restart_timeout();
                }
                Err(error) => {
                    view.service_restart_requested.set(false);
                    view.service_restart_previous_owner.borrow_mut().take();
                    view.cancel_service_restart_timeout();
                    view.operation_status.set_visible(false);
                    view.show_error(
                        &tr("The service could not be restarted"),
                        &format!(
                            "{}\n\n{}\n\nsystemctl --user restart soundtouch-pipewire.service",
                            error,
                            tr("Restart it manually with:")
                        ),
                    );
                }
            }
        });
    }

    fn schedule_service_restart_timeout(self: &Rc<Self>) {
        self.cancel_service_restart_timeout();
        let weak = Rc::downgrade(self);
        let source = glib::timeout_add_local_once(SERVICE_RESTART_TIMEOUT, move || {
            let Some(view) = weak.upgrade() else {
                return;
            };
            view.service_restart_timeout_source.borrow_mut().take();
            if !view.service_restart_requested.replace(false) {
                return;
            }
            view.service_restart_previous_owner.borrow_mut().take();
            view.operation_status.set_visible(false);
            view.show_error(
                &tr("The service did not reconnect in time"),
                &format!(
                    "{}\n\nsystemctl --user status soundtouch-pipewire.service\n\n{}\n\nsystemctl --user restart soundtouch-pipewire.service",
                    tr(
                        "The restart request was accepted, but soundtouch-pipewire did not return within two minutes. Inspect its status with:",
                    ),
                    tr("Restart it manually with:")
                ),
            );
        });
        self.service_restart_timeout_source.replace(Some(source));
    }

    fn cancel_service_restart_timeout(&self) {
        if let Some(source) = self.service_restart_timeout_source.borrow_mut().take() {
            source.remove();
        }
    }

    fn update_service_restart(self: &Rc<Self>, snapshot: &ServiceSnapshot) {
        if !self.service_restart_requested.get() {
            return;
        }
        match &snapshot.state {
            ServiceState::Offline => {
                self.operation_status.set_text(&tr(
                    "soundtouch-pipewire is restarting; waiting for it to reconnect…",
                ));
                self.operation_status.set_visible(true);
            }
            _ if service_restart_completed(
                snapshot,
                self.service_restart_previous_owner.borrow().as_deref(),
            ) =>
            {
                self.service_restart_requested.set(false);
                self.service_restart_previous_owner.borrow_mut().take();
                self.cancel_service_restart_timeout();
                self.operation_status
                    .set_text(&tr("soundtouch-pipewire restarted successfully"));
                self.operation_status.add_css_class("success");
                self.operation_status.set_visible(true);
                self.schedule_operation_status_hide();
            }
            _ => {}
        }
    }

    fn clear_stale_tracked_operation(&self, snapshot: &ServiceSnapshot) {
        let stale = self
            .tracked_operation
            .borrow()
            .as_ref()
            .is_some_and(|tracked| !tracking_generation_is_current(tracked, snapshot));
        if stale {
            self.tracked_operation.borrow_mut().take();
            self.operation_status.set_visible(false);
            self.cancel_operation_status_hide();
        }
    }

    fn update_tracked_operation(self: &Rc<Self>, snapshot: &ServiceSnapshot) {
        let Some(tracked) = self.tracked_operation.borrow().clone() else {
            return;
        };
        let Some(object) = snapshot
            .objects
            .iter()
            .find(|object| object.path == tracked.path)
        else {
            self.operation_status.set_text(&format!(
                "{}: {}",
                tr("Waiting for operation"),
                tracked.path
            ));
            self.operation_status.set_visible(true);
            self.cancel_operation_status_hide();
            return;
        };
        let Some(operation) = object.operation() else {
            return;
        };
        if !tracked_request_matches(&tracked, operation) {
            self.tracked_operation.borrow_mut().take();
            self.operation_status.set_visible(false);
            self.cancel_operation_status_hide();
            return;
        }

        self.operation_status.remove_css_class("warning");
        self.operation_status.remove_css_class("success");
        let hide_after_success = operation.state == "succeeded";
        let message = match operation.state.as_str() {
            "succeeded" => {
                self.operation_status.add_css_class("success");
                format!(
                    "{}: {}",
                    tr("Operation completed"),
                    operation_kind_label(&operation.kind)
                )
            }
            "failed" => {
                self.operation_status.add_css_class("warning");
                format!(
                    "{}: {} — {}",
                    tr("Operation failed"),
                    operation_kind_label(&operation.kind),
                    operation.error_message
                )
            }
            "cancelled" => {
                self.operation_status.add_css_class("warning");
                format!(
                    "{}: {}",
                    tr("Operation cancelled"),
                    operation_kind_label(&operation.kind)
                )
            }
            _ => format!(
                "{}: {} ({})",
                tr("Operation in progress"),
                operation_kind_label(&operation.kind),
                operation_state_label(&operation.state)
            ),
        };
        self.operation_status.set_text(&message);
        self.operation_status.set_visible(true);
        if hide_after_success {
            self.schedule_operation_status_hide();
        } else {
            self.cancel_operation_status_hide();
        }
    }

    fn submit(self: &Rc<Self>, mutation: Mutation) {
        let Some(owner_generation) = self.snapshot.borrow().owner_generation.clone() else {
            self.show_error(
                &tr("The request could not be sent"),
                &tr("The control service is offline."),
            );
            return;
        };
        self.submit_for_owner(mutation, &owner_generation);
    }

    fn submit_for_owner(self: &Rc<Self>, mutation: Mutation, owner_generation: &str) {
        let Some(client) = self.client.borrow().clone() else {
            self.show_error(
                &tr("The request could not be sent"),
                &tr("The control service is offline."),
            );
            return;
        };

        self.cancel_operation_status_hide();
        self.operation_status
            .set_text(&tr("Sending request to soundtouch-pipewire…"));
        self.operation_status.set_visible(true);
        let weak = Rc::downgrade(self);
        client.submit(mutation, owner_generation, move |result| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            match result {
                Ok(operation) => {
                    let operation_path = operation.path.clone();
                    view.tracked_operation
                        .replace(Some(TrackedOperation::from(operation)));
                    view.operation_status
                        .set_text(&format!("{}: {operation_path}", tr("Operation submitted")));
                    if let Some(client) = view.client.borrow().as_ref() {
                        client.refresh();
                    }
                }
                Err(error) => {
                    view.operation_status.set_visible(false);
                    view.show_error(&tr("The request could not be sent"), &error.to_string());
                }
            }
        });
    }

    fn cancel_operation(self: &Rc<Self>, path: &str, request_id: &str, owner_generation: &str) {
        let Some(client) = self.client.borrow().clone() else {
            return;
        };
        self.cancel_operation_status_hide();
        let path = path.to_owned();
        let owner_generation = owner_generation.to_owned();
        let tracked = Some(TrackedOperation {
            path: path.clone(),
            request_id: request_id.to_owned(),
            owner_generation: owner_generation.clone(),
        });
        let weak = Rc::downgrade(self);
        client.cancel_operation(&path, &owner_generation, move |result| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            match result {
                Ok(true) => {
                    view.tracked_operation.replace(tracked.clone());
                    view.operation_status
                        .set_text(&tr("Cancellation requested"));
                    view.operation_status.set_visible(true);
                }
                Ok(false) => view.show_error(
                    &tr("The operation could not be cancelled"),
                    &tr("Only queued operations can be cancelled."),
                ),
                Err(error) => view.show_error(
                    &tr("The operation could not be cancelled"),
                    &error.to_string(),
                ),
            }
        });
    }

    fn schedule_operation_status_hide(self: &Rc<Self>) {
        if self.operation_status_hide_source.borrow().is_some() {
            return;
        }

        let weak = Rc::downgrade(self);
        let source = glib::timeout_add_local_once(Duration::from_secs(6), move || {
            if let Some(view) = weak.upgrade() {
                view.operation_status_hide_source.borrow_mut().take();
                view.operation_status.set_visible(false);
                view.tracked_operation.borrow_mut().take();
            }
        });
        self.operation_status_hide_source.replace(Some(source));
    }

    fn cancel_operation_status_hide(&self) {
        if let Some(source) = self.operation_status_hide_source.borrow_mut().take() {
            source.remove();
        }
    }

    fn confirm_and_submit(
        self: &Rc<Self>,
        message: &str,
        detail: &str,
        confirm_label: &str,
        mutation: Mutation,
    ) {
        let Some(owner_generation) = self.snapshot.borrow().owner_generation.clone() else {
            self.show_error(
                &tr("The request could not be sent"),
                &tr("The control service is offline."),
            );
            return;
        };
        self.confirm_and_submit_for_owner(
            message,
            detail,
            confirm_label,
            mutation,
            owner_generation,
        );
    }

    fn confirm_and_submit_for_owner(
        self: &Rc<Self>,
        message: &str,
        detail: &str,
        confirm_label: &str,
        mutation: Mutation,
        owner_generation: String,
    ) {
        let dialog = gtk::AlertDialog::builder()
            .modal(true)
            .message(message)
            .detail(detail)
            .cancel_button(0)
            .default_button(0)
            .build();
        let cancel = tr("Cancel");
        dialog.set_buttons(&[cancel.as_str(), confirm_label]);
        let weak = Rc::downgrade(self);
        dialog.choose(
            Some(&self.window),
            None::<&gio::Cancellable>,
            move |result| {
                if matches!(result, Ok(1))
                    && let Some(view) = weak.upgrade()
                {
                    view.submit_for_owner(mutation, &owner_generation);
                }
            },
        );
    }

    fn show_zone_editor(self: &Rc<Self>, existing: Option<ManagedObject>) {
        let Some(owner_generation) = self.snapshot.borrow().owner_generation.clone() else {
            self.show_error(
                &tr("The request could not be sent"),
                &tr("The control service is offline."),
            );
            return;
        };
        self.show_zone_editor_for_owner(existing, owner_generation);
    }

    fn show_zone_editor_for_owner(
        self: &Rc<Self>,
        existing: Option<ManagedObject>,
        owner_generation: String,
    ) {
        let existing_zone = existing.as_ref().and_then(ManagedObject::zone).cloned();
        let selected = existing_zone
            .as_ref()
            .map(|zone| zone.members.iter().cloned().collect::<HashSet<_>>())
            .unwrap_or_default();
        let choices = member_choices(&self.snapshot.borrow(), &selected);
        if choices.is_empty() {
            self.show_error(
                &tr("A zone cannot be created"),
                &tr("No available speaker or stereo-pair member was found."),
            );
            return;
        }

        let title = if existing_zone.is_some() {
            tr("Edit Zone")
        } else {
            tr("New Zone")
        };
        let (window, content, save_button) = form_window(&self.window, &title, &tr("Save Zone"));
        let grid = form_grid();
        content.append(&grid);

        let name_entry = gtk::Entry::new();
        name_entry.set_hexpand(true);
        name_entry.set_text(
            existing
                .as_ref()
                .map(|object| object.title.as_str())
                .unwrap_or(""),
        );
        add_form_row(&grid, 0, &tr("Name"), &name_entry);

        let members_box = gtk::Box::new(gtk::Orientation::Vertical, 6);
        let mut checks = Vec::new();
        for choice in &choices {
            let label = if choice.available {
                choice.label.clone()
            } else {
                format!("{} ({})", choice.label, tr("unavailable"))
            };
            let check = gtk::CheckButton::with_label(&label);
            let initially_selected = selected.contains(&choice.member);
            check.set_active(initially_selected);
            check.set_sensitive(choice.available || initially_selected);
            members_box.append(&check);
            checks.push((choice.clone(), check));
        }
        add_form_row(&grid, 1, &tr("Members"), &members_box);

        let preferred_labels = std::iter::once(tr("No preferred master"))
            .chain(choices.iter().map(|choice| choice.label.clone()))
            .collect::<Vec<_>>();
        let preferred_refs = preferred_labels
            .iter()
            .map(String::as_str)
            .collect::<Vec<_>>();
        let preferred_dropdown = gtk::DropDown::from_strings(&preferred_refs);
        if let Some(preferred) = existing_zone
            .as_ref()
            .and_then(|zone| zone.preferred_master.as_ref())
            && let Some(index) = choices
                .iter()
                .position(|choice| &choice.member == preferred)
        {
            preferred_dropdown.set_selected(index as u32 + 1);
        }
        add_form_row(&grid, 2, &tr("Preferred master"), &preferred_dropdown);

        let conflict_dropdown = value_dropdown(
            CONFLICT_POLICIES,
            existing_zone
                .as_ref()
                .map(|zone| zone.conflict_policy.as_str())
                .unwrap_or("inherit"),
        );
        add_form_row(&grid, 3, &tr("Conflict policy"), &conflict_dropdown);

        let resume_dropdown = value_dropdown(
            RESUME_POLICIES,
            existing_zone
                .as_ref()
                .map(|zone| zone.resume_policy.as_str())
                .unwrap_or("inherit"),
        );
        resume_dropdown.set_sensitive(false);
        add_form_row(&grid, 4, &tr("Resume policy"), &resume_dropdown);

        let auto_heal_dropdown = value_dropdown(
            AUTO_HEAL_POLICIES,
            existing_zone
                .as_ref()
                .map(|zone| zone.auto_heal.as_str())
                .unwrap_or("inherit"),
        );
        auto_heal_dropdown.set_sensitive(false);
        add_form_row(&grid, 5, &tr("Automatic healing"), &auto_heal_dropdown);
        content.append(&future_policy_note());

        let weak = Rc::downgrade(self);
        let path_and_revision = existing
            .as_ref()
            .zip(existing_zone.as_ref())
            .map(|(object, zone)| (object.path.clone(), zone.revision));
        let window_for_save = window.downgrade();
        save_button.connect_clicked(move |_| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            let name = name_entry.text().trim().to_owned();
            if name.is_empty() {
                view.show_error(&tr("The zone is incomplete"), &tr("Enter a zone name."));
                return;
            }

            let members = checks
                .iter()
                .filter(|(_, check)| check.is_active())
                .map(|(choice, _)| choice.member.clone())
                .collect::<Vec<_>>();
            if members.is_empty() {
                view.show_error(
                    &tr("The zone is incomplete"),
                    &tr("Select at least one available member."),
                );
                return;
            }

            let preferred_master = match preferred_dropdown.selected() {
                0 => None,
                index => choices
                    .get(index as usize - 1)
                    .map(|choice| choice.member.clone()),
            };
            if preferred_master
                .as_ref()
                .is_some_and(|preferred| !members.contains(preferred))
            {
                view.show_error(
                    &tr("The zone is incomplete"),
                    &tr("The preferred master must also be a selected member."),
                );
                return;
            }

            let conflict_policy = selected_value(CONFLICT_POLICIES, &conflict_dropdown).to_owned();
            let resume_policy = selected_value(RESUME_POLICIES, &resume_dropdown).to_owned();
            let auto_heal = selected_value(AUTO_HEAL_POLICIES, &auto_heal_dropdown).to_owned();
            let mutation = if let Some((path, expected_revision)) = &path_and_revision {
                Mutation::UpdateZone {
                    path: path.clone(),
                    expected_revision: *expected_revision,
                    name,
                    members,
                    preferred_master,
                    conflict_policy,
                    resume_policy,
                    auto_heal,
                }
            } else {
                Mutation::CreateZone {
                    name,
                    members,
                    preferred_master,
                    conflict_policy,
                    resume_policy,
                    auto_heal,
                }
            };
            if let Some(window) = window_for_save.upgrade() {
                window.close();
            }
            view.submit_for_owner(mutation, &owner_generation);
        });
        window.present();
    }

    fn show_pair_editor(self: &Rc<Self>, existing: Option<ManagedObject>) {
        let Some(owner_generation) = self.snapshot.borrow().owner_generation.clone() else {
            self.show_error(
                &tr("The request could not be sent"),
                &tr("The control service is offline."),
            );
            return;
        };
        self.show_pair_editor_for_owner(existing, owner_generation);
    }

    fn show_pair_editor_for_owner(
        self: &Rc<Self>,
        existing: Option<ManagedObject>,
        owner_generation: String,
    ) {
        let existing_pair = existing
            .as_ref()
            .and_then(ManagedObject::stereo_pair)
            .cloned();
        let selected = existing_pair
            .as_ref()
            .map(|pair| HashSet::from([pair.left_device_id.clone(), pair.right_device_id.clone()]))
            .unwrap_or_default();
        let choices = speaker_choices(&self.snapshot.borrow(), &selected);
        if choices.len() < 2 {
            self.show_error(
                &tr("A stereo pair cannot be created"),
                &tr(
                    "At least two available speakers with known stereo-pair capability are required.",
                ),
            );
            return;
        }

        let title = if existing_pair.is_some() {
            tr("Edit Stereo Pair")
        } else {
            tr("New Stereo Pair")
        };
        let (window, content, save_button) =
            form_window(&self.window, &title, &tr("Save Stereo Pair"));
        let grid = form_grid();
        content.append(&grid);

        let name_entry = gtk::Entry::new();
        name_entry.set_hexpand(true);
        name_entry.set_text(
            existing
                .as_ref()
                .map(|object| object.title.as_str())
                .unwrap_or(""),
        );
        add_form_row(&grid, 0, &tr("Name"), &name_entry);

        let labels = choices
            .iter()
            .map(|choice| {
                if choice.available {
                    choice.label.clone()
                } else {
                    format!("{} ({})", choice.label, tr("unavailable"))
                }
            })
            .collect::<Vec<_>>();
        let label_refs = labels.iter().map(String::as_str).collect::<Vec<_>>();
        let left_dropdown = gtk::DropDown::from_strings(&label_refs);
        let right_dropdown = gtk::DropDown::from_strings(&label_refs);
        if let Some(pair) = existing_pair.as_ref() {
            if let Some(index) = choices
                .iter()
                .position(|choice| choice.id == pair.left_device_id)
            {
                left_dropdown.set_selected(index as u32);
            }
            if let Some(index) = choices
                .iter()
                .position(|choice| choice.id == pair.right_device_id)
            {
                right_dropdown.set_selected(index as u32);
            }
        } else {
            right_dropdown.set_selected(1);
        }
        add_form_row(&grid, 1, &tr("Left speaker"), &left_dropdown);
        add_form_row(&grid, 2, &tr("Right speaker"), &right_dropdown);

        let weak = Rc::downgrade(self);
        let path_and_revision = existing
            .as_ref()
            .zip(existing_pair.as_ref())
            .map(|(object, pair)| (object.path.clone(), pair.revision));
        let window_for_save = window.downgrade();
        save_button.connect_clicked(move |_| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            let name = name_entry.text().trim().to_owned();
            if name.is_empty() {
                view.show_error(
                    &tr("The stereo pair is incomplete"),
                    &tr("Enter a stereo-pair name."),
                );
                return;
            }
            let Some(left) = choices.get(left_dropdown.selected() as usize) else {
                return;
            };
            let Some(right) = choices.get(right_dropdown.selected() as usize) else {
                return;
            };
            if left.id == right.id {
                view.show_error(
                    &tr("The stereo pair is incomplete"),
                    &tr("Select different LEFT and RIGHT speakers."),
                );
                return;
            }

            let mutation = if let Some((path, expected_revision)) = &path_and_revision {
                Mutation::UpdateStereoPair {
                    path: path.clone(),
                    expected_revision: *expected_revision,
                    name,
                    left_device_id: left.id.clone(),
                    right_device_id: right.id.clone(),
                }
            } else {
                Mutation::CreateStereoPair {
                    name,
                    left_device_id: left.id.clone(),
                    right_device_id: right.id.clone(),
                }
            };
            if let Some(window) = window_for_save.upgrade() {
                window.close();
            }
            view.submit_for_owner(mutation, &owner_generation);
        });
        window.present();
    }

    fn show_import_editor(self: &Rc<Self>, object: ManagedObject, owner_generation: String) {
        let Some(topology) = object.observed_topology() else {
            return;
        };
        if !topology.matched_preset_id.is_empty() {
            self.show_error(
                &tr("The observed topology is already saved"),
                &tr("It already matches an existing saved zone or stereo pair."),
            );
            return;
        }
        if !topology_is_importable(topology, &self.snapshot.borrow()) {
            let detail = if topology_has_blocked_member(topology, &self.snapshot.borrow()) {
                tr("Wait for the affected speaker to recover before changing hardware topology.")
            } else {
                tr(
                    "The speakers reported inconsistent topology. Reconcile the state and try again.",
                )
            };
            self.show_error(&tr("The observed topology cannot be imported"), &detail);
            return;
        }
        let title = match topology.kind.as_str() {
            "stereo-pair" => tr("Import Stereo Pair"),
            "zone" => tr("Import Zone"),
            _ => return,
        };
        let (window, content, save_button) = form_window(&self.window, &title, &tr("Import"));
        let grid = form_grid();
        content.append(&grid);
        let name_entry = gtk::Entry::new();
        name_entry.set_text(&object.title);
        name_entry.set_hexpand(true);
        add_form_row(&grid, 0, &tr("Saved name"), &name_entry);

        let weak = Rc::downgrade(self);
        let observed_path = object.path.clone();
        let window_for_save = window.downgrade();
        save_button.connect_clicked(move |_| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            let name = name_entry.text().trim().to_owned();
            if name.is_empty() {
                view.show_error(&tr("The import is incomplete"), &tr("Enter a saved name."));
                return;
            }
            if let Some(window) = window_for_save.upgrade() {
                window.close();
            }
            view.submit_for_owner(
                Mutation::ImportTopology {
                    observed_path: observed_path.clone(),
                    name,
                },
                &owner_generation,
            );
        });
        window.present();
    }

    fn show_defaults_editor(self: &Rc<Self>) {
        let Some(owner_generation) = self.snapshot.borrow().owner_generation.clone() else {
            self.show_error(
                &tr("The request could not be sent"),
                &tr("The control service is offline."),
            );
            return;
        };
        let Some(defaults) = self.snapshot.borrow().defaults.clone() else {
            self.show_error(
                &tr("Default policies are unavailable"),
                &tr("The control API is not compatible or is changing owner."),
            );
            return;
        };
        let (window, content, save_button) =
            form_window(&self.window, &tr("Default Policies"), &tr("Save Defaults"));
        let grid = form_grid();
        content.append(&grid);

        let conflict_dropdown =
            value_dropdown(DEFAULT_CONFLICT_POLICIES, &defaults.conflict_policy);
        add_form_row(&grid, 0, &tr("Conflict policy"), &conflict_dropdown);
        let resume_dropdown = value_dropdown(DEFAULT_RESUME_POLICIES, &defaults.resume_policy);
        resume_dropdown.set_sensitive(false);
        add_form_row(&grid, 1, &tr("Resume policy"), &resume_dropdown);
        let auto_heal_switch = gtk::Switch::new();
        auto_heal_switch.set_active(defaults.auto_heal);
        auto_heal_switch.set_sensitive(false);
        auto_heal_switch.set_halign(gtk::Align::Start);
        add_form_row(&grid, 2, &tr("Automatic healing"), &auto_heal_switch);
        content.append(&future_policy_note());

        let weak = Rc::downgrade(self);
        let window_for_save = window.downgrade();
        save_button.connect_clicked(move |_| {
            let Some(view) = weak.upgrade() else {
                return;
            };
            let mutation = Mutation::UpdateDefaults {
                conflict_policy: selected_value(DEFAULT_CONFLICT_POLICIES, &conflict_dropdown)
                    .to_owned(),
                resume_policy: selected_value(DEFAULT_RESUME_POLICIES, &resume_dropdown).to_owned(),
                auto_heal: auto_heal_switch.is_active(),
            };
            if let Some(window) = window_for_save.upgrade() {
                window.close();
            }
            view.submit_for_owner(mutation, &owner_generation);
        });
        window.present();
    }

    fn object_row(self: &Rc<Self>, object: &ManagedObject) -> gtk::ListBoxRow {
        let snapshot = self.snapshot.borrow();
        let presentation = object_presentation(object, &snapshot);
        let owner_generation = snapshot.owner_generation.clone().unwrap_or_default();
        drop(snapshot);
        let row = gtk::ListBoxRow::new();
        row.add_css_class("object-row");
        row.set_tooltip_text(Some(&presentation.tooltip));
        row.set_activatable(false);

        let content = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        let icon = gtk::Image::from_icon_name(kind_icon(object.kind));
        icon.set_pixel_size(28);
        content.append(&icon);

        let labels = gtk::Box::new(gtk::Orientation::Vertical, 3);
        labels.set_hexpand(true);
        let title = gtk::Label::new(Some(&presentation.title));
        title.set_xalign(0.0);
        title.add_css_class("heading");
        labels.append(&title);

        let subtitle = gtk::Label::new(Some(&presentation.subtitle));
        subtitle.set_xalign(0.0);
        subtitle.set_ellipsize(gtk::pango::EllipsizeMode::End);
        subtitle.add_css_class("dim-label");
        labels.append(&subtitle);
        content.append(&labels);

        if let Some(status) = presentation.status.as_ref() {
            let status_label = gtk::Label::new(Some(status));
            status_label.add_css_class("dim-label");
            content.append(&status_label);
        }
        if let ObjectDetails::Zone(zone) = &object.details
            && let Some(audio_state) = zone.audio_state.as_deref()
        {
            let audio_label = gtk::Label::new(Some(&format!(
                "{}: {}",
                tr("Audio"),
                audio_state_label(audio_state)
            )));
            audio_label.add_css_class("dim-label");
            if !zone.sink_node_name.is_empty() {
                audio_label.set_tooltip_text(Some(&zone.sink_node_name));
            }
            content.append(&audio_label);
        }

        let actions = gtk::Box::new(gtk::Orientation::Horizontal, 6);
        match &object.details {
            ObjectDetails::Zone(zone) => {
                self.append_zone_actions(&actions, object, zone, &owner_generation)
            }
            ObjectDetails::StereoPair(pair) => {
                self.append_pair_actions(&actions, object, pair, &owner_generation)
            }
            ObjectDetails::ObservedTopology(topology)
                if topology_is_importable(topology, &self.snapshot.borrow()) =>
            {
                let button = gtk::Button::with_label(&tr("Import"));
                button.set_sensitive(!owner_generation.is_empty());
                let weak = Rc::downgrade(self);
                let object = object.clone();
                let owner_generation = owner_generation.clone();
                button.connect_clicked(move |_| {
                    if let Some(view) = weak.upgrade() {
                        view.show_import_editor(object.clone(), owner_generation.clone());
                    }
                });
                actions.append(&button);
            }
            ObjectDetails::ObservedTopology(topology) if !topology.matched_preset_id.is_empty() => {
                let explanation = gtk::Label::new(Some(&tr("Already saved")));
                explanation.add_css_class("dim-label");
                actions.append(&explanation);
            }
            ObjectDetails::ObservedTopology(topology)
                if topology_has_blocked_member(topology, &self.snapshot.borrow()) =>
            {
                let explanation =
                    gtk::Label::new(Some(&tr("Import unavailable: speaker controls blocked")));
                explanation.add_css_class("dim-label");
                explanation.set_tooltip_text(Some(&tr(
                    "Wait for the affected speaker to recover before changing hardware topology.",
                )));
                actions.append(&explanation);
            }
            ObjectDetails::ObservedTopology(_) => {
                let explanation =
                    gtk::Label::new(Some(&tr("Import unavailable: inconsistent topology")));
                explanation.add_css_class("dim-label");
                explanation.set_tooltip_text(Some(&tr(
                    "Reconcile the speaker state before importing this topology.",
                )));
                actions.append(&explanation);
            }
            ObjectDetails::Operation(operation) if operation.can_cancel => {
                let button = gtk::Button::with_label(&tr("Cancel"));
                let weak = Rc::downgrade(self);
                let path = object.path.clone();
                let request_id = operation.request_id.clone();
                let owner_generation = owner_generation.clone();
                button.set_sensitive(!owner_generation.is_empty());
                button.connect_clicked(move |_| {
                    if let Some(view) = weak.upgrade() {
                        view.cancel_operation(&path, &request_id, &owner_generation);
                    }
                });
                actions.append(&button);
            }
            _ => {}
        }
        content.append(&actions);
        row.set_child(Some(&content));
        row
    }

    fn append_zone_actions(
        self: &Rc<Self>,
        actions: &gtk::Box,
        object: &ManagedObject,
        zone: &ZoneDetails,
        owner_generation: &str,
    ) {
        let availability = zone_action_availability(zone, &self.snapshot.borrow());
        let owner_generation = owner_generation.to_owned();
        let edit = gtk::Button::with_label(&tr("Edit"));
        let weak = Rc::downgrade(self);
        let editable = object.clone();
        let edit_owner_generation = owner_generation.clone();
        edit.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.show_zone_editor_for_owner(
                    Some(editable.clone()),
                    edit_owner_generation.clone(),
                );
            }
        });
        actions.append(&edit);

        let activate = gtk::Button::with_label(&tr("Activate"));
        configure_action_button(&activate, availability.primary);
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let activate_owner_generation = owner_generation.clone();
        activate.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.submit_for_owner(
                    Mutation::ActivateZone {
                        path: path.clone(),
                        take_over: false,
                    },
                    &activate_owner_generation,
                );
            }
        });
        actions.append(&activate);

        let take_over = destructive_button(&tr("Take Over"));
        configure_take_over_button(&take_over, availability.take_over);
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let take_over_owner_generation = owner_generation.clone();
        take_over.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.confirm_and_submit_for_owner(
                    &tr("Take over the current speaker topology?"),
                    &tr(
                        "This may stop an external source or dissolve a conflicting zone before activating the saved zone.",
                    ),
                    &tr("Take Over"),
                    Mutation::ActivateZone {
                        path: path.clone(),
                        take_over: true,
                    },
                    take_over_owner_generation.clone(),
                );
            }
        });
        actions.append(&take_over);

        let dissolve = destructive_button(&tr("Dissolve"));
        configure_action_button(&dissolve, availability.dissolve);
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let dissolve_owner_generation = owner_generation.clone();
        dissolve.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.confirm_and_submit_for_owner(
                    &tr("Dissolve this hardware zone?"),
                    &tr("The saved zone remains available for later activation."),
                    &tr("Dissolve"),
                    Mutation::DissolveZone { path: path.clone() },
                    dissolve_owner_generation.clone(),
                );
            }
        });
        actions.append(&dissolve);

        let delete = destructive_button(&tr("Delete"));
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let expected_revision = zone.revision;
        let delete_owner_generation = owner_generation.clone();
        delete.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.confirm_and_submit_for_owner(
                    &tr("Delete this saved zone?"),
                    &tr("This removes the preset but does not implicitly dissolve hardware."),
                    &tr("Delete"),
                    Mutation::DeleteZone {
                        path: path.clone(),
                        expected_revision,
                    },
                    delete_owner_generation.clone(),
                );
            }
        });
        actions.append(&delete);
    }

    fn append_pair_actions(
        self: &Rc<Self>,
        actions: &gtk::Box,
        object: &ManagedObject,
        pair: &StereoPairDetails,
        owner_generation: &str,
    ) {
        let availability = pair_action_availability(pair, &self.snapshot.borrow());
        let owner_generation = owner_generation.to_owned();
        let edit = gtk::Button::with_label(&tr("Edit"));
        let weak = Rc::downgrade(self);
        let editable = object.clone();
        let edit_owner_generation = owner_generation.clone();
        edit.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.show_pair_editor_for_owner(
                    Some(editable.clone()),
                    edit_owner_generation.clone(),
                );
            }
        });
        actions.append(&edit);

        let create = gtk::Button::with_label(&tr("Create on Hardware"));
        configure_action_button(&create, availability.primary);
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let create_owner_generation = owner_generation.clone();
        create.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.submit_for_owner(
                    Mutation::CreateStereoPairOnHardware {
                        path: path.clone(),
                        take_over: false,
                    },
                    &create_owner_generation,
                );
            }
        });
        actions.append(&create);

        let take_over = destructive_button(&tr("Take Over"));
        configure_take_over_button(&take_over, availability.take_over);
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let take_over_owner_generation = owner_generation.clone();
        take_over.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.confirm_and_submit_for_owner(
                    &tr("Take over both speakers for this stereo pair?"),
                    &tr(
                        "This may stop external sources or dissolve conflicting topology before creating the pair.",
                    ),
                    &tr("Take Over"),
                    Mutation::CreateStereoPairOnHardware {
                        path: path.clone(),
                        take_over: true,
                    },
                    take_over_owner_generation.clone(),
                );
            }
        });
        actions.append(&take_over);

        let dissolve = destructive_button(&tr("Dissolve"));
        configure_action_button(&dissolve, availability.dissolve);
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let dissolve_owner_generation = owner_generation.clone();
        dissolve.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.confirm_and_submit_for_owner(
                    &tr("Dissolve this hardware stereo pair?"),
                    &tr("The saved stereo-pair preset remains available."),
                    &tr("Dissolve"),
                    Mutation::DissolveStereoPair { path: path.clone() },
                    dissolve_owner_generation.clone(),
                );
            }
        });
        actions.append(&dissolve);

        let delete = destructive_button(&tr("Delete"));
        let weak = Rc::downgrade(self);
        let path = object.path.clone();
        let expected_revision = pair.revision;
        let delete_owner_generation = owner_generation.clone();
        delete.connect_clicked(move |_| {
            if let Some(view) = weak.upgrade() {
                view.confirm_and_submit_for_owner(
                    &tr("Delete this saved stereo pair?"),
                    &tr("This removes the preset but does not implicitly dissolve hardware."),
                    &tr("Delete"),
                    Mutation::DeleteStereoPair {
                        path: path.clone(),
                        expected_revision,
                    },
                    delete_owner_generation.clone(),
                );
            }
        });
        actions.append(&delete);
    }

    fn show_error(&self, message: &str, detail: &str) {
        let dialog = gtk::AlertDialog::builder()
            .modal(true)
            .message(message)
            .detail(detail)
            .build();
        dialog.show(Some(&self.window));
    }
}

impl Drop for ControllerView {
    fn drop(&mut self) {
        if let Some(source) = self.operation_status_hide_source.borrow_mut().take() {
            source.remove();
        }
        if let Some(source) = self.service_restart_timeout_source.borrow_mut().take() {
            source.remove();
        }
    }
}

fn member_choices(
    snapshot: &ServiceSnapshot,
    selected: &HashSet<LogicalMember>,
) -> Vec<MemberChoice> {
    let mut choices = snapshot
        .objects
        .iter()
        .filter_map(|object| match &object.details {
            ObjectDetails::Speaker(details) => {
                let member = LogicalMember::new("speaker", &details.id);
                let available = details.topology_actions_enabled();
                let visible = details.online && details.available;
                (visible || selected.contains(&member)).then(|| MemberChoice {
                    member,
                    label: object.title.clone(),
                    available,
                })
            }
            ObjectDetails::StereoPair(details) => {
                let member = LogicalMember::new("stereo-pair", &details.id);
                let available =
                    details.available && !stereo_pair_has_blocked_member(snapshot, details);
                (details.available || selected.contains(&member)).then(|| MemberChoice {
                    member,
                    label: object.title.clone(),
                    available,
                })
            }
            _ => None,
        })
        .collect::<Vec<_>>();

    for member in selected {
        if !choices.iter().any(|choice| &choice.member == member) {
            choices.push(MemberChoice {
                member: member.clone(),
                label: member.id.clone(),
                available: false,
            });
        }
    }
    choices.sort_by_key(|choice| choice.label.to_lowercase());
    choices
}

fn speaker_choices(snapshot: &ServiceSnapshot, selected: &HashSet<String>) -> Vec<SpeakerChoice> {
    let mut choices = snapshot
        .objects
        .iter()
        .filter_map(|object| {
            let ObjectDetails::Speaker(details) = &object.details else {
                return None;
            };
            let available = details.topology_actions_enabled()
                && details.capabilities_known
                && details.stereo_pair_capable;
            (available || selected.contains(&details.id)).then(|| SpeakerChoice {
                id: details.id.clone(),
                label: if details.model.is_empty() {
                    object.title.clone()
                } else {
                    format!("{} — {}", object.title, details.model)
                },
                available,
            })
        })
        .collect::<Vec<_>>();
    for id in selected {
        if !choices.iter().any(|choice| &choice.id == id) {
            choices.push(SpeakerChoice {
                id: id.clone(),
                label: id.clone(),
                available: false,
            });
        }
    }
    choices.sort_by_key(|choice| choice.label.to_lowercase());
    choices
}

fn receiver_access_mutations_enabled(
    snapshot: &ServiceSnapshot,
    access: &ReceiverAccessDetails,
) -> bool {
    matches!(snapshot.state, ServiceState::Online)
        && access.contract_complete
        && access.config_writable
        && !access.config_digest.is_empty()
        && access.configuration_error.is_empty()
}

fn receiver_access_devices(
    snapshot: &ServiceSnapshot,
    access: &ReceiverAccessDetails,
) -> Vec<ReceiverAccessDevice> {
    let speakers = snapshot
        .objects
        .iter()
        .filter_map(|object| match &object.details {
            ObjectDetails::Speaker(details) => Some((object, details)),
            _ => None,
        })
        .collect::<Vec<_>>();
    let mut matched_speakers = HashSet::new();
    let mut devices = access
        .device_policies
        .iter()
        .map(|(policy_id, configured_mode)| {
            let speaker = speakers
                .iter()
                .find(|(_, details)| details.id.eq_ignore_ascii_case(policy_id.as_str()));
            if let Some((_, details)) = speaker {
                matched_speakers.insert(details.id.to_ascii_lowercase());
            }
            ReceiverAccessDevice {
                id: policy_id.clone(),
                name: speaker
                    .map(|(object, _)| object.title.clone())
                    .filter(|name| !name.is_empty())
                    .unwrap_or_else(|| policy_id.clone()),
                configured_mode: configured_mode.clone(),
                effective_mode: speaker
                    .map(|(_, details)| details.policy_mode.clone())
                    .unwrap_or_default(),
                policy_reason: speaker
                    .map(|(_, details)| details.policy_reason.clone())
                    .unwrap_or_default(),
                published: speaker.is_some_and(|(_, details)| details.online && details.available),
            }
        })
        .collect::<Vec<_>>();

    devices.extend(
        speakers
            .into_iter()
            .filter(|(_, details)| !matched_speakers.contains(&details.id.to_ascii_lowercase()))
            .map(|(object, details)| ReceiverAccessDevice {
                id: details.id.clone(),
                name: if object.title.is_empty() {
                    details.id.clone()
                } else {
                    object.title.clone()
                },
                configured_mode: "auto".to_owned(),
                effective_mode: details.policy_mode.clone(),
                policy_reason: details.policy_reason.clone(),
                published: details.online && details.available,
            }),
    );
    devices.sort_by(|left, right| {
        left.name
            .to_lowercase()
            .cmp(&right.name.to_lowercase())
            .then_with(|| left.id.cmp(&right.id))
    });
    devices
}

fn automatic_access_impact(snapshot: &ServiceSnapshot) -> Vec<ReceiverAccessImpact> {
    let mut impacts = snapshot
        .objects
        .iter()
        .filter_map(|object| {
            let ObjectDetails::Speaker(details) = &object.details else {
                return None;
            };
            (details.policy_mode == "auto" && details.online && details.available).then(|| {
                ReceiverAccessImpact {
                    id: details.id.clone(),
                    name: if object.title.is_empty() {
                        details.id.clone()
                    } else {
                        object.title.clone()
                    },
                }
            })
        })
        .collect::<Vec<_>>();
    impacts.sort_by(|left, right| {
        left.name
            .to_lowercase()
            .cmp(&right.name.to_lowercase())
            .then_with(|| left.id.cmp(&right.id))
    });
    impacts
}

fn receiver_access_disable_detail(impacts: &[ReceiverAccessImpact]) -> String {
    if impacts.is_empty() {
        return tr(
            "After restart, only explicitly allowed device IDs will be published. No currently published speaker uses automatic access.",
        );
    }

    let affected = impacts
        .iter()
        .map(|impact| format!("• {} ({})", impact.name, impact.id))
        .collect::<Vec<_>>()
        .join("\n");
    format!(
        "{}\n\n{affected}\n\n{}",
        tr("These automatically added speakers will disappear after restart:"),
        tr("Explicitly allowed speakers will remain published.")
    )
}

fn automatic_access_state_label(enabled: bool) -> String {
    if enabled {
        tr("Automatic access enabled")
    } else {
        tr("Explicit allowlist only")
    }
}

fn device_policy_label(mode: &str) -> String {
    match mode {
        "auto" => tr("Automatic"),
        "allow" => tr("Allowed"),
        "block" => tr("Blocked"),
        "" => tr("Unknown"),
        unknown => unknown.to_owned(),
    }
}

fn policy_reason_label(reason: &str) -> String {
    match reason {
        "verified-auto" => tr("Protocol and device ID verified"),
        "explicitly-allowed" => tr("Explicitly allowed by device ID"),
        "" => String::new(),
        unknown => unknown.to_owned(),
    }
}

fn object_presentation(object: &ManagedObject, snapshot: &ServiceSnapshot) -> ObjectPresentation {
    match &object.details {
        ObjectDetails::Speaker(speaker) => {
            let mut subtitle = status_message_label(&object.subtitle);
            if !speaker.policy_mode.is_empty() {
                subtitle = append_detail(
                    subtitle,
                    format!(
                        "{}: {}",
                        tr("Access policy"),
                        device_policy_label(&speaker.policy_mode)
                    ),
                );
            }
            if !speaker.policy_reason.is_empty() {
                subtitle = append_detail(subtitle, policy_reason_label(&speaker.policy_reason));
            }
            let mut tooltip = vec![
                format!("{}: {}", tr("Device ID"), speaker.id),
                format!("D-Bus: {}", object.path),
            ];
            if !object.subtitle.is_empty() {
                tooltip.push(object.subtitle.clone());
            }
            if !speaker.policy_mode.is_empty() {
                tooltip.push(format!(
                    "{}: {}",
                    tr("Access policy"),
                    device_policy_label(&speaker.policy_mode)
                ));
            }
            if !speaker.policy_reason.is_empty() {
                tooltip.push(format!(
                    "{}: {}",
                    tr("Policy reason"),
                    policy_reason_label(&speaker.policy_reason)
                ));
            }
            ObjectPresentation {
                title: object.title.clone(),
                subtitle,
                status: (!speaker.health.is_empty())
                    .then(|| stable_state_label(ObjectKind::Speaker, &speaker.health))
                    .or_else(|| {
                        object
                            .status
                            .as_deref()
                            .map(|status| stable_state_label(ObjectKind::Speaker, status))
                    }),
                tooltip: tooltip.join("\n"),
            }
        }
        ObjectDetails::Zone(zone) => {
            let mut subtitle = status_message_label(&zone.status_message);
            if subtitle.is_empty() {
                subtitle = status_message_label(&object.subtitle);
            }
            if zone.degraded {
                subtitle = append_detail(subtitle, tr("Some members are unavailable"));
            }
            if !zone.audio_error.is_empty() {
                subtitle = append_detail(
                    subtitle,
                    format!("{}: {}", tr("Audio error"), zone.audio_error),
                );
            }
            let mut tooltip = vec![
                format!("{}: {}", tr("Preset ID"), zone.id),
                format!("D-Bus: {}", object.path),
            ];
            if !zone.audio_error.is_empty() {
                tooltip.push(format!("{}: {}", tr("Audio error"), zone.audio_error));
            }
            ObjectPresentation {
                title: object.title.clone(),
                subtitle,
                status: Some(stable_state_label(ObjectKind::Zone, &zone.state)),
                tooltip: tooltip.join("\n"),
            }
        }
        ObjectDetails::StereoPair(pair) => {
            let mut subtitle = status_message_label(&pair.status_message);
            if subtitle.is_empty() {
                subtitle = status_message_label(&object.subtitle);
            }
            ObjectPresentation {
                title: object.title.clone(),
                subtitle,
                status: Some(stable_state_label(ObjectKind::StereoPair, &pair.state)),
                tooltip: format!("{}: {}\nD-Bus: {}", tr("Preset ID"), pair.id, object.path),
            }
        }
        ObjectDetails::ObservedTopology(topology) => {
            observed_topology_presentation(object, topology, snapshot)
        }
        ObjectDetails::Operation(operation) => operation_presentation(object, operation),
    }
}

fn operation_presentation(
    object: &ManagedObject,
    operation: &crate::model::OperationDetails,
) -> ObjectPresentation {
    let timestamp = format_timestamp(operation.updated_at_usec);
    let mut subtitle = format!(
        "{} · {}",
        operation_origin_label(&operation.initiator),
        timestamp
    );
    if operation.state == "failed" && !operation.error_message.is_empty() {
        subtitle = append_detail(subtitle, operation.error_message.clone());
    }

    let mut tooltip = vec![
        format!("{}: {}", tr("Operation ID"), operation.id),
        format!("{}: {}", tr("Request ID"), operation.request_id),
        format!("{}: {}", tr("Initiator"), operation.initiator),
        format!(
            "{}: {}",
            tr("Phase"),
            operation_phase_label(&operation.phase)
        ),
        format!("D-Bus: {}", object.path),
    ];
    if !operation.error_name.is_empty() {
        tooltip.push(format!("{}: {}", tr("Error name"), operation.error_name));
    }
    if !operation.error_message.is_empty() {
        tooltip.push(format!(
            "{}: {}",
            tr("Technical detail"),
            operation.error_message
        ));
    }

    ObjectPresentation {
        title: operation_kind_label(&operation.kind),
        subtitle,
        status: Some(operation_state_label(&operation.state)),
        tooltip: tooltip.join("\n"),
    }
}

fn observed_topology_presentation(
    object: &ManagedObject,
    topology: &crate::model::ObservedTopologyDetails,
    snapshot: &ServiceSnapshot,
) -> ObjectPresentation {
    let member_names = topology
        .device_ids
        .iter()
        .map(|id| speaker_name(snapshot, id))
        .collect::<Vec<_>>();
    let kind = topology_kind_label(&topology.kind);
    let title = if member_names.is_empty() {
        kind
    } else {
        format!("{kind}: {}", member_names.join(" + "))
    };

    let mut details = Vec::new();
    if !topology.matched_preset_id.is_empty() {
        let preset = preset_name(snapshot, &topology.matched_preset_id)
            .unwrap_or_else(|| topology.matched_preset_id.clone());
        details.push(format!("{}: {preset}", tr("Matches saved preset")));
    } else {
        details.push(topology_origin_label(&topology.origin));
    }
    if !topology.master_device_id.is_empty() {
        details.push(format!(
            "{}: {}",
            tr("Master speaker"),
            speaker_name(snapshot, &topology.master_device_id)
        ));
    }
    if topology.external_source_active {
        details.push(tr("An external source is playing"));
    }
    if topology.observed_at_usec > 0 {
        details.push(format!(
            "{}: {}",
            tr("Observed"),
            format_timestamp(topology.observed_at_usec)
        ));
    }

    let mut tooltip = vec![
        format!("{}: {}", tr("Topology ID"), topology.id),
        format!("D-Bus: {}", object.path),
    ];
    if !topology.group_id.is_empty() {
        tooltip.push(format!("{}: {}", tr("Group ID"), topology.group_id));
    }
    if !topology.device_ids.is_empty() {
        tooltip.push(format!(
            "{}: {}",
            tr("Device IDs"),
            topology.device_ids.join(", ")
        ));
    }

    ObjectPresentation {
        title,
        subtitle: details.join(" · "),
        status: Some(if topology.consistent {
            tr("Consistent")
        } else {
            tr("Inconsistent")
        }),
        tooltip: tooltip.join("\n"),
    }
}

fn append_detail(current: String, detail: String) -> String {
    if current.is_empty() {
        detail
    } else {
        format!("{current} · {detail}")
    }
}

fn speaker_name(snapshot: &ServiceSnapshot, device_id: &str) -> String {
    snapshot
        .objects
        .iter()
        .find_map(|object| {
            let ObjectDetails::Speaker(details) = &object.details else {
                return None;
            };
            details
                .id
                .eq_ignore_ascii_case(device_id)
                .then(|| object.title.clone())
        })
        .unwrap_or_else(|| device_id.to_owned())
}

fn preset_name(snapshot: &ServiceSnapshot, preset_id: &str) -> Option<String> {
    snapshot
        .objects
        .iter()
        .find_map(|object| match &object.details {
            ObjectDetails::Zone(details) if details.id.eq_ignore_ascii_case(preset_id) => {
                Some(object.title.clone())
            }
            ObjectDetails::StereoPair(details) if details.id.eq_ignore_ascii_case(preset_id) => {
                Some(object.title.clone())
            }
            _ => None,
        })
}

fn can_create_zone(snapshot: &ServiceSnapshot) -> bool {
    snapshot.objects.iter().any(|object| match &object.details {
        ObjectDetails::Speaker(details) => details.topology_actions_enabled(),
        ObjectDetails::StereoPair(details) => {
            details.available && !stereo_pair_has_blocked_member(snapshot, details)
        }
        _ => false,
    })
}

fn can_create_stereo_pair(snapshot: &ServiceSnapshot) -> bool {
    snapshot
        .objects
        .iter()
        .filter(|object| {
            matches!(
                &object.details,
                ObjectDetails::Speaker(details)
                    if details.topology_actions_enabled()
                        && details.capabilities_known
                        && details.stereo_pair_capable
            )
        })
        .take(2)
        .count()
        >= 2
}

fn zone_action_availability(
    zone: &ZoneDetails,
    snapshot: &ServiceSnapshot,
) -> HardwareActionAvailability {
    let members_are_writable = zone
        .members
        .iter()
        .all(|member| match member.kind.as_str() {
            "speaker" => speaker_allows_topology_actions(snapshot, &member.id),
            "stereo-pair" => stereo_pair_allows_topology_actions(snapshot, &member.id),
            _ => false,
        });
    hardware_action_availability(zone.available && members_are_writable, &zone.state)
}

fn pair_action_availability(
    pair: &StereoPairDetails,
    snapshot: &ServiceSnapshot,
) -> HardwareActionAvailability {
    hardware_action_availability(
        pair.available && !stereo_pair_has_blocked_member(snapshot, pair),
        &pair.state,
    )
}

fn speaker_allows_topology_actions(snapshot: &ServiceSnapshot, device_id: &str) -> bool {
    snapshot.objects.iter().any(|object| {
        matches!(
            &object.details,
            ObjectDetails::Speaker(speaker)
                if speaker.id.eq_ignore_ascii_case(device_id)
                    && speaker.topology_actions_enabled()
        )
    })
}

fn stereo_pair_allows_topology_actions(snapshot: &ServiceSnapshot, pair_id: &str) -> bool {
    snapshot.objects.iter().any(|object| {
        matches!(
            &object.details,
            ObjectDetails::StereoPair(pair)
                if pair.id.eq_ignore_ascii_case(pair_id)
                    && pair.available
                    && !stereo_pair_has_blocked_member(snapshot, pair)
        )
    })
}

fn stereo_pair_has_blocked_member(snapshot: &ServiceSnapshot, pair: &StereoPairDetails) -> bool {
    !speaker_allows_topology_actions(snapshot, &pair.left_device_id)
        || !speaker_allows_topology_actions(snapshot, &pair.right_device_id)
}

fn hardware_action_availability(available: bool, state: &str) -> HardwareActionAvailability {
    if !available {
        return HardwareActionAvailability {
            primary: false,
            take_over: false,
            dissolve: false,
        };
    }

    match state {
        "saved" => HardwareActionAvailability {
            primary: true,
            take_over: false,
            dissolve: false,
        },
        "active" => HardwareActionAvailability {
            primary: false,
            take_over: false,
            dissolve: true,
        },
        "inconsistent" => HardwareActionAvailability {
            primary: false,
            take_over: false,
            dissolve: true,
        },
        _ => HardwareActionAvailability {
            primary: false,
            take_over: false,
            dissolve: false,
        },
    }
}

fn configure_action_button(button: &gtk::Button, available: bool) {
    button.set_sensitive(available);
    button.set_tooltip_text(
        (!available)
            .then(|| tr("This action is not available in the current hardware state."))
            .as_deref(),
    );
}

fn configure_take_over_button(button: &gtk::Button, available: bool) {
    button.set_sensitive(available);
    button.set_tooltip_text(
        (!available)
            .then(|| {
                tr(
                    "Take-over is temporarily unavailable until active sources and topology can be handed off safely.",
                )
            })
            .as_deref(),
    );
}

fn form_window(
    parent: &gtk::ApplicationWindow,
    title: &str,
    save_label: &str,
) -> (gtk::Window, gtk::Box, gtk::Button) {
    let window = gtk::Window::builder()
        .title(title)
        .transient_for(parent)
        .modal(true)
        .destroy_with_parent(true)
        .default_width(560)
        .default_height(560)
        .build();
    let root = gtk::Box::new(gtk::Orientation::Vertical, 18);
    root.set_margin_top(18);
    root.set_margin_bottom(18);
    root.set_margin_start(18);
    root.set_margin_end(18);
    window.set_child(Some(&root));

    let content = gtk::Box::new(gtk::Orientation::Vertical, 12);
    content.set_hexpand(true);
    content.set_vexpand(true);
    let scrolled = gtk::ScrolledWindow::builder()
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vscrollbar_policy(gtk::PolicyType::Automatic)
        .hexpand(true)
        .vexpand(true)
        .child(&content)
        .build();
    root.append(&scrolled);

    let actions = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    actions.set_halign(gtk::Align::End);
    let cancel = gtk::Button::with_label(&tr("Cancel"));
    let save = gtk::Button::with_label(save_label);
    save.add_css_class("suggested-action");
    actions.append(&cancel);
    actions.append(&save);
    root.append(&actions);

    let window_for_cancel = window.downgrade();
    cancel.connect_clicked(move |_| {
        if let Some(window) = window_for_cancel.upgrade() {
            window.close();
        }
    });
    window.set_default_widget(Some(&save));
    (window, content, save)
}

fn form_grid() -> gtk::Grid {
    gtk::Grid::builder()
        .row_spacing(12)
        .column_spacing(12)
        .hexpand(true)
        .build()
}

fn add_form_row(grid: &gtk::Grid, row: i32, label: &str, widget: &impl IsA<gtk::Widget>) {
    let label = gtk::Label::new(Some(label));
    label.set_xalign(1.0);
    label.set_valign(gtk::Align::Start);
    label.set_margin_top(6);
    grid.attach(&label, 0, row, 1, 1);
    grid.attach(widget, 1, row, 1, 1);
}

fn future_policy_note() -> gtk::Label {
    let label = gtk::Label::new(Some(&tr(
        "Automatic resume and healing settings are stored for future support; the daemon does not execute them yet.",
    )));
    label.set_xalign(0.0);
    label.set_wrap(true);
    label.add_css_class("dim-label");
    label
}

fn value_dropdown(values: &[&str], selected: &str) -> gtk::DropDown {
    let labels = values
        .iter()
        .map(|value| policy_label(value))
        .collect::<Vec<_>>();
    let refs = labels.iter().map(String::as_str).collect::<Vec<_>>();
    let dropdown = gtk::DropDown::from_strings(&refs);
    if let Some(index) = values.iter().position(|value| *value == selected) {
        dropdown.set_selected(index as u32);
    }
    dropdown
}

fn selected_value<'a>(values: &'a [&str], dropdown: &gtk::DropDown) -> &'a str {
    values
        .get(dropdown.selected() as usize)
        .copied()
        .unwrap_or(values[0])
}

fn compatibility_issue_message(issue: &CompatibilityIssue) -> String {
    match issue {
        CompatibilityIssue::ManagerUnavailable => {
            tr("The root Manager interface is not ready. Waiting for a complete daemon snapshot.")
        }
        CompatibilityIssue::OwnerGenerationMismatch => tr(
            "The control interfaces belong to different daemon generations. Waiting for a consistent snapshot.",
        ),
        CompatibilityIssue::MissingApiVersion => {
            tr("The daemon does not publish the required control API version.")
        }
        CompatibilityIssue::UnsupportedApiVersion(version) => format!(
            "{} {version}. {}",
            tr("Unsupported control API version:"),
            tr("This controller requires API version 1.")
        ),
        CompatibilityIssue::InvalidDefaults => {
            tr("The daemon published incomplete or invalid default policies.")
        }
    }
}

fn tracking_generation_is_current(tracked: &TrackedOperation, snapshot: &ServiceSnapshot) -> bool {
    snapshot.owner_generation.as_deref() == Some(tracked.owner_generation.as_str())
}

fn service_restart_completed(snapshot: &ServiceSnapshot, previous_owner: Option<&str>) -> bool {
    matches!(snapshot.state, ServiceState::Online)
        && snapshot
            .receiver_access
            .as_ref()
            .is_some_and(|access| !access.restart_required)
        && snapshot
            .owner_generation
            .as_deref()
            .zip(previous_owner)
            .is_some_and(|(owner, previous)| owner != previous)
}

fn tracked_request_matches(
    tracked: &TrackedOperation,
    operation: &crate::model::OperationDetails,
) -> bool {
    operation.request_id == tracked.request_id
}

fn topology_is_importable(
    topology: &crate::model::ObservedTopologyDetails,
    snapshot: &ServiceSnapshot,
) -> bool {
    topology.consistent
        && matches!(topology.kind.as_str(), "zone" | "stereo-pair")
        && !topology.device_ids.is_empty()
        && topology.matched_preset_id.is_empty()
        && !topology_has_blocked_member(topology, snapshot)
}

fn topology_has_blocked_member(
    topology: &crate::model::ObservedTopologyDetails,
    snapshot: &ServiceSnapshot,
) -> bool {
    topology
        .device_ids
        .iter()
        .any(|device_id| !speaker_allows_topology_actions(snapshot, device_id))
}

fn operation_kind_label(value: &str) -> String {
    match value {
        "create-zone" => tr("Create zone"),
        "update-zone" => tr("Update zone"),
        "delete-zone" => tr("Delete zone"),
        "activate-zone" => tr("Activate zone"),
        "dissolve-zone" => tr("Dissolve zone"),
        "create-stereo-pair" => tr("Create stereo pair"),
        "update-stereo-pair" => tr("Update stereo pair"),
        "dissolve-stereo-pair" => tr("Dissolve stereo pair"),
        "delete-stereo-pair" => tr("Delete stereo pair"),
        "import-topology" => tr("Import topology"),
        "update-defaults" => tr("Update defaults"),
        "set-manage-all-verified" => tr("Update automatic receiver access"),
        "set-device-policy" => tr("Update receiver policy"),
        "reconcile" => tr("Reconcile speaker state"),
        "auto-heal" => tr("Automatic repair"),
        unknown => unknown.to_owned(),
    }
}

fn operation_state_label(value: &str) -> String {
    match value {
        "queued" => tr("Queued"),
        "running" => tr("Running"),
        "succeeded" => tr("Succeeded"),
        "failed" => tr("Failed"),
        "cancelled" => tr("Cancelled"),
        unknown => unknown.to_owned(),
    }
}

fn operation_phase_label(value: &str) -> String {
    match value {
        "queued" => tr("Queued"),
        "running" => tr("Running"),
        "completed" => tr("Completed"),
        "failed" => tr("Failed"),
        "cancelled" => tr("Cancelled"),
        unknown => unknown.to_owned(),
    }
}

fn operation_origin_label(value: &str) -> String {
    if value == "daemon:topology-event" {
        tr("Automatic topology refresh")
    } else if value.starts_with("daemon:") || value == "internal" {
        tr("Companion daemon")
    } else if value.starts_with(':') {
        tr("User application")
    } else if value.is_empty() {
        tr("Unknown origin")
    } else {
        value.to_owned()
    }
}

fn topology_kind_label(value: &str) -> String {
    match value {
        "zone" => tr("Zone"),
        "stereo-pair" => tr("Stereo pair"),
        "unknown" | "" => tr("Unknown topology"),
        unknown => unknown.to_owned(),
    }
}

fn topology_origin_label(value: &str) -> String {
    match value {
        "managed" => tr("Managed topology"),
        "external" => tr("External topology"),
        "matched-after-restart" => tr("Restored topology"),
        "unknown" | "" => tr("Unknown origin"),
        unknown => unknown.to_owned(),
    }
}

fn stable_state_label(kind: ObjectKind, value: &str) -> String {
    match (kind, value) {
        (ObjectKind::Speaker, "verifying") => tr("Verifying"),
        (ObjectKind::Speaker, "connecting-events") => tr("Connecting"),
        (ObjectKind::Speaker, "reading-volume") => tr("Reading volume"),
        (ObjectKind::Speaker, "recovering") => tr("Recovering"),
        (ObjectKind::Speaker, "active") => tr("Active"),
        (ObjectKind::Speaker, "error") => tr("Error"),
        (ObjectKind::Speaker, "offline") => tr("Offline"),
        (ObjectKind::Speaker, "quarantined") => tr("Quarantined"),
        (ObjectKind::Speaker, "write-quarantined") => tr("Hardware writes blocked"),
        (ObjectKind::Zone | ObjectKind::StereoPair, "saved") => tr("Saved"),
        (ObjectKind::Zone | ObjectKind::StereoPair, "active") => tr("Active"),
        (ObjectKind::Zone | ObjectKind::StereoPair, "unavailable") => tr("Unavailable"),
        (ObjectKind::Zone | ObjectKind::StereoPair, "inconsistent") => tr("Inconsistent"),
        (_, "unknown") => tr("Unknown state"),
        _ => value.to_owned(),
    }
}

fn status_message_label(value: &str) -> String {
    match value {
        "Not activated" => tr("Not activated"),
        "Not created" => tr("Not created"),
        "Preset changed; activate or reconcile it again" => {
            tr("Preset changed; activate or reconcile it again")
        }
        "Preset changed; create or reconcile the pair again" => {
            tr("Preset changed; create or reconcile the pair again")
        }
        "Activated with unavailable members" => tr("Activated with unavailable members"),
        "Verified on all logical members" => tr("Verified on all logical members"),
        "Dissolved and verified" => tr("Dissolved and verified"),
        "Created and verified on both speakers" => tr("Created and verified on both speakers"),
        "One or both speakers are offline" => tr("One or both speakers are offline"),
        "Observed consistently on both speakers" => tr("Observed consistently on both speakers"),
        "Not present on hardware" => tr("Not present on hardware"),
        "Hardware group differs between speakers" => tr("Hardware group differs between speakers"),
        "No valid logical member is online" => tr("No valid logical member is online"),
        "Available members are not zoned" => tr("Available members are not zoned"),
        "Observed zone is incomplete or inconsistent" => {
            tr("Observed zone is incomplete or inconsistent")
        }
        "Active with unavailable logical members" => tr("Active with unavailable logical members"),
        "Observed consistently" => tr("Observed consistently"),
        unknown => unknown.to_owned(),
    }
}

fn format_timestamp(unix_usec: i64) -> String {
    if unix_usec <= 0 {
        return tr("Unknown time");
    }
    glib::DateTime::from_unix_local(unix_usec.div_euclid(1_000_000))
        .and_then(|timestamp| timestamp.format("%x %X"))
        .map(|formatted| formatted.to_string())
        .unwrap_or_else(|_| tr("Unknown time"))
}

fn policy_label(value: &str) -> String {
    match value {
        "inherit" => tr("Inherit"),
        "protected" => tr("Protected"),
        "take-over-on-activation" => tr("Take over on activation"),
        "manual" => tr("Manual"),
        "automatic" => tr("Automatic"),
        "disabled" => tr("Disabled"),
        "enabled" => tr("Enabled"),
        _ => value.to_owned(),
    }
}

fn audio_state_label(value: &str) -> String {
    match value {
        "unpublished" => tr("Unpublished"),
        "idle" => tr("Idle"),
        "demand-waiting" => tr("Waiting for playback"),
        "gate-waiting" => tr("Verifying receiver"),
        "routed" => tr("Playing"),
        "promoted-direct" => tr("Playing through direct master"),
        "promoted-dissolving" => tr("Dissolving playback zone"),
        "volume-writing" => tr("Updating volume"),
        "volume-rolling-back" => tr("Rolling volume back"),
        "volume-verifying" => tr("Verifying volume"),
        "failed" => tr("Failed"),
        unknown => unknown.to_owned(),
    }
}

fn destructive_button(label: &str) -> gtk::Button {
    let button = gtk::Button::with_label(label);
    button.add_css_class("destructive-action");
    button
}

fn page_title(kind: ObjectKind) -> String {
    match kind {
        ObjectKind::Speaker => tr("Speakers"),
        ObjectKind::Zone => tr("Zones"),
        ObjectKind::StereoPair => tr("Stereo Pairs"),
        ObjectKind::ObservedTopology => tr("Observed Topology"),
        ObjectKind::Operation => tr("Operations"),
    }
}

fn empty_placeholder(kind: ObjectKind) -> gtk::Widget {
    let title = match kind {
        ObjectKind::Speaker => tr("No speakers"),
        ObjectKind::Zone => tr("No saved zones"),
        ObjectKind::StereoPair => tr("No stereo pairs"),
        ObjectKind::ObservedTopology => tr("No observed topology"),
        ObjectKind::Operation => tr("No recent operations"),
    };

    let container = gtk::Box::new(gtk::Orientation::Vertical, 8);
    container.set_valign(gtk::Align::Center);
    container.set_halign(gtk::Align::Center);
    container.add_css_class("empty-page");

    let image = gtk::Image::from_icon_name(kind_icon(kind));
    image.set_pixel_size(48);
    image.add_css_class("dim-label");
    container.append(&image);

    let label = gtk::Label::new(Some(&title));
    label.add_css_class("title-3");
    container.append(&label);

    if kind == ObjectKind::ObservedTopology {
        let description = gtk::Label::new(Some(&tr(
            "The speakers currently report no zone or stereo pair.",
        )));
        description.add_css_class("dim-label");
        description.set_wrap(true);
        description.set_justify(gtk::Justification::Center);
        container.append(&description);
    }

    container.upcast()
}

fn receiver_access_empty_placeholder() -> gtk::Widget {
    let container = gtk::Box::new(gtk::Orientation::Vertical, 8);
    container.set_margin_top(24);
    container.set_margin_bottom(24);
    container.set_halign(gtk::Align::Center);

    let label = gtk::Label::new(Some(&tr("No known speaker device IDs")));
    label.add_css_class("dim-label");
    container.append(&label);
    container.upcast()
}

fn kind_icon(kind: ObjectKind) -> &'static str {
    match kind {
        ObjectKind::Speaker | ObjectKind::StereoPair => "audio-speakers-symbolic",
        ObjectKind::Zone => "network-workgroup-symbolic",
        ObjectKind::ObservedTopology => "view-refresh-symbolic",
        ObjectKind::Operation => "emblem-system-symbolic",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{
        ManagerDefaults, ObjectDetails, ObservedTopologyDetails, OperationDetails, SpeakerDetails,
    };
    use std::collections::BTreeMap;

    struct DropProbe(Rc<Cell<bool>>);

    impl Drop for DropProbe {
        fn drop(&mut self) {
            self.0.set(true);
        }
    }

    fn speaker_with_capabilities(
        id: &str,
        title: &str,
        available: bool,
        capabilities_known: bool,
        stereo_pair_capable: bool,
    ) -> ManagedObject {
        ManagedObject {
            path: format!("/speakers/{id}"),
            kind: ObjectKind::Speaker,
            title: title.to_owned(),
            subtitle: String::new(),
            status: None,
            details: ObjectDetails::Speaker(SpeakerDetails {
                id: id.to_owned(),
                model: "SoundTouch 10".to_owned(),
                online: true,
                available,
                health: if available { "active" } else { "offline" }.to_owned(),
                write_quarantined: false,
                capabilities_known,
                stereo_pair_capable,
                policy_mode: "auto".to_owned(),
                policy_reason: "verified-auto".to_owned(),
            }),
        }
    }

    fn speaker(id: &str, title: &str, available: bool) -> ManagedObject {
        speaker_with_capabilities(id, title, available, true, true)
    }

    fn speaker_with_policy(id: &str, title: &str, mode: &str, reason: &str) -> ManagedObject {
        let mut speaker = speaker(id, title, true);
        let ObjectDetails::Speaker(details) = &mut speaker.details else {
            unreachable!();
        };
        details.policy_mode = mode.to_owned();
        details.policy_reason = reason.to_owned();
        speaker
    }

    fn receiver_access() -> ReceiverAccessDetails {
        ReceiverAccessDetails {
            manage_all_verified: true,
            configured_manage_all_verified: true,
            config_path: "/home/test/.config/soundtouch-pipewire/config.conf".to_owned(),
            config_writable: true,
            config_digest: "sha256:digest".to_owned(),
            restart_required: false,
            configuration_error: String::new(),
            device_policies: BTreeMap::new(),
            device_policy_count: 0,
            explicitly_allowed_device_count: 0,
            explicitly_blocked_device_count: 0,
            contract_complete: true,
        }
    }

    fn online_snapshot(objects: Vec<ManagedObject>) -> ServiceSnapshot {
        ServiceSnapshot {
            state: ServiceState::Online,
            objects,
            defaults: None,
            receiver_access: Some(receiver_access()),
            owner_generation: Some(":1.10".to_owned()),
        }
    }

    fn zone_details(state: &str, available: bool) -> ZoneDetails {
        ZoneDetails {
            id: "10000000-0000-4000-8000-000000000001".to_owned(),
            revision: 1,
            members: Vec::new(),
            preferred_master: None,
            conflict_policy: "inherit".to_owned(),
            resume_policy: "inherit".to_owned(),
            auto_heal: "inherit".to_owned(),
            state: state.to_owned(),
            available,
            degraded: false,
            sink_node_name: String::new(),
            audio_state: None,
            audio_error: String::new(),
            status_message: String::new(),
        }
    }

    #[test]
    fn member_selector_excludes_unavailable_new_members_but_preserves_saved_ones() {
        let unavailable = LogicalMember::new("speaker", "bbbbbbbbbbbb");
        let snapshot = ServiceSnapshot {
            state: ServiceState::Online,
            objects: vec![
                speaker("aaaaaaaaaaaa", "Available", true),
                speaker("bbbbbbbbbbbb", "Saved offline", false),
                speaker("cccccccccccc", "Other offline", false),
            ],
            defaults: None,
            receiver_access: None,
            owner_generation: Some(":1.10".to_owned()),
        };
        let choices = member_choices(&snapshot, &HashSet::from([unavailable.clone()]));

        assert!(
            choices
                .iter()
                .any(|choice| choice.member.id == "aaaaaaaaaaaa" && choice.available)
        );
        assert!(
            choices
                .iter()
                .any(|choice| choice.member == unavailable && !choice.available)
        );
        assert!(
            choices
                .iter()
                .all(|choice| choice.member.id != "cccccccccccc")
        );
    }

    #[test]
    fn stereo_pair_selector_requires_known_capability_but_preserves_saved_members() {
        let snapshot = ServiceSnapshot {
            state: ServiceState::Online,
            objects: vec![
                speaker_with_capabilities("aaaaaaaaaaaa", "Capable", true, true, true),
                speaker_with_capabilities(
                    "bbbbbbbbbbbb",
                    "Capabilities pending",
                    true,
                    false,
                    false,
                ),
                speaker_with_capabilities("cccccccccccc", "Not capable", true, true, false),
            ],
            defaults: Some(ManagerDefaults {
                conflict_policy: "protected".to_owned(),
                resume_policy: "manual".to_owned(),
                auto_heal: false,
            }),
            receiver_access: None,
            owner_generation: Some(":1.10".to_owned()),
        };
        let selected = HashSet::from(["bbbbbbbbbbbb".to_owned()]);
        let choices = speaker_choices(&snapshot, &selected);

        assert!(
            choices
                .iter()
                .any(|choice| choice.id == "aaaaaaaaaaaa" && choice.available)
        );
        assert!(
            choices
                .iter()
                .any(|choice| choice.id == "bbbbbbbbbbbb" && !choice.available)
        );
        assert!(choices.iter().all(|choice| choice.id != "cccccccccccc"));
        assert!(!can_create_stereo_pair(&snapshot));

        let mut with_two_capable = snapshot;
        with_two_capable
            .objects
            .push(speaker("dddddddddddd", "Second capable", true));
        assert!(can_create_stereo_pair(&with_two_capable));
    }

    #[test]
    fn write_quarantined_speaker_stays_visible_but_is_not_topology_mutable() {
        let mut gated = speaker("aaaaaaaaaaaa", "Recovering speaker", true);
        let ObjectDetails::Speaker(details) = &mut gated.details else {
            unreachable!();
        };
        details.health = "write-quarantined".to_owned();
        details.write_quarantined = true;
        let snapshot = online_snapshot(vec![gated.clone()]);

        let choices = member_choices(&snapshot, &HashSet::new());
        assert_eq!(choices.len(), 1);
        assert_eq!(choices[0].member.id, "aaaaaaaaaaaa");
        assert!(!choices[0].available);
        assert!(!can_create_zone(&snapshot));
        assert!(!can_create_stereo_pair(&snapshot));

        let access = snapshot.receiver_access.as_ref().expect("receiver access");
        let devices = receiver_access_devices(&snapshot, access);
        assert_eq!(devices.len(), 1);
        assert!(devices[0].published);

        let presentation = object_presentation(&gated, &snapshot);
        assert_eq!(
            presentation.status.as_deref(),
            Some("Hardware writes blocked")
        );
    }

    #[test]
    fn hardware_actions_expand_quarantined_stereo_pair_members() {
        let mut gated = speaker("aaaaaaaaaaaa", "Gated", true);
        let ObjectDetails::Speaker(details) = &mut gated.details else {
            unreachable!();
        };
        details.write_quarantined = true;
        details.health = "write-quarantined".to_owned();
        let pair = StereoPairDetails {
            id: "pair-1".to_owned(),
            revision: 1,
            left_device_id: "AAAAAAAAAAAA".to_owned(),
            right_device_id: "bbbbbbbbbbbb".to_owned(),
            state: "active".to_owned(),
            available: true,
            observed_consistent: true,
            status_message: String::new(),
        };
        let pair_object = ManagedObject {
            path: "/stereo-pairs/pair-1".to_owned(),
            kind: ObjectKind::StereoPair,
            title: "Pair".to_owned(),
            subtitle: String::new(),
            status: Some("active".to_owned()),
            details: ObjectDetails::StereoPair(pair.clone()),
        };
        let snapshot = online_snapshot(vec![
            gated,
            speaker("bbbbbbbbbbbb", "Writable", true),
            pair_object,
        ]);
        let mut zone = zone_details("saved", true);
        zone.members
            .push(LogicalMember::new("stereo-pair", "pair-1"));

        assert_eq!(
            pair_action_availability(&pair, &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: false,
            }
        );
        assert_eq!(
            zone_action_availability(&zone, &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: false,
            }
        );
        assert!(!can_create_zone(&ServiceSnapshot {
            objects: vec![snapshot.objects[0].clone(), snapshot.objects[2].clone()],
            ..snapshot.clone()
        }));
    }

    #[test]
    fn zone_hardware_actions_fail_closed_for_missing_stereo_pair() {
        let snapshot = online_snapshot(Vec::new());
        let mut zone = zone_details("saved", true);
        zone.members
            .push(LogicalMember::new("stereo-pair", "missing-pair"));

        assert_eq!(
            zone_action_availability(&zone, &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: false,
            }
        );
    }

    #[test]
    fn zone_hardware_actions_fail_closed_for_unknown_member_kind() {
        let snapshot = online_snapshot(Vec::new());
        let mut zone = zone_details("saved", true);
        zone.members
            .push(LogicalMember::new("future-member-kind", "member-1"));

        assert_eq!(
            zone_action_availability(&zone, &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: false,
            }
        );
    }

    #[test]
    fn hardware_action_availability_tracks_runtime_state() {
        let snapshot = online_snapshot(Vec::new());
        assert_eq!(
            zone_action_availability(&zone_details("saved", true), &snapshot),
            HardwareActionAvailability {
                primary: true,
                take_over: false,
                dissolve: false,
            }
        );
        assert_eq!(
            zone_action_availability(&zone_details("active", true), &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: true,
            }
        );
        assert_eq!(
            zone_action_availability(&zone_details("inconsistent", true), &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: true,
            }
        );
        assert_eq!(
            zone_action_availability(&zone_details("unavailable", false), &snapshot),
            HardwareActionAvailability {
                primary: false,
                take_over: false,
                dissolve: false,
            }
        );
        for state in ["", "future-state"] {
            assert_eq!(
                zone_action_availability(&zone_details(state, true), &snapshot),
                HardwareActionAvailability {
                    primary: false,
                    take_over: false,
                    dissolve: false,
                },
                "unknown runtime state must fail closed: {state:?}"
            );
        }
    }

    #[test]
    fn receiver_access_combines_published_speakers_and_configured_only_ids() {
        let published = speaker_with_policy(
            "AABBCCDDEEFF",
            "Test speaker",
            "allow",
            "explicitly-allowed",
        );
        let automatic =
            speaker_with_policy("001122334455", "SoundTouch 30", "auto", "verified-auto");
        let mut snapshot = online_snapshot(vec![published, automatic]);
        let access = snapshot.receiver_access.as_mut().expect("receiver access");
        access.device_policies = BTreeMap::from([
            ("aabbccddeeff".to_owned(), "allow".to_owned()),
            ("FFEEDDCCBBAA".to_owned(), "block".to_owned()),
        ]);
        access.device_policy_count = 2;
        let access = access.clone();

        let devices = receiver_access_devices(&snapshot, &access);

        assert_eq!(devices.len(), 3);
        assert!(devices.iter().any(|device| {
            device.id == "aabbccddeeff"
                && device.name == "Test speaker"
                && device.configured_mode == "allow"
                && device.effective_mode == "allow"
                && device.published
        }));
        assert!(devices.iter().any(|device| {
            device.id == "001122334455" && device.configured_mode == "auto" && device.published
        }));
        assert!(devices.iter().any(|device| {
            device.id == "FFEEDDCCBBAA"
                && device.name == "FFEEDDCCBBAA"
                && device.configured_mode == "block"
                && !device.published
        }));
    }

    #[test]
    fn disabling_automatic_access_previews_only_published_auto_speakers() {
        let active = speaker_with_policy("aaaaaaaaaaaa", "Automatic", "auto", "verified-auto");
        let mut provisional = speaker_with_policy("dddddddddddd", "Still verifying", "auto", "");
        let ObjectDetails::Speaker(provisional_details) = &mut provisional.details else {
            unreachable!();
        };
        provisional_details.available = false;
        let mut offline = speaker_with_policy("eeeeeeeeeeee", "Offline", "auto", "");
        let ObjectDetails::Speaker(offline_details) = &mut offline.details else {
            unreachable!();
        };
        offline_details.online = false;
        offline_details.available = false;
        let snapshot = online_snapshot(vec![
            active,
            speaker_with_policy("bbbbbbbbbbbb", "Allowed", "allow", "explicitly-allowed"),
            speaker_with_policy("cccccccccccc", "Blocked", "block", "future-reason"),
            provisional,
            offline,
        ]);

        let impact = automatic_access_impact(&snapshot);

        assert_eq!(
            impact,
            vec![ReceiverAccessImpact {
                id: "aaaaaaaaaaaa".to_owned(),
                name: "Automatic".to_owned(),
            }]
        );
        let detail = receiver_access_disable_detail(&impact);
        assert!(detail.contains("Automatic"));
        assert!(detail.contains("aaaaaaaaaaaa"));
        assert!(!detail.contains("bbbbbbbbbbbb"));
    }

    #[test]
    fn receiver_access_does_not_call_provisional_or_offline_speakers_published() {
        let mut provisional = speaker_with_policy("aaaaaaaaaaaa", "Still verifying", "auto", "");
        let ObjectDetails::Speaker(provisional_details) = &mut provisional.details else {
            unreachable!();
        };
        provisional_details.available = false;
        let mut offline = speaker_with_policy(
            "bbbbbbbbbbbb",
            "Saved offline",
            "allow",
            "explicitly-allowed",
        );
        let ObjectDetails::Speaker(offline_details) = &mut offline.details else {
            unreachable!();
        };
        offline_details.online = false;
        offline_details.available = false;
        let snapshot = online_snapshot(vec![provisional, offline]);
        let access = snapshot.receiver_access.as_ref().expect("receiver access");

        let devices = receiver_access_devices(&snapshot, access);

        assert_eq!(devices.len(), 2);
        assert!(devices.iter().all(|device| !device.published));
        assert!(automatic_access_impact(&snapshot).is_empty());
    }

    #[test]
    fn receiver_access_mutations_fail_closed_for_unsafe_configuration() {
        let mut snapshot = online_snapshot(Vec::new());
        assert!(receiver_access_mutations_enabled(
            &snapshot,
            snapshot.receiver_access.as_ref().expect("receiver access")
        ));

        snapshot
            .receiver_access
            .as_mut()
            .expect("receiver access")
            .config_writable = false;
        assert!(!receiver_access_mutations_enabled(
            &snapshot,
            snapshot.receiver_access.as_ref().expect("receiver access")
        ));
        {
            let access = snapshot.receiver_access.as_mut().expect("receiver access");
            access.config_writable = true;
            access.configuration_error = "parse error".to_owned();
        }
        assert!(!receiver_access_mutations_enabled(
            &snapshot,
            snapshot.receiver_access.as_ref().expect("receiver access")
        ));
        {
            let access = snapshot.receiver_access.as_mut().expect("receiver access");
            access.configuration_error.clear();
            access.contract_complete = false;
        }
        assert!(!receiver_access_mutations_enabled(
            &snapshot,
            snapshot.receiver_access.as_ref().expect("receiver access")
        ));
        {
            let access = snapshot.receiver_access.as_mut().expect("receiver access");
            access.contract_complete = true;
            access.config_digest.clear();
        }
        assert!(!receiver_access_mutations_enabled(
            &snapshot,
            snapshot.receiver_access.as_ref().expect("receiver access")
        ));
    }

    #[test]
    fn restart_completion_requires_a_reconnected_service_with_applied_configuration() {
        let mut snapshot = online_snapshot(Vec::new());
        assert!(!service_restart_completed(&snapshot, Some(":1.10")));
        assert!(service_restart_completed(&snapshot, Some(":1.9")));

        snapshot
            .receiver_access
            .as_mut()
            .expect("receiver access")
            .restart_required = true;
        assert!(!service_restart_completed(&snapshot, Some(":1.9")));

        snapshot
            .receiver_access
            .as_mut()
            .expect("receiver access")
            .restart_required = false;
        snapshot.state = ServiceState::Offline;
        assert!(!service_restart_completed(&snapshot, Some(":1.9")));

        snapshot.state = ServiceState::Online;
        snapshot.receiver_access = None;
        assert!(!service_restart_completed(&snapshot, Some(":1.9")));
        assert!(!service_restart_completed(&snapshot, None));
    }

    #[test]
    fn tracked_operation_is_scoped_to_one_owner_generation() {
        let tracked = TrackedOperation {
            path: "/operations/o_1".to_owned(),
            request_id: "10000000-0000-4000-8000-000000000001".to_owned(),
            owner_generation: ":1.10".to_owned(),
        };
        let mut snapshot = ServiceSnapshot {
            state: ServiceState::Online,
            objects: Vec::new(),
            defaults: None,
            receiver_access: None,
            owner_generation: Some(":1.10".to_owned()),
        };

        assert!(tracking_generation_is_current(&tracked, &snapshot));
        snapshot.owner_generation = Some(":1.11".to_owned());
        assert!(!tracking_generation_is_current(&tracked, &snapshot));
        snapshot.owner_generation = None;
        assert!(!tracking_generation_is_current(&tracked, &snapshot));

        let mut operation = OperationDetails {
            id: "o_1".to_owned(),
            request_id: tracked.request_id.clone(),
            kind: "reconcile".to_owned(),
            initiator: ":1.10".to_owned(),
            state: "queued".to_owned(),
            phase: "queued".to_owned(),
            can_cancel: true,
            error_name: String::new(),
            error_message: String::new(),
            created_at_usec: 1,
            updated_at_usec: 1,
        };
        assert!(tracked_request_matches(&tracked, &operation));
        operation.request_id = "20000000-0000-4000-8000-000000000002".to_owned();
        assert!(!tracked_request_matches(&tracked, &operation));
    }

    #[test]
    fn observed_topology_import_requires_a_supported_consistent_nonempty_topology() {
        let snapshot = online_snapshot(vec![speaker("aaaaaaaaaaaa", "Available", true)]);
        let mut topology = ObservedTopologyDetails {
            id: "observed-1".to_owned(),
            kind: "zone".to_owned(),
            origin: "external".to_owned(),
            device_ids: vec!["aaaaaaaaaaaa".to_owned()],
            master_device_id: "aaaaaaaaaaaa".to_owned(),
            group_id: String::new(),
            matched_preset_id: String::new(),
            consistent: false,
            external_source_active: false,
            observed_at_usec: 1,
        };

        assert!(!topology_is_importable(&topology, &snapshot));
        topology.consistent = true;
        assert!(topology_is_importable(&topology, &snapshot));

        topology.kind = "future-kind".to_owned();
        assert!(!topology_is_importable(&topology, &snapshot));
        topology.kind = String::new();
        assert!(!topology_is_importable(&topology, &snapshot));
        topology.kind = "stereo-pair".to_owned();
        assert!(topology_is_importable(&topology, &snapshot));

        topology.device_ids.clear();
        assert!(!topology_is_importable(&topology, &snapshot));
        topology.device_ids.push("aaaaaaaaaaaa".to_owned());
        topology.matched_preset_id = "saved-zone".to_owned();
        assert!(!topology_is_importable(&topology, &snapshot));

        topology.matched_preset_id.clear();
        let mut gated = speaker("AAAAAAAAAAAA", "Gated", true);
        let ObjectDetails::Speaker(details) = &mut gated.details else {
            unreachable!();
        };
        details.write_quarantined = true;
        let gated_snapshot = online_snapshot(vec![gated]);
        assert!(!topology_is_importable(&topology, &gated_snapshot));
    }

    #[test]
    fn zone_audio_error_remains_visible_beside_topology_status() {
        let mut zone = zone_details("active", true);
        zone.status_message = "Observed consistently".to_owned();
        zone.audio_error = "receiver verification timed out".to_owned();
        let object = ManagedObject {
            path: "/zones/zone-1".to_owned(),
            kind: ObjectKind::Zone,
            title: "Downstairs".to_owned(),
            subtitle: zone.audio_error.clone(),
            status: Some(zone.state.clone()),
            details: ObjectDetails::Zone(zone),
        };

        let presentation = object_presentation(&object, &ServiceSnapshot::offline());

        assert!(
            presentation
                .subtitle
                .contains("receiver verification timed out")
        );
        assert!(
            presentation
                .tooltip
                .contains("receiver verification timed out")
        );
    }

    #[test]
    fn speaker_presentation_includes_effective_access_policy_and_reason() {
        let object = speaker_with_policy("aaaaaaaaaaaa", "Living room", "auto", "verified-auto");

        let presentation = object_presentation(&object, &ServiceSnapshot::offline());

        assert!(
            presentation
                .subtitle
                .contains("Protocol and device ID verified")
        );
        assert!(
            presentation
                .tooltip
                .contains("Protocol and device ID verified")
        );
        assert!(presentation.tooltip.contains("aaaaaaaaaaaa"));
        assert!(!presentation.tooltip.contains("verified-auto"));
        assert_eq!(policy_reason_label("future-reason"), "future-reason");
    }

    #[test]
    fn speaker_error_remains_available_in_tooltip_when_subtitle_is_ellipsized() {
        let mut object = speaker("aaaaaaaaaaaa", "Living room", false);
        let error = "SoundTouch direct activation could not prove receiver state; sink withdrawn; the guarded RAOP transport generation changed during topology activation";
        object.subtitle = error.to_owned();
        object.status = Some("error".to_owned());
        let ObjectDetails::Speaker(details) = &mut object.details else {
            unreachable!();
        };
        details.write_quarantined = true;

        let presentation = object_presentation(&object, &ServiceSnapshot::offline());

        assert!(presentation.subtitle.contains(error));
        assert!(presentation.tooltip.contains(error));
    }

    #[test]
    fn policy_dropdown_values_remain_protocol_values_not_translations() {
        assert_eq!(CONFLICT_POLICIES[1], "protected");
        assert_eq!(RESUME_POLICIES[2], "automatic");
        assert_eq!(AUTO_HEAL_POLICIES[1], "disabled");
        assert_eq!(DEVICE_POLICY_MODES, ["auto", "allow", "block"]);
    }

    #[test]
    fn audio_state_labels_tolerate_future_protocol_values() {
        assert!(!audio_state_label("gate-waiting").is_empty());
        assert_ne!(audio_state_label("promoted-direct"), "promoted-direct");
        assert_ne!(
            audio_state_label("promoted-dissolving"),
            "promoted-dissolving"
        );
        assert_eq!(audio_state_label("future-state"), "future-state");
    }

    #[test]
    fn stable_protocol_labels_are_human_readable_and_future_safe() {
        assert_ne!(operation_kind_label("reconcile"), "reconcile");
        assert_ne!(operation_state_label("succeeded"), "succeeded");
        assert_ne!(
            stable_state_label(ObjectKind::Zone, "inconsistent"),
            "inconsistent"
        );
        assert_ne!(
            stable_state_label(ObjectKind::Speaker, "recovering"),
            "recovering"
        );
        assert_ne!(
            stable_state_label(ObjectKind::Speaker, "write-quarantined"),
            "write-quarantined"
        );
        assert_eq!(operation_kind_label("future-operation"), "future-operation");
        assert_eq!(
            stable_state_label(ObjectKind::Speaker, "future-state"),
            "future-state"
        );
    }

    #[test]
    fn topology_member_names_use_speaker_display_names() {
        let snapshot = ServiceSnapshot {
            state: ServiceState::Online,
            objects: vec![speaker("aaaaaaaaaaaa", "Living room", true)],
            defaults: None,
            receiver_access: None,
            owner_generation: Some(":1.10".to_owned()),
        };

        assert_eq!(speaker_name(&snapshot, "AAAAAAAAAAAA"), "Living room");
        assert_eq!(speaker_name(&snapshot, "bbbbbbbbbbbb"), "bbbbbbbbbbbb");
    }

    #[test]
    fn invalid_operation_timestamp_has_a_readable_fallback() {
        assert!(!format_timestamp(0).is_empty());
    }

    #[test]
    fn view_anchor_retains_the_controller_until_window_teardown() {
        let dropped = Rc::new(Cell::new(false));
        let anchor = ViewAnchor::default();
        let view = Rc::new(DropProbe(dropped.clone()));

        anchor.set(view.clone());
        drop(view);
        assert!(!dropped.get());

        anchor.clear();
        assert!(dropped.get());
    }
}
