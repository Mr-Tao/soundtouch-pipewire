// SPDX-License-Identifier: MIT

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use wireplumber::core::ObjectFeatures;
use wireplumber::prelude::*;
use wireplumber::pw::{Device, Node};
use wireplumber::registry::{Interest, ObjectManager};
use wireplumber::spa::SpaPodBuilder;
use wireplumber::{Core, InitFlags};

use crate::model::{ReceiverSnapshot, normalize_device_id};
use crate::optimistic::RouteTuple;

#[cfg(test)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GraphObjectKind {
    Device,
    Node,
}

#[cfg(test)]
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GraphObject {
    pub global_id: u32,
    pub kind: GraphObjectKind,
    pub name: Option<String>,
    pub device_id: Option<String>,
}

#[cfg(test)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExactMatch {
    pub device_global_id: u32,
    pub node_global_id: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MatchToken {
    connection_generation: u64,
    device_global_id: u32,
    node_global_id: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MatchError {
    InvalidDeviceId,
    Missing,
    Ambiguous,
}

#[cfg(test)]
pub fn exact_match(
    device_name: &str,
    node_name: &str,
    device_id: &str,
    objects: &[GraphObject],
) -> Result<ExactMatch, MatchError> {
    let normalized = normalize_device_id(device_id).ok_or(MatchError::InvalidDeviceId)?;
    let devices = objects
        .iter()
        .filter(|object| {
            object.kind == GraphObjectKind::Device
                && object.name.as_deref() == Some(device_name)
                && object
                    .device_id
                    .as_deref()
                    .and_then(normalize_device_id)
                    .as_deref()
                    == Some(normalized.as_str())
        })
        .collect::<Vec<_>>();
    let nodes = objects
        .iter()
        .filter(|object| {
            object.kind == GraphObjectKind::Node
                && object.name.as_deref() == Some(node_name)
                && object
                    .device_id
                    .as_deref()
                    .and_then(normalize_device_id)
                    .as_deref()
                    == Some(normalized.as_str())
        })
        .collect::<Vec<_>>();
    match (devices.as_slice(), nodes.as_slice()) {
        ([device], [node]) => Ok(ExactMatch {
            device_global_id: device.global_id,
            node_global_id: node.global_id,
        }),
        ([], _) | (_, []) => Err(MatchError::Missing),
        _ => Err(MatchError::Ambiguous),
    }
}

pub struct PipeWireClient {
    core: Core,
    objects: ObjectManager,
    graph_epoch: Cell<u64>,
    connection_generation: Cell<u64>,
    connected: Cell<bool>,
    retry_source: RefCell<Option<glib::SourceId>>,
    on_graph_changed: Rc<dyn Fn(u64)>,
}

impl PipeWireClient {
    pub fn new(on_graph_changed: impl Fn(u64) + 'static) -> Rc<Self> {
        Core::init_with_flags(InitFlags::ALL);
        let context = glib::MainContext::default();
        let core = Core::new(Some(&context), None, None);
        let objects = ObjectManager::new();
        objects.add_interest(Interest::<Device>::new());
        objects.add_interest(Interest::<Node>::new());
        objects.request_object_features(Device::static_type(), ObjectFeatures::ALL);
        objects.request_object_features(Node::static_type(), ObjectFeatures::ALL);

        let client = Rc::new(Self {
            core,
            objects,
            graph_epoch: Cell::new(1),
            connection_generation: Cell::new(1),
            connected: Cell::new(false),
            retry_source: RefCell::new(None),
            on_graph_changed: Rc::new(on_graph_changed),
        });
        let weak = Rc::downgrade(&client);
        client.objects.connect_objects_changed(move |_| {
            if let Some(client) = weak.upgrade() {
                client.bump_graph_epoch();
            }
        });
        let weak = Rc::downgrade(&client);
        client.objects.connect_object_added(move |_, object| {
            let object_weak = weak.clone();
            if let Some(device) = object.downcast_ref::<Device>() {
                device.connect_properties_notify(move |_| {
                    if let Some(client) = object_weak.upgrade() {
                        client.bump_graph_epoch();
                    }
                });
            } else if let Some(node) = object.downcast_ref::<Node>() {
                node.connect_properties_notify(move |_| {
                    if let Some(client) = object_weak.upgrade() {
                        client.bump_graph_epoch();
                    }
                });
            }
        });
        let weak = Rc::downgrade(&client);
        client.core.connect_connected(move |_| {
            if let Some(client) = weak.upgrade() {
                client.connected.set(true);
                client
                    .connection_generation
                    .set(client.connection_generation.get().wrapping_add(1));
                if let Some(source) = client.retry_source.borrow_mut().take() {
                    source.remove();
                }
                client.bump_graph_epoch();
            }
        });
        let weak = Rc::downgrade(&client);
        client.core.connect_disconnected(move |_| {
            if let Some(client) = weak.upgrade() {
                client.connected.set(false);
                client
                    .connection_generation
                    .set(client.connection_generation.get().wrapping_add(1));
                client.bump_graph_epoch();
                client.schedule_reconnect();
            }
        });
        client.core.install_object_manager(&client.objects);
        if !client.core.connect() {
            client.bump_graph_epoch();
            client.schedule_reconnect();
        }
        client
    }

    pub fn match_token(&self, receiver: &ReceiverSnapshot) -> Result<MatchToken, MatchError> {
        self.resolve(receiver).map(|resolved| resolved.1)
    }

    pub fn dispatch(
        self: &Rc<Self>,
        receiver: &ReceiverSnapshot,
        expected_match: MatchToken,
        tuple: RouteTuple,
        callback: impl FnOnce(Result<(), String>) + 'static,
    ) {
        let (device, current_match) = match self.resolve(receiver) {
            Ok(resolved) => resolved,
            Err(error) => {
                callback(Err(match error {
                    MatchError::InvalidDeviceId => "invalid receiver device ID",
                    MatchError::Missing => "matching PipeWire Device or Node is absent",
                    MatchError::Ambiguous => "matching PipeWire Device or Node is ambiguous",
                }
                .into()));
                return;
            }
        };
        if current_match != expected_match {
            callback(Err(
                "matching PipeWire objects changed before dispatch".into()
            ));
            return;
        }
        let Some(route) = route_pod(tuple) else {
            callback(Err("failed to construct PipeWire Route parameter".into()));
            return;
        };
        if !device.set_param("Route", 0, route) {
            callback(Err("PipeWire rejected the Route dispatch".into()));
            return;
        }

        let weak = Rc::downgrade(self);
        let receiver = receiver.clone();
        self.core.sync(None::<&gio::Cancellable>, move |result| {
            let Some(client) = weak.upgrade() else {
                return;
            };
            if client.match_token(&receiver).ok() != Some(expected_match) {
                callback(Err(
                    "matching PipeWire objects changed during dispatch".into()
                ));
            } else {
                callback(result.map_err(|error| error.to_string()));
            }
        });
    }

    fn bump_graph_epoch(&self) {
        self.graph_epoch.set(self.graph_epoch.get().wrapping_add(1));
        (self.on_graph_changed)(self.graph_epoch.get());
    }

    fn schedule_reconnect(self: &Rc<Self>) {
        if self.connected.get() || self.retry_source.borrow().is_some() {
            return;
        }
        let weak = Rc::downgrade(self);
        let source = glib::timeout_add_local(std::time::Duration::from_secs(2), move || {
            let Some(client) = weak.upgrade() else {
                return glib::ControlFlow::Break;
            };
            if client.connected.get() {
                client.retry_source.borrow_mut().take();
                return glib::ControlFlow::Break;
            }
            let _ = client.core.connect();
            glib::ControlFlow::Continue
        });
        self.retry_source.replace(Some(source));
    }

    fn resolve(&self, receiver: &ReceiverSnapshot) -> Result<(Device, MatchToken), MatchError> {
        if !self.connected.get() || !self.objects.is_installed() {
            return Err(MatchError::Missing);
        }
        let normalized =
            normalize_device_id(&receiver.device_id).ok_or(MatchError::InvalidDeviceId)?;
        let devices = self
            .objects
            .objects::<Device>()
            .into_iter()
            .filter(|device| {
                device.get_pw_property("device.name").as_deref()
                    == Some(receiver.pipewire_device_name.as_str())
                    && device
                        .get_pw_property("soundtouch.device-id")
                        .as_deref()
                        .and_then(normalize_device_id)
                        .as_deref()
                        == Some(normalized.as_str())
            })
            .collect::<Vec<_>>();
        let nodes = self
            .objects
            .objects::<Node>()
            .into_iter()
            .filter(|node| {
                node.get_pw_property("node.name").as_deref()
                    == Some(receiver.pipewire_node_name.as_str())
                    && node
                        .get_pw_property("soundtouch.device-id")
                        .as_deref()
                        .and_then(normalize_device_id)
                        .as_deref()
                        == Some(normalized.as_str())
            })
            .collect::<Vec<_>>();
        match (devices.as_slice(), nodes.as_slice()) {
            ([device], [node]) => Ok((
                device.clone(),
                MatchToken {
                    connection_generation: self.connection_generation.get(),
                    device_global_id: device.bound_id(),
                    node_global_id: node.bound_id(),
                },
            )),
            ([], _) | (_, []) => Err(MatchError::Missing),
            _ => Err(MatchError::Ambiguous),
        }
    }
}

fn route_pod(tuple: RouteTuple) -> Option<wireplumber::spa::SpaPod> {
    let volume = (tuple.volume as f32 / 100.0).powi(3);
    let volumes = SpaPodBuilder::new_array();
    volumes.add_float(volume);
    let volumes = volumes.end()?;

    let props = SpaPodBuilder::new_object("Spa:Pod:Object:Param:Props", "Props");
    props.add_property("channelVolumes");
    props.add_pod(&volumes);
    props.add_property("mute");
    props.add_boolean(tuple.muted);
    let props = props.end()?;

    let route = SpaPodBuilder::new_object("Spa:Pod:Object:Param:Route", "Route");
    route.add_property("index");
    route.add_int(0);
    route.add_property("direction");
    route.add_id(wireplumber::spa::ffi::SPA_DIRECTION_OUTPUT);
    route.add_property("device");
    route.add_int(0);
    route.add_property("props");
    route.add_pod(&props);
    route.add_property("save");
    route.add_boolean(true);
    route.end()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn object(id: u32, kind: GraphObjectKind, name: &str, device_id: &str) -> GraphObject {
        GraphObject {
            global_id: id,
            kind,
            name: Some(name.into()),
            device_id: Some(device_id.into()),
        }
    }

    #[test]
    fn exact_device_and_node_match() {
        let objects = [
            object(
                10,
                GraphObjectKind::Device,
                "device.soundtouch.1",
                "AABBCCDDEEFF",
            ),
            object(
                11,
                GraphObjectKind::Node,
                "soundtouch.1",
                "aa:bb:cc:dd:ee:ff",
            ),
        ];
        assert_eq!(
            exact_match(
                "device.soundtouch.1",
                "soundtouch.1",
                "AABBCCDDEEFF",
                &objects
            ),
            Ok(ExactMatch {
                device_global_id: 10,
                node_global_id: 11
            })
        );
    }

    #[test]
    fn duplicates_fail_closed() {
        let objects = [
            object(
                10,
                GraphObjectKind::Device,
                "device.soundtouch.1",
                "AABBCCDDEEFF",
            ),
            object(
                12,
                GraphObjectKind::Device,
                "device.soundtouch.1",
                "AABBCCDDEEFF",
            ),
            object(11, GraphObjectKind::Node, "soundtouch.1", "AABBCCDDEEFF"),
        ];
        assert_eq!(
            exact_match(
                "device.soundtouch.1",
                "soundtouch.1",
                "AABBCCDDEEFF",
                &objects
            ),
            Err(MatchError::Ambiguous)
        );
    }

    #[test]
    fn node_only_and_reused_names_fail_closed() {
        let node_only = [object(
            11,
            GraphObjectKind::Node,
            "soundtouch.1",
            "AABBCCDDEEFF",
        )];
        assert_eq!(
            exact_match(
                "device.soundtouch.1",
                "soundtouch.1",
                "AABBCCDDEEFF",
                &node_only
            ),
            Err(MatchError::Missing)
        );

        let reused = [
            object(
                10,
                GraphObjectKind::Device,
                "device.soundtouch.1",
                "112233445566",
            ),
            object(11, GraphObjectKind::Node, "soundtouch.1", "112233445566"),
        ];
        assert_eq!(
            exact_match(
                "device.soundtouch.1",
                "soundtouch.1",
                "AABBCCDDEEFF",
                &reused
            ),
            Err(MatchError::Missing)
        );
    }
}
