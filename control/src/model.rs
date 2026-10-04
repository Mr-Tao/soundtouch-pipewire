// SPDX-License-Identifier: MIT

pub const DBUS_NAME: &str = "io.github.Mr_Tao.SoundTouchPipeWire2";
pub const DBUS_ROOT: &str = "/io/github/Mr_Tao/SoundTouchPipeWire2";
pub const MANAGER_INTERFACE: &str = "io.github.Mr_Tao.SoundTouchPipeWire2.Manager";
pub const RECEIVER_INTERFACE: &str = "io.github.Mr_Tao.SoundTouchPipeWire2.Receiver";
pub const SUPPORTED_API_VERSION: u32 = 2;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ConfirmedTuple {
    pub volume: u32,
    pub muted: bool,
}

impl ConfirmedTuple {
    pub fn new(volume: u32, muted: bool) -> Option<Self> {
        (volume <= 100).then_some(Self { volume, muted })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Lifecycle {
    Active,
    Degraded,
    Unknown,
}

impl Lifecycle {
    pub fn parse(value: &str) -> Self {
        match value {
            "active" => Self::Active,
            "degraded" => Self::Degraded,
            _ => Self::Unknown,
        }
    }

    pub fn can_mutate(self) -> bool {
        !matches!(self, Self::Unknown)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ReceiverSnapshot {
    pub path: String,
    pub device_id: String,
    pub display_name: String,
    pub lifecycle: Lifecycle,
    pub detail: String,
    pub pipewire_device_name: String,
    pub pipewire_node_name: String,
    pub control_available: bool,
    pub control_busy: bool,
    pub confirmed: ConfirmedTuple,
    pub confirmed_revision: u64,
}

impl ReceiverSnapshot {
    pub fn mutating_available(&self) -> bool {
        self.control_available && self.lifecycle.can_mutate()
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DiscoveryHealth {
    Starting,
    Ready,
    Degraded,
    Unknown,
}

impl DiscoveryHealth {
    pub fn parse(value: &str) -> Self {
        match value {
            "starting" => Self::Starting,
            "ready" => Self::Ready,
            "degraded" => Self::Degraded,
            _ => Self::Unknown,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ServiceState {
    Absent,
    Connecting,
    Ready,
    Degraded,
    Incompatible { api_version: Option<u32> },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServiceSnapshot {
    pub owner_epoch: u64,
    pub owner: Option<String>,
    pub state: ServiceState,
    pub discovery_health: DiscoveryHealth,
    pub detail: String,
    pub receivers: Vec<ReceiverSnapshot>,
}

impl ServiceSnapshot {
    pub fn absent(owner_epoch: u64) -> Self {
        Self {
            owner_epoch,
            owner: None,
            state: ServiceState::Absent,
            discovery_health: DiscoveryHealth::Starting,
            detail: String::new(),
            receivers: Vec::new(),
        }
    }

    pub fn connecting(owner_epoch: u64, owner: String) -> Self {
        Self {
            owner_epoch,
            owner: Some(owner),
            state: ServiceState::Connecting,
            discovery_health: DiscoveryHealth::Starting,
            detail: String::new(),
            receivers: Vec::new(),
        }
    }

    pub fn compatible_ready(&self) -> bool {
        matches!(self.state, ServiceState::Ready) && self.discovery_health == DiscoveryHealth::Ready
    }
}

pub fn normalize_device_id(value: &str) -> Option<String> {
    let mut normalized = String::with_capacity(12);
    for character in value.chars() {
        if matches!(character, ':' | '-' | '.') || character.is_ascii_whitespace() {
            continue;
        }
        if !character.is_ascii_hexdigit() || normalized.len() >= 12 {
            return None;
        }
        normalized.push(character.to_ascii_uppercase());
    }
    (normalized.len() == 12).then_some(normalized)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_id_normalization_matches_service_rules() {
        assert_eq!(
            normalize_device_id("aa:bb-cc.dd ee ff"),
            Some("AABBCCDDEEFF".into())
        );
        assert_eq!(normalize_device_id("AABBCCDDEEF"), None);
        assert_eq!(normalize_device_id("AABBCCDDEEFG"), None);
    }

    #[test]
    fn unknown_lifecycle_is_non_mutating() {
        assert!(!Lifecycle::parse("future-state").can_mutate());
        assert!(Lifecycle::parse("degraded").can_mutate());
    }
}
