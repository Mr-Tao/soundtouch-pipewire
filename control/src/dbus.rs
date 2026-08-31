use std::cell::{Cell, RefCell};
use std::rc::{Rc, Weak};
use std::time::Duration;

use gio::prelude::*;
use glib::variant::{ObjectPath, ToVariant};

use crate::model::{
    DBUS_NAME, DBUS_ROOT, LogicalMember, MANAGER_INTERFACE, ObjectKind, ServiceSnapshot,
    ServiceState, snapshot,
};

type SnapshotCallback = dyn Fn(ServiceSnapshot);

const CALL_TIMEOUT_MSEC: i32 = 30_000;
const OBJECT_MANAGER_FLAGS: gio::DBusObjectManagerClientFlags =
    gio::DBusObjectManagerClientFlags::DO_NOT_AUTO_START;
const MANAGER_PROXY_FLAGS: gio::DBusProxyFlags = gio::DBusProxyFlags::DO_NOT_AUTO_START;
const SYSTEMD_DBUS_NAME: &str = "org.freedesktop.systemd1";
const SYSTEMD_DBUS_PATH: &str = "/org/freedesktop/systemd1";
const SYSTEMD_MANAGER_INTERFACE: &str = "org.freedesktop.systemd1.Manager";
const SERVICE_UNIT: &str = "soundtouch-pipewire.service";

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Mutation {
    Reconcile,
    CreateZone {
        name: String,
        members: Vec<LogicalMember>,
        preferred_master: Option<LogicalMember>,
        conflict_policy: String,
        resume_policy: String,
        auto_heal: String,
    },
    CreateStereoPair {
        name: String,
        left_device_id: String,
        right_device_id: String,
    },
    ImportTopology {
        observed_path: String,
        name: String,
    },
    UpdateDefaults {
        conflict_policy: String,
        resume_policy: String,
        auto_heal: bool,
    },
    SetManageAllVerified {
        expected_digest: String,
        value: bool,
    },
    SetDevicePolicy {
        expected_digest: String,
        device_id: String,
        mode: String,
    },
    UpdateZone {
        path: String,
        expected_revision: u64,
        name: String,
        members: Vec<LogicalMember>,
        preferred_master: Option<LogicalMember>,
        conflict_policy: String,
        resume_policy: String,
        auto_heal: String,
    },
    ActivateZone {
        path: String,
        take_over: bool,
    },
    DissolveZone {
        path: String,
    },
    DeleteZone {
        path: String,
        expected_revision: u64,
    },
    UpdateStereoPair {
        path: String,
        expected_revision: u64,
        name: String,
        left_device_id: String,
        right_device_id: String,
    },
    CreateStereoPairOnHardware {
        path: String,
        take_over: bool,
    },
    DissolveStereoPair {
        path: String,
    },
    DeleteStereoPair {
        path: String,
        expected_revision: u64,
    },
}

struct CallSpec {
    path: String,
    interface: String,
    method: &'static str,
    parameters: glib::Variant,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SubmittedOperation {
    pub path: String,
    pub request_id: String,
    pub owner_generation: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct PinnedOwner(String);

impl PinnedOwner {
    fn as_str(&self) -> &str {
        &self.0
    }

    fn into_string(self) -> String {
        self.0
    }
}

fn require_current_owner(
    expected_owner_generation: &str,
    current_owner_generation: &str,
) -> Result<PinnedOwner, glib::Error> {
    if expected_owner_generation != current_owner_generation {
        return Err(client_error(
            gio::IOErrorEnum::NotFound,
            "The request belongs to a previous control-service generation",
        ));
    }
    Ok(PinnedOwner(expected_owner_generation.to_owned()))
}

impl Mutation {
    fn call_spec(&self, request_id: &str) -> Result<CallSpec, glib::Error> {
        let manager = |method, parameters| CallSpec {
            path: DBUS_ROOT.to_owned(),
            interface: MANAGER_INTERFACE.to_owned(),
            method,
            parameters,
        };
        let child = |path: &str, kind: ObjectKind, method, parameters| CallSpec {
            path: path.to_owned(),
            interface: kind.interface_name(),
            method,
            parameters,
        };
        let members = |members: &[LogicalMember]| {
            members
                .iter()
                .map(LogicalMember::as_tuple)
                .collect::<Vec<_>>()
        };
        let preferred = |preferred: &Option<LogicalMember>| {
            preferred
                .as_ref()
                .map(LogicalMember::as_tuple)
                .unwrap_or_else(|| (String::new(), String::new()))
        };

        Ok(match self {
            Self::Reconcile => manager("Reconcile", (request_id,).to_variant()),
            Self::CreateZone {
                name,
                members: selected,
                preferred_master,
                conflict_policy,
                resume_policy,
                auto_heal,
            } => manager(
                "CreateZone",
                (
                    request_id,
                    name.as_str(),
                    members(selected),
                    preferred(preferred_master),
                    conflict_policy.as_str(),
                    resume_policy.as_str(),
                    auto_heal.as_str(),
                )
                    .to_variant(),
            ),
            Self::CreateStereoPair {
                name,
                left_device_id,
                right_device_id,
            } => manager(
                "CreateStereoPair",
                (
                    request_id,
                    name.as_str(),
                    left_device_id.as_str(),
                    right_device_id.as_str(),
                )
                    .to_variant(),
            ),
            Self::ImportTopology {
                observed_path,
                name,
            } => {
                let path = ObjectPath::try_from(observed_path.as_str()).map_err(|_| {
                    client_error(
                        gio::IOErrorEnum::InvalidArgument,
                        "Observed topology path is invalid",
                    )
                })?;
                manager(
                    "ImportTopology",
                    (request_id, path, name.as_str()).to_variant(),
                )
            }
            Self::UpdateDefaults {
                conflict_policy,
                resume_policy,
                auto_heal,
            } => manager(
                "UpdateDefaults",
                (
                    request_id,
                    conflict_policy.as_str(),
                    resume_policy.as_str(),
                    *auto_heal,
                )
                    .to_variant(),
            ),
            Self::SetManageAllVerified {
                expected_digest,
                value,
            } => manager(
                "SetManageAllVerified",
                (request_id, expected_digest.as_str(), *value).to_variant(),
            ),
            Self::SetDevicePolicy {
                expected_digest,
                device_id,
                mode,
            } => manager(
                "SetDevicePolicy",
                (
                    request_id,
                    expected_digest.as_str(),
                    device_id.as_str(),
                    mode.as_str(),
                )
                    .to_variant(),
            ),
            Self::UpdateZone {
                path,
                expected_revision,
                name,
                members: selected,
                preferred_master,
                conflict_policy,
                resume_policy,
                auto_heal,
            } => child(
                path,
                ObjectKind::Zone,
                "Update",
                (
                    request_id,
                    *expected_revision,
                    name.as_str(),
                    members(selected),
                    preferred(preferred_master),
                    conflict_policy.as_str(),
                    resume_policy.as_str(),
                    auto_heal.as_str(),
                )
                    .to_variant(),
            ),
            Self::ActivateZone { path, take_over } => child(
                path,
                ObjectKind::Zone,
                "Activate",
                (request_id, *take_over).to_variant(),
            ),
            Self::DissolveZone { path } => child(
                path,
                ObjectKind::Zone,
                "Dissolve",
                (request_id,).to_variant(),
            ),
            Self::DeleteZone {
                path,
                expected_revision,
            } => child(
                path,
                ObjectKind::Zone,
                "Delete",
                (request_id, *expected_revision).to_variant(),
            ),
            Self::UpdateStereoPair {
                path,
                expected_revision,
                name,
                left_device_id,
                right_device_id,
            } => child(
                path,
                ObjectKind::StereoPair,
                "Update",
                (
                    request_id,
                    *expected_revision,
                    name.as_str(),
                    left_device_id.as_str(),
                    right_device_id.as_str(),
                )
                    .to_variant(),
            ),
            Self::CreateStereoPairOnHardware { path, take_over } => child(
                path,
                ObjectKind::StereoPair,
                "CreateOnHardware",
                (request_id, *take_over).to_variant(),
            ),
            Self::DissolveStereoPair { path } => child(
                path,
                ObjectKind::StereoPair,
                "Dissolve",
                (request_id,).to_variant(),
            ),
            Self::DeleteStereoPair {
                path,
                expected_revision,
            } => child(
                path,
                ObjectKind::StereoPair,
                "Delete",
                (request_id, *expected_revision).to_variant(),
            ),
        })
    }
}

pub struct ControlClient {
    manager: RefCell<Option<gio::DBusObjectManagerClient>>,
    manager_proxy: RefCell<Option<gio::DBusProxy>>,
    connecting: Cell<bool>,
    refresh_source: RefCell<Option<glib::SourceId>>,
    retry_source: RefCell<Option<glib::SourceId>>,
    on_snapshot: Rc<SnapshotCallback>,
}

impl ControlClient {
    pub fn new(on_snapshot: impl Fn(ServiceSnapshot) + 'static) -> Rc<Self> {
        Rc::new(Self {
            manager: RefCell::new(None),
            manager_proxy: RefCell::new(None),
            connecting: Cell::new(false),
            refresh_source: RefCell::new(None),
            retry_source: RefCell::new(None),
            on_snapshot: Rc::new(on_snapshot),
        })
    }

    pub fn start(self: &Rc<Self>) {
        self.connect();
    }

    pub fn refresh(self: &Rc<Self>) {
        if let Some(source) = self.refresh_source.borrow_mut().take() {
            source.remove();
        }
        let manager = self.manager.borrow();
        let manager_proxy = self.manager_proxy.borrow();
        if let (Some(manager), Some(manager_proxy)) = (manager.as_ref(), manager_proxy.as_ref()) {
            (self.on_snapshot)(snapshot(manager, manager_proxy));
        } else if !self.connecting.get() {
            (self.on_snapshot)(ServiceSnapshot::offline());
            self.connect();
        }
    }

    fn schedule_refresh(self: &Rc<Self>) {
        if self.refresh_source.borrow().is_some() {
            return;
        }

        let weak = Rc::downgrade(self);
        let source = glib::idle_add_local_once(move || {
            if let Some(client) = weak.upgrade() {
                client.refresh_source.borrow_mut().take();
                client.refresh();
            }
        });
        self.refresh_source.replace(Some(source));
    }

    pub fn submit(
        self: &Rc<Self>,
        mutation: Mutation,
        expected_owner_generation: &str,
        callback: impl FnOnce(Result<SubmittedOperation, glib::Error>) + 'static,
    ) {
        let current_owner_generation = match self.compatible_owner_generation() {
            Ok(owner_generation) => owner_generation,
            Err(error) => {
                callback(Err(error));
                return;
            }
        };
        let owner_generation =
            match require_current_owner(expected_owner_generation, &current_owner_generation) {
                Ok(owner_generation) => owner_generation,
                Err(error) => {
                    callback(Err(error));
                    return;
                }
            };
        let request_id = fresh_request_id();
        let spec = match mutation.call_spec(&request_id) {
            Ok(spec) => spec,
            Err(error) => {
                callback(Err(error));
                return;
            }
        };
        if let Err(error) = self.proxy_for(&spec.path, &spec.interface) {
            callback(Err(error));
            return;
        }
        let connection = match self.connection() {
            Ok(connection) => connection,
            Err(error) => {
                callback(Err(error));
                return;
            }
        };
        let weak = Rc::downgrade(self);

        glib::MainContext::default().spawn_local(async move {
            /*
             * Address the exact unique owner whose object snapshot was
             * validated above. A GDBusProxy created for DBUS_NAME would
             * otherwise follow a replacement owner if the daemon restarted
             * between validation and dispatch, allowing a stale mutation to
             * cross daemon generations.
             */
            let result = connection
                .call_future(
                    Some(owner_generation.as_str()),
                    &spec.path,
                    &spec.interface,
                    spec.method,
                    Some(&spec.parameters),
                    None,
                    gio::DBusCallFlags::NONE,
                    CALL_TIMEOUT_MSEC,
                )
                .await
                .and_then(operation_path_from_reply)
                .map(|path| SubmittedOperation {
                    path,
                    request_id,
                    owner_generation: owner_generation.into_string(),
                });
            callback(result);
            refresh_weak(&weak);
        });
    }

    pub fn cancel_operation(
        self: &Rc<Self>,
        operation_path: &str,
        expected_owner_generation: &str,
        callback: impl FnOnce(Result<bool, glib::Error>) + 'static,
    ) {
        let current_owner_generation = match self.compatible_owner_generation() {
            Ok(owner_generation) => owner_generation,
            Err(error) => {
                callback(Err(error));
                return;
            }
        };
        let owner_generation =
            match require_current_owner(expected_owner_generation, &current_owner_generation) {
                Ok(owner_generation) => owner_generation,
                Err(error) => {
                    callback(Err(error));
                    return;
                }
            };
        let interface = ObjectKind::Operation.interface_name();
        if let Err(error) = self.proxy_for(operation_path, &interface) {
            callback(Err(error));
            return;
        }
        let connection = match self.connection() {
            Ok(connection) => connection,
            Err(error) => {
                callback(Err(error));
                return;
            }
        };
        let operation_path = operation_path.to_owned();
        let weak = Rc::downgrade(self);

        glib::MainContext::default().spawn_local(async move {
            let result = connection
                .call_future(
                    Some(owner_generation.as_str()),
                    &operation_path,
                    &interface,
                    "Cancel",
                    None,
                    None,
                    gio::DBusCallFlags::NONE,
                    CALL_TIMEOUT_MSEC,
                )
                .await
                .and_then(|reply| {
                    reply.get::<(bool,)>().map(|value| value.0).ok_or_else(|| {
                        client_error(
                            gio::IOErrorEnum::InvalidData,
                            "Cancel returned an invalid reply",
                        )
                    })
                });
            callback(result);
            refresh_weak(&weak);
        });
    }

    pub fn restart_service(
        self: &Rc<Self>,
        expected_owner_generation: &str,
        callback: impl FnOnce(Result<String, glib::Error>) + 'static,
    ) {
        let current_owner_generation = match self.compatible_owner_generation() {
            Ok(owner_generation) => owner_generation,
            Err(error) => {
                callback(Err(error));
                return;
            }
        };
        if let Err(error) =
            require_current_owner(expected_owner_generation, &current_owner_generation)
        {
            callback(Err(error));
            return;
        }
        let Some(connection) = self
            .manager
            .borrow()
            .as_ref()
            .map(gio::DBusObjectManagerClient::connection)
        else {
            callback(Err(client_error(
                gio::IOErrorEnum::NotConnected,
                "The SoundTouch control service is offline",
            )));
            return;
        };
        let weak = Rc::downgrade(self);

        glib::MainContext::default().spawn_local(async move {
            let result = async {
                let proxy = gio::DBusProxy::new_future(
                    &connection,
                    gio::DBusProxyFlags::DO_NOT_AUTO_START,
                    None,
                    Some(SYSTEMD_DBUS_NAME),
                    SYSTEMD_DBUS_PATH,
                    SYSTEMD_MANAGER_INTERFACE,
                )
                .await?;
                proxy
                    .call_future(
                        "RestartUnit",
                        Some(&(SERVICE_UNIT, "replace").to_variant()),
                        gio::DBusCallFlags::NONE,
                        CALL_TIMEOUT_MSEC,
                    )
                    .await
                    .and_then(operation_path_from_reply)
            }
            .await;
            callback(result);
            refresh_weak(&weak);
        });
    }

    fn proxy_for(&self, path: &str, interface: &str) -> Result<gio::DBusProxy, glib::Error> {
        if path == DBUS_ROOT && interface == MANAGER_INTERFACE {
            return self.manager_proxy.borrow().clone().ok_or_else(|| {
                client_error(
                    gio::IOErrorEnum::NotConnected,
                    "The SoundTouch control service is offline",
                )
            });
        }

        self.manager
            .borrow()
            .as_ref()
            .and_then(|manager| manager.object(path))
            .and_then(|object| gio::prelude::DBusObjectExt::interface(&object, interface))
            .and_then(|interface| interface.downcast::<gio::DBusProxy>().ok())
            .ok_or_else(|| {
                client_error(
                    gio::IOErrorEnum::NotFound,
                    "The selected control object no longer exists",
                )
            })
    }

    fn connection(&self) -> Result<gio::DBusConnection, glib::Error> {
        self.manager
            .borrow()
            .as_ref()
            .map(gio::DBusObjectManagerClient::connection)
            .ok_or_else(|| {
                client_error(
                    gio::IOErrorEnum::NotConnected,
                    "The SoundTouch control service is offline",
                )
            })
    }

    fn compatible_owner_generation(&self) -> Result<String, glib::Error> {
        let manager = self.manager.borrow();
        let manager_proxy = self.manager_proxy.borrow();
        let (Some(manager), Some(manager_proxy)) = (manager.as_ref(), manager_proxy.as_ref())
        else {
            return Err(client_error(
                gio::IOErrorEnum::NotConnected,
                "The SoundTouch control service is offline",
            ));
        };
        let current = snapshot(manager, manager_proxy);
        match current.state {
            ServiceState::Online => current.owner_generation.ok_or_else(|| {
                client_error(
                    gio::IOErrorEnum::InvalidData,
                    "The SoundTouch control owner generation is missing",
                )
            }),
            ServiceState::Offline => Err(client_error(
                gio::IOErrorEnum::NotConnected,
                "The SoundTouch control service is offline",
            )),
            ServiceState::Incompatible(_) => Err(client_error(
                gio::IOErrorEnum::InvalidData,
                "The SoundTouch control API is incompatible or changing owner",
            )),
            ServiceState::Error(message) => Err(client_error(gio::IOErrorEnum::Failed, &message)),
        }
    }

    fn connect(self: &Rc<Self>) {
        if self.connecting.replace(true) || self.manager.borrow().is_some() {
            return;
        }

        self.retry_source.borrow_mut().take();
        let weak = Rc::downgrade(self);

        glib::MainContext::default().spawn_local(async move {
            let result = async {
                let manager = gio::DBusObjectManagerClient::new_for_bus_future(
                    gio::BusType::Session,
                    OBJECT_MANAGER_FLAGS,
                    DBUS_NAME,
                    DBUS_ROOT,
                )
                .await?;
                let manager_proxy = gio::DBusProxy::new_future(
                    &manager.connection(),
                    MANAGER_PROXY_FLAGS,
                    None,
                    Some(DBUS_NAME),
                    DBUS_ROOT,
                    MANAGER_INTERFACE,
                )
                .await?;
                Ok::<_, glib::Error>((manager, manager_proxy))
            }
            .await;

            let Some(client) = weak.upgrade() else {
                return;
            };
            client.connecting.set(false);

            match result {
                Ok((manager, manager_proxy)) => client.install_manager(manager, manager_proxy),
                Err(error) => {
                    (client.on_snapshot)(ServiceSnapshot::error(error.to_string()));
                    client.schedule_retry();
                }
            }
        });
    }

    fn install_manager(
        self: &Rc<Self>,
        manager: gio::DBusObjectManagerClient,
        manager_proxy: gio::DBusProxy,
    ) {
        let weak = Rc::downgrade(self);
        manager.connect_object_added(move |_, _| refresh_weak(&weak));

        let weak = Rc::downgrade(self);
        manager.connect_object_removed(move |_, _| refresh_weak(&weak));

        let weak = Rc::downgrade(self);
        manager.connect_interface_added(move |_, _, _| refresh_weak(&weak));

        let weak = Rc::downgrade(self);
        manager.connect_interface_removed(move |_, _, _| refresh_weak(&weak));

        let weak = Rc::downgrade(self);
        manager.connect_notify_local(Some("name-owner"), move |_, _| refresh_weak(&weak));

        let weak = Rc::downgrade(self);
        manager.connect_local("interface-proxy-properties-changed", false, move |_| {
            refresh_weak(&weak);
            None
        });

        let weak = Rc::downgrade(self);
        manager_proxy.connect_local("g-properties-changed", false, move |_| {
            refresh_weak(&weak);
            None
        });

        let weak = Rc::downgrade(self);
        manager_proxy.connect_notify_local(Some("g-name-owner"), move |_, _| {
            refresh_weak(&weak);
        });

        let connection = manager.connection();
        let weak = Rc::downgrade(self);
        connection.connect_local("closed", false, move |_| {
            if let Some(client) = weak.upgrade() {
                if let Some(source) = client.refresh_source.borrow_mut().take() {
                    source.remove();
                }
                client.manager.borrow_mut().take();
                client.manager_proxy.borrow_mut().take();
                (client.on_snapshot)(ServiceSnapshot::offline());
                client.schedule_retry();
            }
            None
        });

        self.manager_proxy.replace(Some(manager_proxy));
        self.manager.replace(Some(manager));
        self.refresh();
    }

    fn schedule_retry(self: &Rc<Self>) {
        if self.retry_source.borrow().is_some() {
            return;
        }

        let weak = Rc::downgrade(self);
        let source = glib::timeout_add_local_once(Duration::from_secs(2), move || {
            if let Some(client) = weak.upgrade() {
                client.retry_source.borrow_mut().take();
                client.connect();
            }
        });
        self.retry_source.replace(Some(source));
    }
}

fn fresh_request_id() -> String {
    let request_id = glib::uuid_string_random().to_string();
    debug_assert!(glib::uuid_string_is_valid(&request_id));
    request_id
}

fn operation_path_from_reply(reply: glib::Variant) -> Result<String, glib::Error> {
    reply
        .get::<(ObjectPath,)>()
        .map(|value| value.0.to_string())
        .ok_or_else(|| {
            client_error(
                gio::IOErrorEnum::InvalidData,
                "Mutation returned an invalid operation path",
            )
        })
}

fn client_error(kind: gio::IOErrorEnum, message: &str) -> glib::Error {
    glib::Error::new(kind, message)
}

fn refresh_weak(weak: &Weak<ControlClient>) {
    if let Some(client) = weak.upgrade() {
        client.schedule_refresh();
    }
}

impl Drop for ControlClient {
    fn drop(&mut self) {
        if let Some(source) = self.refresh_source.borrow_mut().take() {
            source.remove();
        }
        if let Some(source) = self.retry_source.borrow_mut().take() {
            source.remove();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const REQUEST_ID: &str = "10000000-0000-4000-8000-000000000001";

    #[test]
    fn every_submission_gets_a_canonical_uuid() {
        for _ in 0..32 {
            let request_id = fresh_request_id();
            assert!(glib::uuid_string_is_valid(&request_id));
            assert_eq!(request_id, request_id.to_ascii_lowercase());
        }
    }

    #[test]
    fn both_proxy_layers_refuse_dbus_auto_start() {
        assert!(
            OBJECT_MANAGER_FLAGS.contains(gio::DBusObjectManagerClientFlags::DO_NOT_AUTO_START)
        );
        assert!(MANAGER_PROXY_FLAGS.contains(gio::DBusProxyFlags::DO_NOT_AUTO_START));
    }

    #[test]
    fn mutations_are_pinned_only_to_the_expected_current_owner() {
        assert_eq!(
            require_current_owner(":1.10", ":1.10").expect("matching owner"),
            PinnedOwner(":1.10".to_owned())
        );
        assert!(require_current_owner(":1.10", ":1.11").is_err());
    }

    #[test]
    fn import_uses_an_object_path_in_its_canonical_signature() {
        let spec = Mutation::ImportTopology {
            observed_path: "/io/github/Mr_Tao/SoundTouchPipeWire1/topologies/t_1".to_owned(),
            name: "Imported".to_owned(),
        }
        .call_spec(REQUEST_ID)
        .expect("valid import");

        assert_eq!(spec.path, DBUS_ROOT);
        assert_eq!(spec.interface, MANAGER_INTERFACE);
        assert_eq!(spec.method, "ImportTopology");
        assert_eq!(spec.parameters.type_().as_str(), "(sos)");
    }

    #[test]
    fn zone_update_preserves_revision_and_canonical_member_shape() {
        let spec = Mutation::UpdateZone {
            path: "/io/github/Mr_Tao/SoundTouchPipeWire1/zones/z_1".to_owned(),
            expected_revision: 42,
            name: "Downstairs".to_owned(),
            members: vec![LogicalMember::new("speaker", "aabbccddeeff")],
            preferred_master: None,
            conflict_policy: "protected".to_owned(),
            resume_policy: "manual".to_owned(),
            auto_heal: "enabled".to_owned(),
        }
        .call_spec(REQUEST_ID)
        .expect("valid update");

        assert_eq!(spec.method, "Update");
        assert_eq!(spec.parameters.type_().as_str(), "(stsa(ss)(ss)sss)");
        let (_, revision, _, members, preferred, _, _, _) = spec
            .parameters
            .get::<(
                String,
                u64,
                String,
                Vec<(String, String)>,
                (String, String),
                String,
                String,
                String,
            )>()
            .expect("canonical zone update parameters");
        assert_eq!(revision, 42);
        assert_eq!(members, [("speaker".to_owned(), "aabbccddeeff".to_owned())]);
        assert_eq!(preferred, (String::new(), String::new()));
    }

    #[test]
    fn every_mutation_uses_the_published_dbus_signature() {
        let zone_path = "/io/github/Mr_Tao/SoundTouchPipeWire1/zones/z_1";
        let pair_path = "/io/github/Mr_Tao/SoundTouchPipeWire1/pairs/p_1";
        let member = LogicalMember::new("speaker", "aabbccddeeff");
        let cases = vec![
            (Mutation::Reconcile, "Reconcile", "(s)"),
            (
                Mutation::CreateZone {
                    name: "Zone".to_owned(),
                    members: vec![member.clone()],
                    preferred_master: Some(member),
                    conflict_policy: "inherit".to_owned(),
                    resume_policy: "inherit".to_owned(),
                    auto_heal: "inherit".to_owned(),
                },
                "CreateZone",
                "(ssa(ss)(ss)sss)",
            ),
            (
                Mutation::CreateStereoPair {
                    name: "Pair".to_owned(),
                    left_device_id: "aabbccddeeff".to_owned(),
                    right_device_id: "001122334455".to_owned(),
                },
                "CreateStereoPair",
                "(ssss)",
            ),
            (
                Mutation::UpdateDefaults {
                    conflict_policy: "protected".to_owned(),
                    resume_policy: "manual".to_owned(),
                    auto_heal: false,
                },
                "UpdateDefaults",
                "(sssb)",
            ),
            (
                Mutation::SetManageAllVerified {
                    expected_digest: "sha256:digest".to_owned(),
                    value: true,
                },
                "SetManageAllVerified",
                "(ssb)",
            ),
            (
                Mutation::SetDevicePolicy {
                    expected_digest: "sha256:digest".to_owned(),
                    device_id: "aabbccddeeff".to_owned(),
                    mode: "allow".to_owned(),
                },
                "SetDevicePolicy",
                "(ssss)",
            ),
            (
                Mutation::ActivateZone {
                    path: zone_path.to_owned(),
                    take_over: false,
                },
                "Activate",
                "(sb)",
            ),
            (
                Mutation::DissolveZone {
                    path: zone_path.to_owned(),
                },
                "Dissolve",
                "(s)",
            ),
            (
                Mutation::DeleteZone {
                    path: zone_path.to_owned(),
                    expected_revision: 3,
                },
                "Delete",
                "(st)",
            ),
            (
                Mutation::UpdateStereoPair {
                    path: pair_path.to_owned(),
                    expected_revision: 4,
                    name: "Pair".to_owned(),
                    left_device_id: "aabbccddeeff".to_owned(),
                    right_device_id: "001122334455".to_owned(),
                },
                "Update",
                "(stsss)",
            ),
            (
                Mutation::CreateStereoPairOnHardware {
                    path: pair_path.to_owned(),
                    take_over: false,
                },
                "CreateOnHardware",
                "(sb)",
            ),
            (
                Mutation::DissolveStereoPair {
                    path: pair_path.to_owned(),
                },
                "Dissolve",
                "(s)",
            ),
            (
                Mutation::DeleteStereoPair {
                    path: pair_path.to_owned(),
                    expected_revision: 5,
                },
                "Delete",
                "(st)",
            ),
        ];

        for (mutation, method, signature) in cases {
            let spec = mutation.call_spec(REQUEST_ID).expect("valid call spec");
            assert_eq!(spec.method, method);
            assert_eq!(spec.parameters.type_().as_str(), signature);
        }
    }

    #[test]
    fn take_over_is_explicit_in_hardware_calls() {
        let normal = Mutation::ActivateZone {
            path: "/io/github/Mr_Tao/SoundTouchPipeWire1/zones/z_1".to_owned(),
            take_over: false,
        }
        .call_spec(REQUEST_ID)
        .expect("normal activation");
        let forced = Mutation::ActivateZone {
            path: "/io/github/Mr_Tao/SoundTouchPipeWire1/zones/z_1".to_owned(),
            take_over: true,
        }
        .call_spec(REQUEST_ID)
        .expect("take-over activation");

        assert!(!normal.parameters.get::<(String, bool)>().unwrap().1);
        assert!(forced.parameters.get::<(String, bool)>().unwrap().1);
    }
}
