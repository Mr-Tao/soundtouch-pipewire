// SPDX-License-Identifier: MIT

use std::cell::RefCell;
use std::rc::{Rc, Weak};

use gio::prelude::*;

use crate::model::{
    ConfirmedTuple, DBUS_NAME, DBUS_ROOT, DiscoveryHealth, Lifecycle, MANAGER_INTERFACE,
    RECEIVER_INTERFACE, ReceiverSnapshot, SUPPORTED_API_VERSION, ServiceSnapshot, ServiceState,
    normalize_device_id,
};

const OBJECT_MANAGER_FLAGS: gio::DBusObjectManagerClientFlags =
    gio::DBusObjectManagerClientFlags::DO_NOT_AUTO_START;
const PROXY_FLAGS: gio::DBusProxyFlags = gio::DBusProxyFlags::DO_NOT_AUTO_START;

type SnapshotCallback = dyn Fn(ServiceSnapshot);

pub struct StatusClient {
    manager: RefCell<Option<gio::DBusObjectManagerClient>>,
    manager_proxy: RefCell<Option<gio::DBusProxy>>,
    authority: RefCell<OwnerLifecycle>,
    refresh_source: RefCell<Option<glib::SourceId>>,
    on_snapshot: Rc<SnapshotCallback>,
}

#[derive(Debug, Default, Eq, PartialEq)]
struct OwnerLifecycle {
    owner: Option<String>,
    epoch: u64,
}

impl OwnerLifecycle {
    fn follow(&mut self, next_owner: Option<String>) -> Option<u64> {
        if self.owner == next_owner {
            return None;
        }
        self.epoch = self.epoch.wrapping_add(1);
        self.owner = next_owner;
        Some(self.epoch)
    }

    fn accepts_epoch(&self, epoch: u64) -> bool {
        self.epoch == epoch
    }

    fn accepts_owner_callback(&self, epoch: u64, owner: &str) -> bool {
        self.accepts_epoch(epoch) && self.owner.as_deref() == Some(owner)
    }
}

impl StatusClient {
    pub fn new(on_snapshot: impl Fn(ServiceSnapshot) + 'static) -> Rc<Self> {
        Rc::new(Self {
            manager: RefCell::new(None),
            manager_proxy: RefCell::new(None),
            authority: RefCell::new(OwnerLifecycle::default()),
            refresh_source: RefCell::new(None),
            on_snapshot: Rc::new(on_snapshot),
        })
    }

    pub fn start(self: &Rc<Self>) {
        let appeared = Rc::downgrade(self);
        let vanished = Rc::downgrade(self);
        let _watcher = gio::bus_watch_name(
            gio::BusType::Session,
            DBUS_NAME,
            gio::BusNameWatcherFlags::NONE,
            move |_, _, owner| {
                if let Some(client) = appeared.upgrade() {
                    client.follow_owner(Some(owner.to_string()));
                }
            },
            move |_, _| {
                if let Some(client) = vanished.upgrade() {
                    client.follow_owner(None);
                }
            },
        );
    }

    fn follow_owner(self: &Rc<Self>, next_owner: Option<String>) {
        let Some(epoch) = self.authority.borrow_mut().follow(next_owner.clone()) else {
            if next_owner.is_none() {
                (self.on_snapshot)(ServiceSnapshot::absent(self.authority.borrow().epoch));
            }
            return;
        };

        self.manager.borrow_mut().take();
        self.manager_proxy.borrow_mut().take();
        if let Some(source) = self.refresh_source.borrow_mut().take() {
            source.remove();
        }

        let Some(owner) = next_owner else {
            (self.on_snapshot)(ServiceSnapshot::absent(epoch));
            return;
        };
        (self.on_snapshot)(ServiceSnapshot::connecting(epoch, owner.clone()));

        let weak = Rc::downgrade(self);
        glib::MainContext::default().spawn_local(async move {
            let result = async {
                let manager = gio::DBusObjectManagerClient::new_for_bus_future(
                    gio::BusType::Session,
                    OBJECT_MANAGER_FLAGS,
                    &owner,
                    DBUS_ROOT,
                )
                .await?;
                let proxy = gio::DBusProxy::new_future(
                    &manager.connection(),
                    PROXY_FLAGS,
                    None,
                    Some(&owner),
                    DBUS_ROOT,
                    MANAGER_INTERFACE,
                )
                .await?;
                Ok::<_, glib::Error>((manager, proxy))
            }
            .await;

            let Some(client) = weak.upgrade() else {
                return;
            };
            if !client
                .authority
                .borrow()
                .accepts_owner_callback(epoch, &owner)
            {
                return;
            }
            match result {
                Ok((manager, proxy))
                    if manager.name_owner().as_deref() == Some(owner.as_str())
                        && proxy.name_owner().as_deref() == Some(owner.as_str()) =>
                {
                    client.install_pinned(epoch, manager, proxy);
                }
                _ => {
                    client.manager.borrow_mut().take();
                    client.manager_proxy.borrow_mut().take();
                    client.emit_incompatible(epoch, None);
                }
            }
        });
    }

    fn install_pinned(
        self: &Rc<Self>,
        epoch: u64,
        manager: gio::DBusObjectManagerClient,
        proxy: gio::DBusProxy,
    ) {
        let refresh = |weak: &Weak<Self>, callback_epoch| {
            if let Some(client) = weak.upgrade()
                && client.authority.borrow().accepts_epoch(callback_epoch)
            {
                client.schedule_refresh(callback_epoch);
            }
        };

        let immediate_refresh = |weak: &Weak<Self>, callback_epoch| {
            if let Some(client) = weak.upgrade()
                && client.authority.borrow().accepts_epoch(callback_epoch)
            {
                client.refresh(callback_epoch);
            }
        };
        let weak = Rc::downgrade(self);
        manager.connect_object_added(move |_, _| immediate_refresh(&weak, epoch));
        let weak = Rc::downgrade(self);
        manager.connect_object_removed(move |_, _| immediate_refresh(&weak, epoch));
        let weak = Rc::downgrade(self);
        manager.connect_interface_added(move |_, _, _| immediate_refresh(&weak, epoch));
        let weak = Rc::downgrade(self);
        manager.connect_interface_removed(move |_, _, _| immediate_refresh(&weak, epoch));
        let weak = Rc::downgrade(self);
        manager.connect_local("interface-proxy-properties-changed", false, move |values| {
            let revoke_now = values
                .get(2)
                .and_then(|value| value.get::<gio::DBusProxy>().ok())
                .is_some_and(|proxy| receiver_change_revokes_authority(&proxy));
            if revoke_now {
                immediate_refresh(&weak, epoch);
            } else {
                refresh(&weak, epoch);
            }
            None
        });
        let weak = Rc::downgrade(self);
        let changed_proxy = proxy.clone();
        proxy.connect_local("g-properties-changed", false, move |_| {
            if property::<u32>(&changed_proxy, "ApiVersion") != Some(SUPPORTED_API_VERSION) {
                immediate_refresh(&weak, epoch);
            } else {
                refresh(&weak, epoch);
            }
            None
        });

        self.manager.replace(Some(manager));
        self.manager_proxy.replace(Some(proxy));
        self.refresh(epoch);
    }

    fn schedule_refresh(self: &Rc<Self>, epoch: u64) {
        if self.refresh_source.borrow().is_some() {
            return;
        }
        let weak = Rc::downgrade(self);
        let source = glib::idle_add_local_once(move || {
            if let Some(client) = weak.upgrade()
                && client.authority.borrow().accepts_epoch(epoch)
            {
                client.refresh_source.borrow_mut().take();
                client.refresh(epoch);
            }
        });
        self.refresh_source.replace(Some(source));
    }

    fn refresh(&self, epoch: u64) {
        if !self.authority.borrow().accepts_epoch(epoch) {
            return;
        }
        let manager = self.manager.borrow();
        let manager_proxy = self.manager_proxy.borrow();
        let (Some(manager), Some(manager_proxy), Some(owner)) = (
            manager.as_ref(),
            manager_proxy.as_ref(),
            self.authority.borrow().owner.clone(),
        ) else {
            return;
        };
        (self.on_snapshot)(snapshot(epoch, owner, manager, manager_proxy));
    }

    fn emit_incompatible(&self, epoch: u64, api_version: Option<u32>) {
        (self.on_snapshot)(ServiceSnapshot {
            owner_epoch: epoch,
            owner: self.authority.borrow().owner.clone(),
            state: ServiceState::Incompatible { api_version },
            discovery_health: DiscoveryHealth::Unknown,
            detail: String::new(),
            receivers: Vec::new(),
        });
    }
}

fn receiver_change_revokes_authority(proxy: &gio::DBusProxy) -> bool {
    proxy.interface_name() == RECEIVER_INTERFACE
        && property::<bool>(proxy, "ControlAvailable") != Some(true)
}

fn snapshot(
    owner_epoch: u64,
    owner: String,
    manager: &gio::DBusObjectManagerClient,
    manager_proxy: &gio::DBusProxy,
) -> ServiceSnapshot {
    let api_version = property::<u32>(manager_proxy, "ApiVersion");
    if api_version != Some(SUPPORTED_API_VERSION) {
        return ServiceSnapshot {
            owner_epoch,
            owner: Some(owner),
            state: ServiceState::Incompatible { api_version },
            discovery_health: DiscoveryHealth::Unknown,
            detail: String::new(),
            receivers: Vec::new(),
        };
    }

    let discovery_health = property::<String>(manager_proxy, "DiscoveryHealth")
        .as_deref()
        .map(DiscoveryHealth::parse)
        .unwrap_or(DiscoveryHealth::Unknown);
    let detail = property::<String>(manager_proxy, "Detail").unwrap_or_default();
    let mut invalid_receiver = false;
    let mut receivers = manager
        .objects()
        .into_iter()
        .filter_map(|object| {
            let path = object.object_path().to_string();
            let interface = gio::prelude::DBusObjectExt::interface(&object, RECEIVER_INTERFACE)?;
            let proxy = interface.downcast::<gio::DBusProxy>().ok()?;
            match receiver_from_proxy(path, &proxy) {
                Some(receiver) => {
                    invalid_receiver |= receiver.lifecycle == Lifecycle::Unknown;
                    Some(receiver)
                }
                None => {
                    invalid_receiver = true;
                    None
                }
            }
        })
        .collect::<Vec<_>>();
    receivers.sort_by(|left, right| {
        left.display_name
            .to_lowercase()
            .cmp(&right.display_name.to_lowercase())
            .then_with(|| left.device_id.cmp(&right.device_id))
    });

    let state = if discovery_health == DiscoveryHealth::Ready && !invalid_receiver {
        ServiceState::Ready
    } else {
        ServiceState::Degraded
    };
    ServiceSnapshot {
        owner_epoch,
        owner: Some(owner),
        state,
        discovery_health,
        detail,
        receivers,
    }
}

fn receiver_from_proxy(path: String, proxy: &gio::DBusProxy) -> Option<ReceiverSnapshot> {
    let raw_device_id = property::<String>(proxy, "DeviceId")?;
    let device_id = normalize_device_id(&raw_device_id)?;
    let display_name = property::<String>(proxy, "DisplayName")?;
    let lifecycle = Lifecycle::parse(&property::<String>(proxy, "Lifecycle")?);
    let detail = property::<String>(proxy, "Detail")?;
    let pipewire_device_name = property::<String>(proxy, "PipeWireDeviceName")?;
    let pipewire_node_name = property::<String>(proxy, "PipeWireNodeName")?;
    let control_available = property::<bool>(proxy, "ControlAvailable")?;
    let control_busy = property::<bool>(proxy, "ControlBusy")?;
    let volume = property::<u32>(proxy, "Volume")?;
    let muted = property::<bool>(proxy, "Muted")?;
    let confirmed_revision = property::<u64>(proxy, "ConfirmedRevision")?;
    let confirmed = ConfirmedTuple::new(volume, muted)?;
    Some(ReceiverSnapshot {
        path,
        device_id,
        display_name,
        lifecycle,
        detail,
        pipewire_device_name,
        pipewire_node_name,
        control_available,
        control_busy,
        confirmed,
        confirmed_revision,
    })
}

fn property<T: glib::variant::FromVariant>(proxy: &gio::DBusProxy, name: &str) -> Option<T> {
    proxy.cached_property(name)?.get::<T>()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unique_owner_replacement_rejects_delayed_old_owner_callbacks() {
        let mut lifecycle = OwnerLifecycle::default();
        let first_epoch = lifecycle.follow(Some(":1.42".into())).unwrap();
        assert!(lifecycle.accepts_owner_callback(first_epoch, ":1.42"));

        let replacement_epoch = lifecycle.follow(Some(":1.43".into())).unwrap();
        assert_ne!(replacement_epoch, first_epoch);
        assert!(!lifecycle.accepts_owner_callback(first_epoch, ":1.42"));
        assert!(!lifecycle.accepts_owner_callback(replacement_epoch, ":1.42"));
        assert!(lifecycle.accepts_owner_callback(replacement_epoch, ":1.43"));
    }

    #[test]
    fn unique_owner_removal_rejects_delayed_old_epoch_callbacks() {
        let mut lifecycle = OwnerLifecycle::default();
        let owner_epoch = lifecycle.follow(Some(":1.42".into())).unwrap();
        let absent_epoch = lifecycle.follow(None).unwrap();

        assert_ne!(absent_epoch, owner_epoch);
        assert!(!lifecycle.accepts_epoch(owner_epoch));
        assert!(!lifecycle.accepts_owner_callback(owner_epoch, ":1.42"));
        assert!(lifecycle.accepts_epoch(absent_epoch));
        assert_eq!(lifecycle.owner, None);
    }
}
