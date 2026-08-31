use gio::prelude::*;
use std::collections::{BTreeMap, HashMap};

pub const DBUS_NAME: &str = "io.github.Mr_Tao.SoundTouchPipeWire1";
pub const DBUS_ROOT: &str = "/io/github/Mr_Tao/SoundTouchPipeWire1";
pub const DBUS_INTERFACE_PREFIX: &str = "io.github.Mr_Tao.SoundTouchPipeWire1.";
pub const MANAGER_INTERFACE: &str = "io.github.Mr_Tao.SoundTouchPipeWire1.Manager";
pub const SUPPORTED_API_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum ObjectKind {
    Speaker,
    Zone,
    StereoPair,
    ObservedTopology,
    Operation,
}

impl ObjectKind {
    pub const ALL: [Self; 5] = [
        Self::Speaker,
        Self::Zone,
        Self::StereoPair,
        Self::ObservedTopology,
        Self::Operation,
    ];

    pub fn interface_suffix(self) -> &'static str {
        match self {
            Self::Speaker => "Speaker",
            Self::Zone => "ZonePreset",
            Self::StereoPair => "StereoPair",
            Self::ObservedTopology => "ObservedTopology",
            Self::Operation => "Operation",
        }
    }

    pub fn interface_name(self) -> String {
        format!("{DBUS_INTERFACE_PREFIX}{}", self.interface_suffix())
    }

    pub fn from_interface(interface_name: &str) -> Option<Self> {
        let suffix = interface_name.strip_prefix(DBUS_INTERFACE_PREFIX)?;
        Self::ALL
            .into_iter()
            .find(|kind| kind.interface_suffix() == suffix)
    }
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct LogicalMember {
    pub kind: String,
    pub id: String,
}

impl LogicalMember {
    pub fn new(kind: impl Into<String>, id: impl Into<String>) -> Self {
        Self {
            kind: kind.into(),
            id: id.into(),
        }
    }

    pub fn as_tuple(&self) -> (String, String) {
        (self.kind.clone(), self.id.clone())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpeakerDetails {
    pub id: String,
    pub model: String,
    pub online: bool,
    pub available: bool,
    pub health: String,
    pub write_quarantined: bool,
    pub capabilities_known: bool,
    pub stereo_pair_capable: bool,
    pub policy_mode: String,
    pub policy_reason: String,
}

impl SpeakerDetails {
    pub fn topology_actions_enabled(&self) -> bool {
        self.online && self.available && !self.write_quarantined && self.health == "active"
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ZoneDetails {
    pub id: String,
    pub revision: u64,
    pub members: Vec<LogicalMember>,
    pub preferred_master: Option<LogicalMember>,
    pub conflict_policy: String,
    pub resume_policy: String,
    pub auto_heal: String,
    pub state: String,
    pub available: bool,
    pub degraded: bool,
    pub sink_node_name: String,
    pub audio_state: Option<String>,
    pub audio_error: String,
    pub status_message: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StereoPairDetails {
    pub id: String,
    pub revision: u64,
    pub left_device_id: String,
    pub right_device_id: String,
    pub state: String,
    pub available: bool,
    pub observed_consistent: bool,
    pub status_message: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ObservedTopologyDetails {
    pub id: String,
    pub kind: String,
    pub origin: String,
    pub device_ids: Vec<String>,
    pub master_device_id: String,
    pub group_id: String,
    pub matched_preset_id: String,
    pub consistent: bool,
    pub external_source_active: bool,
    pub observed_at_usec: i64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OperationDetails {
    pub id: String,
    pub request_id: String,
    pub kind: String,
    pub initiator: String,
    pub state: String,
    pub phase: String,
    pub can_cancel: bool,
    pub error_name: String,
    pub error_message: String,
    pub created_at_usec: i64,
    pub updated_at_usec: i64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ObjectDetails {
    Speaker(SpeakerDetails),
    Zone(ZoneDetails),
    StereoPair(StereoPairDetails),
    ObservedTopology(ObservedTopologyDetails),
    Operation(OperationDetails),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ManagedObject {
    pub path: String,
    pub kind: ObjectKind,
    pub title: String,
    pub subtitle: String,
    pub status: Option<String>,
    pub details: ObjectDetails,
}

impl ManagedObject {
    pub fn zone(&self) -> Option<&ZoneDetails> {
        match &self.details {
            ObjectDetails::Zone(details) => Some(details),
            _ => None,
        }
    }

    pub fn stereo_pair(&self) -> Option<&StereoPairDetails> {
        match &self.details {
            ObjectDetails::StereoPair(details) => Some(details),
            _ => None,
        }
    }

    pub fn observed_topology(&self) -> Option<&ObservedTopologyDetails> {
        match &self.details {
            ObjectDetails::ObservedTopology(details) => Some(details),
            _ => None,
        }
    }

    pub fn operation(&self) -> Option<&OperationDetails> {
        match &self.details {
            ObjectDetails::Operation(details) => Some(details),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ServiceState {
    Online,
    Offline,
    Incompatible(CompatibilityIssue),
    Error(String),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CompatibilityIssue {
    ManagerUnavailable,
    OwnerGenerationMismatch,
    MissingApiVersion,
    UnsupportedApiVersion(u32),
    InvalidDefaults,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServiceSnapshot {
    pub state: ServiceState,
    pub objects: Vec<ManagedObject>,
    pub defaults: Option<ManagerDefaults>,
    pub receiver_access: Option<ReceiverAccessDetails>,
    pub owner_generation: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ManagerDefaults {
    pub conflict_policy: String,
    pub resume_policy: String,
    pub auto_heal: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ReceiverAccessDetails {
    pub manage_all_verified: bool,
    pub configured_manage_all_verified: bool,
    pub config_path: String,
    pub config_writable: bool,
    pub config_digest: String,
    pub restart_required: bool,
    pub configuration_error: String,
    pub device_policies: BTreeMap<String, String>,
    pub device_policy_count: u32,
    pub explicitly_allowed_device_count: u32,
    pub explicitly_blocked_device_count: u32,
    pub contract_complete: bool,
}

impl ServiceSnapshot {
    pub fn offline() -> Self {
        Self {
            state: ServiceState::Offline,
            objects: Vec::new(),
            defaults: None,
            receiver_access: None,
            owner_generation: None,
        }
    }

    pub fn error(message: impl Into<String>) -> Self {
        Self {
            state: ServiceState::Error(message.into()),
            objects: Vec::new(),
            defaults: None,
            receiver_access: None,
            owner_generation: None,
        }
    }

    fn incompatible(issue: CompatibilityIssue) -> Self {
        Self {
            state: ServiceState::Incompatible(issue),
            objects: Vec::new(),
            defaults: None,
            receiver_access: None,
            owner_generation: None,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct ManagerMetadata {
    owner_generation: String,
    defaults: ManagerDefaults,
    receiver_access: Option<ReceiverAccessDetails>,
}

pub fn snapshot(
    manager: &gio::DBusObjectManagerClient,
    manager_proxy: &gio::DBusProxy,
) -> ServiceSnapshot {
    let Some(object_manager_owner) = manager.name_owner() else {
        return ServiceSnapshot::offline();
    };
    let mut metadata = match validate_manager_metadata(
        object_manager_owner.as_str(),
        manager_proxy.name_owner().as_deref(),
        u32_property(manager_proxy, "ApiVersion"),
        manager_defaults(manager_proxy),
    ) {
        Ok(metadata) => metadata,
        Err(issue) => return ServiceSnapshot::incompatible(issue),
    };
    metadata.receiver_access = manager_receiver_access(manager_proxy);

    let mut objects = manager
        .objects()
        .into_iter()
        .flat_map(managed_objects_from_object)
        .collect::<Vec<_>>();
    sort_managed_objects(&mut objects);

    ServiceSnapshot {
        state: ServiceState::Online,
        objects,
        defaults: Some(metadata.defaults),
        receiver_access: metadata.receiver_access,
        owner_generation: Some(metadata.owner_generation),
    }
}

fn validate_manager_metadata(
    object_manager_owner: &str,
    manager_owner: Option<&str>,
    api_version: Option<u32>,
    defaults: Option<ManagerDefaults>,
) -> Result<ManagerMetadata, CompatibilityIssue> {
    let manager_owner = manager_owner.ok_or(CompatibilityIssue::ManagerUnavailable)?;
    if !gio::dbus_is_unique_name(object_manager_owner)
        || !gio::dbus_is_unique_name(manager_owner)
        || object_manager_owner != manager_owner
    {
        return Err(CompatibilityIssue::OwnerGenerationMismatch);
    }
    match api_version {
        None => return Err(CompatibilityIssue::MissingApiVersion),
        Some(SUPPORTED_API_VERSION) => {}
        Some(version) => return Err(CompatibilityIssue::UnsupportedApiVersion(version)),
    }
    let defaults = defaults.ok_or(CompatibilityIssue::InvalidDefaults)?;
    Ok(ManagerMetadata {
        owner_generation: object_manager_owner.to_owned(),
        defaults,
        receiver_access: None,
    })
}

fn manager_defaults(proxy: &gio::DBusProxy) -> Option<ManagerDefaults> {
    let conflict_policy = string_property(proxy, &["DefaultConflictPolicy"])?;
    let resume_policy = string_property(proxy, &["DefaultResumePolicy"])?;
    if !matches!(
        conflict_policy.as_str(),
        "protected" | "take-over-on-activation"
    ) || !matches!(resume_policy.as_str(), "manual" | "automatic")
    {
        return None;
    }
    Some(ManagerDefaults {
        conflict_policy,
        resume_policy,
        auto_heal: optional_bool_property(proxy, "DefaultAutoHeal")?,
    })
}

fn manager_receiver_access(proxy: &gio::DBusProxy) -> Option<ReceiverAccessDetails> {
    let manage_all_verified = optional_bool_property(proxy, "ManageAllVerified")?;
    let configured_manage_all_verified =
        optional_bool_property(proxy, "ConfiguredManageAllVerified");
    let config_path = single_string_property(proxy, "ConfigPath");
    let config_writable = optional_bool_property(proxy, "ConfigWritable");
    let config_digest = single_string_property(proxy, "ConfigDigest");
    let restart_required = optional_bool_property(proxy, "ConfigurationRestartRequired");
    let configuration_error = single_string_property(proxy, "ConfigurationError");
    let device_policies = string_map_property(proxy, "DevicePolicies");
    let device_policy_count = optional_u32_property(proxy, "DevicePolicyCount");
    let explicitly_allowed_device_count =
        optional_u32_property(proxy, "ExplicitlyAllowedDeviceCount");
    let explicitly_blocked_device_count =
        optional_u32_property(proxy, "ExplicitlyBlockedDeviceCount");

    let contract_complete = configured_manage_all_verified.is_some()
        && config_path.is_some()
        && config_writable.is_some()
        && config_digest.is_some()
        && restart_required.is_some()
        && configuration_error.is_some()
        && device_policies.is_some()
        && device_policy_count.is_some()
        && explicitly_allowed_device_count.is_some()
        && explicitly_blocked_device_count.is_some();

    Some(ReceiverAccessDetails {
        manage_all_verified,
        configured_manage_all_verified: configured_manage_all_verified
            .unwrap_or(manage_all_verified),
        config_path: config_path.unwrap_or_default(),
        config_writable: config_writable.unwrap_or(false),
        config_digest: config_digest.unwrap_or_default(),
        restart_required: restart_required.unwrap_or(false),
        configuration_error: configuration_error.unwrap_or_default(),
        device_policies: device_policies.unwrap_or_default(),
        device_policy_count: device_policy_count.unwrap_or(0),
        explicitly_allowed_device_count: explicitly_allowed_device_count.unwrap_or(0),
        explicitly_blocked_device_count: explicitly_blocked_device_count.unwrap_or(0),
        contract_complete,
    })
}

fn managed_objects_from_object(object: gio::DBusObject) -> Vec<ManagedObject> {
    let path = object.object_path().to_string();

    object
        .interfaces()
        .into_iter()
        .filter_map(|interface| interface.downcast::<gio::DBusProxy>().ok())
        .filter_map(|proxy| {
            let kind = ObjectKind::from_interface(proxy.interface_name().as_str())?;
            Some(managed_object_from_proxy(&path, kind, &proxy))
        })
        .collect()
}

fn managed_object_from_proxy(
    path: &str,
    kind: ObjectKind,
    proxy: &gio::DBusProxy,
) -> ManagedObject {
    let title = string_property(proxy, &["DisplayName", "Name", "Title", "Id", "DeviceId"])
        .unwrap_or_else(|| object_path_leaf(path));

    let status = string_property(proxy, &["Status", "State", "Health"]);
    let subtitle = subtitle_for(kind, proxy, path);
    let details = details_for(kind, proxy);

    ManagedObject {
        path: path.to_owned(),
        kind,
        title,
        subtitle,
        status,
        details,
    }
}

fn details_for(kind: ObjectKind, proxy: &gio::DBusProxy) -> ObjectDetails {
    match kind {
        ObjectKind::Speaker => ObjectDetails::Speaker(SpeakerDetails {
            id: string_property(proxy, &["Id"]).unwrap_or_default(),
            model: string_property(proxy, &["Model"]).unwrap_or_default(),
            online: bool_property(proxy, "Online"),
            available: bool_property(proxy, "Available"),
            health: string_property(proxy, &["Health"]).unwrap_or_default(),
            write_quarantined: bool_property(proxy, "WriteQuarantined"),
            capabilities_known: bool_property(proxy, "CapabilitiesKnown"),
            stereo_pair_capable: bool_property(proxy, "StereoPairCapable"),
            policy_mode: string_property(proxy, &["PolicyMode"]).unwrap_or_default(),
            policy_reason: string_property(proxy, &["PolicyReason"]).unwrap_or_default(),
        }),
        ObjectKind::Zone => ObjectDetails::Zone(ZoneDetails {
            id: string_property(proxy, &["Id"]).unwrap_or_default(),
            revision: u64_property(proxy, "Revision"),
            members: member_array_property(proxy, "Members"),
            preferred_master: member_property(proxy, "PreferredMaster"),
            conflict_policy: string_property(proxy, &["ConflictPolicy"]).unwrap_or_default(),
            resume_policy: string_property(proxy, &["ResumePolicy"]).unwrap_or_default(),
            auto_heal: string_property(proxy, &["AutoHeal"]).unwrap_or_default(),
            state: string_property(proxy, &["State"]).unwrap_or_default(),
            available: bool_property(proxy, "Available"),
            degraded: bool_property(proxy, "Degraded"),
            sink_node_name: string_property(proxy, &["SinkNodeName"]).unwrap_or_default(),
            audio_state: string_property(proxy, &["AudioState"]).filter(|state| !state.is_empty()),
            audio_error: string_property(proxy, &["AudioError"]).unwrap_or_default(),
            status_message: string_property(proxy, &["StatusMessage"]).unwrap_or_default(),
        }),
        ObjectKind::StereoPair => ObjectDetails::StereoPair(StereoPairDetails {
            id: string_property(proxy, &["Id"]).unwrap_or_default(),
            revision: u64_property(proxy, "Revision"),
            left_device_id: string_property(proxy, &["LeftDeviceId"]).unwrap_or_default(),
            right_device_id: string_property(proxy, &["RightDeviceId"]).unwrap_or_default(),
            state: string_property(proxy, &["State"]).unwrap_or_default(),
            available: bool_property(proxy, "Available"),
            observed_consistent: bool_property(proxy, "ObservedConsistent"),
            status_message: string_property(proxy, &["StatusMessage"]).unwrap_or_default(),
        }),
        ObjectKind::ObservedTopology => ObjectDetails::ObservedTopology(ObservedTopologyDetails {
            id: string_property(proxy, &["Id"]).unwrap_or_default(),
            kind: string_property(proxy, &["Kind"]).unwrap_or_default(),
            origin: string_property(proxy, &["Origin"]).unwrap_or_default(),
            device_ids: string_array_property(proxy, "DeviceIds"),
            master_device_id: string_property(proxy, &["MasterDeviceId"]).unwrap_or_default(),
            group_id: string_property(proxy, &["GroupId"]).unwrap_or_default(),
            matched_preset_id: string_property(proxy, &["MatchedPresetId"]).unwrap_or_default(),
            consistent: bool_property(proxy, "Consistent"),
            external_source_active: bool_property(proxy, "ExternalSourceActive"),
            observed_at_usec: i64_property(proxy, "ObservedAtUsec"),
        }),
        ObjectKind::Operation => ObjectDetails::Operation(OperationDetails {
            id: string_property(proxy, &["Id"]).unwrap_or_default(),
            request_id: string_property(proxy, &["RequestId"]).unwrap_or_default(),
            kind: string_property(proxy, &["Kind"]).unwrap_or_default(),
            initiator: string_property(proxy, &["Initiator"]).unwrap_or_default(),
            state: string_property(proxy, &["State"]).unwrap_or_default(),
            phase: string_property(proxy, &["Phase"]).unwrap_or_default(),
            can_cancel: bool_property(proxy, "CanCancel"),
            error_name: string_property(proxy, &["ErrorName"]).unwrap_or_default(),
            error_message: string_property(proxy, &["ErrorMessage"]).unwrap_or_default(),
            created_at_usec: i64_property(proxy, "CreatedAtUsec"),
            updated_at_usec: i64_property(proxy, "UpdatedAtUsec"),
        }),
    }
}

fn sort_managed_objects(objects: &mut [ManagedObject]) {
    objects.sort_by(|left, right| {
        left.kind
            .cmp(&right.kind)
            .then_with(|| match (&left.details, &right.details) {
                (ObjectDetails::Operation(left), ObjectDetails::Operation(right)) => right
                    .updated_at_usec
                    .cmp(&left.updated_at_usec)
                    .then_with(|| right.created_at_usec.cmp(&left.created_at_usec))
                    .then_with(|| right.id.cmp(&left.id)),
                (ObjectDetails::ObservedTopology(left), ObjectDetails::ObservedTopology(right)) => {
                    right
                        .observed_at_usec
                        .cmp(&left.observed_at_usec)
                        .then_with(|| left.id.cmp(&right.id))
                }
                _ => left
                    .title
                    .to_lowercase()
                    .cmp(&right.title.to_lowercase())
                    .then_with(|| left.path.cmp(&right.path)),
            })
    });
}

fn subtitle_for(kind: ObjectKind, proxy: &gio::DBusProxy, path: &str) -> String {
    let property_names: &[&str] = match kind {
        ObjectKind::Speaker => &["Error", "Model", "Id"],
        ObjectKind::Zone => &[
            "AudioError",
            "StatusMessage",
            "Members",
            "PreferredMaster",
            "SinkNodeName",
        ],
        ObjectKind::StereoPair => &["StatusMessage", "LeftDeviceId", "RightDeviceId"],
        ObjectKind::ObservedTopology => &["Kind", "DeviceIds", "Origin"],
        ObjectKind::Operation => &["ErrorMessage", "Phase", "Kind"],
    };

    property_names
        .iter()
        .filter_map(|name| display_property(proxy, name))
        .find(|value| !value.is_empty())
        .unwrap_or_else(|| path.to_owned())
}

fn object_path_leaf(path: &str) -> String {
    path.rsplit('/')
        .find(|part| !part.is_empty())
        .unwrap_or(path)
        .to_owned()
}

fn string_property(proxy: &gio::DBusProxy, names: &[&str]) -> Option<String> {
    names
        .iter()
        .find_map(|name| proxy.cached_property(name)?.get::<String>())
}

fn single_string_property(proxy: &gio::DBusProxy, name: &str) -> Option<String> {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<String>())
}

fn bool_property(proxy: &gio::DBusProxy, name: &str) -> bool {
    optional_bool_property(proxy, name).unwrap_or(false)
}

fn optional_bool_property(proxy: &gio::DBusProxy, name: &str) -> Option<bool> {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<bool>())
}

fn u32_property(proxy: &gio::DBusProxy, name: &str) -> Option<u32> {
    optional_u32_property(proxy, name)
}

fn optional_u32_property(proxy: &gio::DBusProxy, name: &str) -> Option<u32> {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<u32>())
}

fn string_map_property(proxy: &gio::DBusProxy, name: &str) -> Option<BTreeMap<String, String>> {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<HashMap<String, String>>())
        .map(|entries| entries.into_iter().collect())
}

fn u64_property(proxy: &gio::DBusProxy, name: &str) -> u64 {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<u64>())
        .unwrap_or(0)
}

fn i64_property(proxy: &gio::DBusProxy, name: &str) -> i64 {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<i64>())
        .unwrap_or(0)
}

fn member_array_property(proxy: &gio::DBusProxy, name: &str) -> Vec<LogicalMember> {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<Vec<(String, String)>>())
        .unwrap_or_default()
        .into_iter()
        .map(|(kind, id)| LogicalMember::new(kind, id))
        .collect()
}

fn member_property(proxy: &gio::DBusProxy, name: &str) -> Option<LogicalMember> {
    let (kind, id) = proxy.cached_property(name)?.get::<(String, String)>()?;
    (!kind.is_empty() && !id.is_empty()).then(|| LogicalMember::new(kind, id))
}

fn string_array_property(proxy: &gio::DBusProxy, name: &str) -> Vec<String> {
    proxy
        .cached_property(name)
        .and_then(|value| value.get::<Vec<String>>())
        .unwrap_or_default()
}

fn display_property(proxy: &gio::DBusProxy, name: &str) -> Option<String> {
    let value = proxy.cached_property(name)?;
    display_variant(&value)
}

fn display_variant(value: &glib::Variant) -> Option<String> {
    if let Some(text) = value.get::<String>() {
        return Some(text);
    }
    if let Some(items) = value.get::<Vec<(String, String)>>() {
        return Some(
            items
                .into_iter()
                .map(|(kind, id)| format!("{kind}: {id}"))
                .collect::<Vec<_>>()
                .join(", "),
        );
    }
    if let Some((kind, id)) = value.get::<(String, String)>() {
        if kind.is_empty() && id.is_empty() {
            return None;
        }
        return Some(format!("{kind}: {id}"));
    }
    if let Some(items) = value.get::<Vec<String>>() {
        return Some(items.join(", "));
    }
    if let Some(value) = value.get::<bool>() {
        return Some(if value { "yes" } else { "no" }.to_owned());
    }
    if let Some(value) = value.get::<u32>() {
        return Some(value.to_string());
    }
    if let Some(value) = value.get::<u64>() {
        return Some(value.to_string());
    }
    if let Some(value) = value.get::<i32>() {
        return Some(value.to_string());
    }
    if let Some(value) = value.get::<i64>() {
        return Some(value.to_string());
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_only_versioned_control_interfaces() {
        assert_eq!(
            ObjectKind::from_interface("io.github.Mr_Tao.SoundTouchPipeWire1.Speaker"),
            Some(ObjectKind::Speaker)
        );
        assert_eq!(
            ObjectKind::from_interface("io.github.Mr_Tao.SoundTouchPipeWire1.ZonePreset"),
            Some(ObjectKind::Zone)
        );
        assert_eq!(
            ObjectKind::from_interface("io.github.Mr_Tao.SoundTouchPipeWire2.Speaker"),
            None
        );
        assert_eq!(
            ObjectKind::from_interface("org.freedesktop.DBus.Properties"),
            None
        );
    }

    #[test]
    fn derives_readable_fallback_from_object_path() {
        assert_eq!(
            object_path_leaf("/example/speakers/s_deadbeef"),
            "s_deadbeef"
        );
        assert_eq!(object_path_leaf("/"), "/");
    }

    #[test]
    fn write_quarantine_does_not_erase_published_availability() {
        let mut speaker = SpeakerDetails {
            id: "aabbccddeeff".to_owned(),
            model: "SoundTouch 10".to_owned(),
            online: true,
            available: true,
            health: "write-quarantined".to_owned(),
            write_quarantined: true,
            capabilities_known: true,
            stereo_pair_capable: true,
            policy_mode: "auto".to_owned(),
            policy_reason: "verified-auto".to_owned(),
        };

        assert!(speaker.available);
        assert!(!speaker.topology_actions_enabled());
        speaker.write_quarantined = false;
        speaker.health = "recovering".to_owned();
        assert!(!speaker.topology_actions_enabled());
        speaker.health = "active".to_owned();
        assert!(speaker.topology_actions_enabled());
    }

    #[test]
    fn offline_snapshot_has_no_stale_objects() {
        assert_eq!(
            ServiceSnapshot::offline(),
            ServiceSnapshot {
                state: ServiceState::Offline,
                objects: Vec::new(),
                defaults: None,
                receiver_access: None,
                owner_generation: None,
            }
        );
    }

    fn valid_defaults() -> ManagerDefaults {
        ManagerDefaults {
            conflict_policy: "protected".to_owned(),
            resume_policy: "manual".to_owned(),
            auto_heal: true,
        }
    }

    #[test]
    fn accepts_only_matching_unique_owner_generation_and_api_one() {
        let metadata = validate_manager_metadata(
            ":1.42",
            Some(":1.42"),
            Some(SUPPORTED_API_VERSION),
            Some(valid_defaults()),
        )
        .expect("compatible manager");

        assert_eq!(metadata.owner_generation, ":1.42");
        assert_eq!(metadata.defaults, valid_defaults());
    }

    #[test]
    fn rejects_missing_mismatched_or_stale_manager_metadata() {
        assert_eq!(
            validate_manager_metadata(
                ":1.42",
                None,
                Some(SUPPORTED_API_VERSION),
                Some(valid_defaults())
            ),
            Err(CompatibilityIssue::ManagerUnavailable)
        );
        assert_eq!(
            validate_manager_metadata(
                ":1.42",
                Some(":1.43"),
                Some(SUPPORTED_API_VERSION),
                Some(valid_defaults())
            ),
            Err(CompatibilityIssue::OwnerGenerationMismatch)
        );
        assert_eq!(
            validate_manager_metadata(":1.42", Some(":1.42"), None, Some(valid_defaults())),
            Err(CompatibilityIssue::MissingApiVersion)
        );
        assert_eq!(
            validate_manager_metadata(":1.42", Some(":1.42"), Some(2), Some(valid_defaults())),
            Err(CompatibilityIssue::UnsupportedApiVersion(2))
        );
        assert_eq!(
            validate_manager_metadata(":1.42", Some(":1.42"), Some(SUPPORTED_API_VERSION), None),
            Err(CompatibilityIssue::InvalidDefaults)
        );
    }

    #[test]
    fn formats_logical_members_from_the_canonical_dbus_shape() {
        use glib::variant::ToVariant;

        let members = vec![
            ("speaker".to_owned(), "aabbccddeeff".to_owned()),
            ("stereo-pair".to_owned(), "pair-id".to_owned()),
        ]
        .to_variant();
        assert_eq!(
            display_variant(&members),
            Some("speaker: aabbccddeeff, stereo-pair: pair-id".to_owned())
        );

        let no_preferred_master = ("".to_owned(), "".to_owned()).to_variant();
        assert_eq!(display_variant(&no_preferred_master), None);
    }

    fn operation_object(id: &str, updated_at_usec: i64) -> ManagedObject {
        ManagedObject {
            path: format!("/operations/{id}"),
            kind: ObjectKind::Operation,
            title: id.to_owned(),
            subtitle: "completed".to_owned(),
            status: Some("succeeded".to_owned()),
            details: ObjectDetails::Operation(OperationDetails {
                id: id.to_owned(),
                request_id: format!("request-{id}"),
                kind: "reconcile".to_owned(),
                initiator: ":1.42".to_owned(),
                state: "succeeded".to_owned(),
                phase: "completed".to_owned(),
                can_cancel: false,
                error_name: String::new(),
                error_message: String::new(),
                created_at_usec: updated_at_usec - 1,
                updated_at_usec,
            }),
        }
    }

    #[test]
    fn operations_are_sorted_newest_first() {
        let mut objects = vec![
            operation_object("o_2", 200),
            operation_object("o_10", 1_000),
            operation_object("o_1", 100),
        ];

        sort_managed_objects(&mut objects);

        assert_eq!(
            objects
                .iter()
                .map(|object| object.title.as_str())
                .collect::<Vec<_>>(),
            vec!["o_10", "o_2", "o_1"]
        );
    }
}
