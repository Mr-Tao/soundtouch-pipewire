// SPDX-License-Identifier: MIT

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use gio::prelude::*;
use glib::variant::ToVariant;

use crate::i18n::tr;

const ITEM_PATH: &str = "/StatusNotifierItem";
const MENU_PATH: &str = "/StatusNotifierItem/Menu";
const ITEM_INTERFACE: &str = "org.kde.StatusNotifierItem";
const MENU_INTERFACE: &str = "com.canonical.dbusmenu";
const WATCHER_NAME: &str = "org.kde.StatusNotifierWatcher";
const WATCHER_PATH: &str = "/StatusNotifierWatcher";
const WATCHER_INTERFACE: &str = "org.kde.StatusNotifierWatcher";
const ONLINE_ICON_NAME: &str = "io.github.Mr_Tao.SoundTouchPipeWire.Control";
const DEGRADED_ICON_NAME: &str = "dialog-warning-symbolic";
const UNAVAILABLE_ICON_NAME: &str = "network-offline-symbolic";

const INTROSPECTION_XML: &str = r#"
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="u" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
    <signal name="NewStatus"><arg type="s"/></signal>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewToolTip"/>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout"><arg name="parentId" type="i" direction="in"/><arg name="recursionDepth" type="i" direction="in"/><arg name="propertyNames" type="as" direction="in"/><arg name="revision" type="u" direction="out"/><arg name="layout" type="(ia{sv}av)" direction="out"/></method>
    <method name="GetGroupProperties"><arg name="ids" type="ai" direction="in"/><arg name="propertyNames" type="as" direction="in"/><arg name="properties" type="a(ia{sv})" direction="out"/></method>
    <method name="Event"><arg name="id" type="i" direction="in"/><arg name="eventId" type="s" direction="in"/><arg name="data" type="v" direction="in"/><arg name="timestamp" type="u" direction="in"/></method>
    <method name="EventGroup"><arg name="events" type="a(isvu)" direction="in"/><arg name="idErrors" type="ai" direction="out"/></method>
    <method name="AboutToShow"><arg name="id" type="i" direction="in"/><arg name="needUpdate" type="b" direction="out"/></method>
    <method name="AboutToShowGroup"><arg name="ids" type="ai" direction="in"/><arg name="updatesNeeded" type="ai" direction="out"/><arg name="idErrors" type="ai" direction="out"/></method>
    <signal name="LayoutUpdated"><arg name="revision" type="u"/><arg name="parent" type="i"/></signal>
    <signal name="ItemsPropertiesUpdated"><arg name="updatedProps" type="a(ia{sv})"/><arg name="removedProps" type="a(ias)"/></signal>
    <signal name="ItemActivationRequested"><arg name="id" type="i"/><arg name="timestamp" type="u"/></signal>
  </interface>
</node>
"#;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MenuAction {
    Open,
    About,
    Quit,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum IndicatorState {
    Online,
    Degraded,
    Unavailable,
}

impl IndicatorState {
    fn status(self) -> &'static str {
        match self {
            Self::Online => "Active",
            Self::Degraded | Self::Unavailable => "NeedsAttention",
        }
    }

    fn icon_name(self) -> &'static str {
        match self {
            Self::Online => ONLINE_ICON_NAME,
            Self::Degraded => DEGRADED_ICON_NAME,
            Self::Unavailable => UNAVAILABLE_ICON_NAME,
        }
    }

    fn attention_icon_name(self) -> &'static str {
        match self {
            Self::Online => "",
            Self::Degraded | Self::Unavailable => self.icon_name(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct IndicatorPresentation {
    state: IndicatorState,
    tooltip: String,
}

pub fn menu_action(id: i32, event: &str) -> Option<MenuAction> {
    if event != "clicked" {
        return None;
    }
    match id {
        1 => Some(MenuAction::Open),
        2 => Some(MenuAction::About),
        3 => Some(MenuAction::Quit),
        _ => None,
    }
}

fn item_layout(id: i32, label: &str) -> glib::Variant {
    let properties = menu_item_properties(label);
    glib::Variant::tuple_from_iter([
        id.to_variant(),
        properties.to_variant(),
        Vec::<glib::Variant>::new().to_variant(),
    ])
}

fn menu_item_properties(label: &str) -> BTreeMap<String, glib::Variant> {
    BTreeMap::from([
        ("label".to_string(), label.to_variant()),
        ("enabled".to_string(), true.to_variant()),
        ("visible".to_string(), true.to_variant()),
    ])
}

fn group_properties(ids: &[i32]) -> Vec<(i32, BTreeMap<String, glib::Variant>)> {
    ids.iter()
        .filter_map(|id| match id {
            0 => Some((
                0,
                BTreeMap::from([("children-display".to_string(), "submenu".to_variant())]),
            )),
            1 => Some((1, menu_item_properties(&tr("Open")))),
            2 => Some((2, menu_item_properties(&tr("About")))),
            3 => Some((3, menu_item_properties(&tr("Quit")))),
            _ => None,
        })
        .collect()
}

pub fn menu_layout(open: &str, about: &str, quit: &str) -> glib::Variant {
    let root_properties =
        BTreeMap::from([("children-display".to_string(), "submenu".to_variant())]);
    let children = vec![
        item_layout(1, open),
        item_layout(2, about),
        item_layout(3, quit),
    ];
    let root = glib::Variant::tuple_from_iter([
        0i32.to_variant(),
        root_properties.to_variant(),
        children.to_variant(),
    ]);
    glib::Variant::tuple_from_iter([1u32.to_variant(), root])
}

pub struct StatusNotifier {
    connection: gio::DBusConnection,
    presentation: Rc<RefCell<IndicatorPresentation>>,
    _item_registration: gio::RegistrationId,
    _menu_registration: gio::RegistrationId,
}

impl StatusNotifier {
    pub fn new(
        application: &gtk::Application,
        on_open: impl Fn() + 'static,
        on_about: impl Fn() + 'static,
        on_quit: impl Fn() + 'static,
    ) -> Result<Rc<Self>, glib::Error> {
        let connection = application.dbus_connection().ok_or_else(|| {
            glib::Error::new(
                gio::IOErrorEnum::NotConnected,
                "GTK application has no session bus connection",
            )
        })?;
        let node = gio::DBusNodeInfo::for_xml(INTROSPECTION_XML)?;
        let item_interface = node
            .lookup_interface(ITEM_INTERFACE)
            .expect("item interface is present");
        let menu_interface = node
            .lookup_interface(MENU_INTERFACE)
            .expect("menu interface is present");

        let open = Rc::new(on_open);
        let about = Rc::new(on_about);
        let quit = Rc::new(on_quit);
        let presentation = Rc::new(RefCell::new(IndicatorPresentation {
            state: IndicatorState::Unavailable,
            tooltip: tr("SoundTouch service is unavailable"),
        }));
        let open_method = open.clone();
        let item_presentation = presentation.clone();
        let item_registration = connection
            .register_object(ITEM_PATH, &item_interface)
            .method_call(move |_, _, _, _, method, _, invocation| {
                if matches!(method, "Activate" | "SecondaryActivate") {
                    open_method();
                }
                invocation.return_value(None);
            })
            .property(move |_, _, _, _, property| {
                item_property(property, &item_presentation.borrow())
                    .unwrap_or_else(|| "".to_variant())
            })
            .build()?;

        let open_menu = open;
        let about_menu = about;
        let quit_menu = quit;
        let menu_registration = connection
            .register_object(MENU_PATH, &menu_interface)
            .method_call(
                move |_, _, _, _, method, parameters, invocation| match method {
                    "GetLayout" => invocation.return_value(Some(&menu_layout(
                        &tr("Open"),
                        &tr("About"),
                        &tr("Quit"),
                    ))),
                    "GetGroupProperties" => {
                        let ids = parameters
                            .get::<(Vec<i32>, Vec<String>)>()
                            .map(|parameters| parameters.0)
                            .unwrap_or_default();
                        invocation.return_value(Some(&(group_properties(&ids),).to_variant()));
                    }
                    "Event" => {
                        if let Some((id, event, _, _)) =
                            parameters.get::<(i32, String, glib::Variant, u32)>()
                        {
                            match menu_action(id, &event) {
                                Some(MenuAction::Open) => open_menu(),
                                Some(MenuAction::About) => about_menu(),
                                Some(MenuAction::Quit) => quit_menu(),
                                None => {}
                            }
                        }
                        invocation.return_value(None);
                    }
                    "EventGroup" => {
                        if let Some(events) = parameters
                            .get::<(Vec<(i32, String, glib::Variant, u32)>,)>()
                            .map(|parameters| parameters.0)
                        {
                            for (id, event, _, _) in events {
                                match menu_action(id, &event) {
                                    Some(MenuAction::Open) => open_menu(),
                                    Some(MenuAction::About) => about_menu(),
                                    Some(MenuAction::Quit) => quit_menu(),
                                    None => {}
                                }
                            }
                        }
                        invocation.return_value(Some(&(Vec::<i32>::new(),).to_variant()))
                    }
                    "AboutToShow" => invocation.return_value(Some(&(false,).to_variant())),
                    "AboutToShowGroup" => invocation
                        .return_value(Some(&(Vec::<i32>::new(), Vec::<i32>::new()).to_variant())),
                    _ => invocation.return_dbus_error(
                        "org.freedesktop.DBus.Error.UnknownMethod",
                        "Unknown dbusmenu method",
                    ),
                },
            )
            .property(|_, _, _, _, property| {
                menu_property(property).unwrap_or_else(|| "".to_variant())
            })
            .build()?;

        let notifier = Rc::new(Self {
            connection,
            presentation,
            _item_registration: item_registration,
            _menu_registration: menu_registration,
        });
        notifier.connect_watcher();
        Ok(notifier)
    }

    pub fn update(&self, state: IndicatorState, tooltip: &str) {
        let next = IndicatorPresentation {
            state,
            tooltip: tooltip.to_owned(),
        };
        let previous = self.presentation.replace(next);
        if previous.state.status() != state.status() {
            let _ = self.connection.emit_signal(
                None,
                ITEM_PATH,
                ITEM_INTERFACE,
                "NewStatus",
                Some(&(state.status(),).to_variant()),
            );
        }
        if previous.state != state {
            for signal in ["NewIcon", "NewAttentionIcon"] {
                let _ = self
                    .connection
                    .emit_signal(None, ITEM_PATH, ITEM_INTERFACE, signal, None);
            }
        }
        if previous.tooltip != tooltip || previous.state != state {
            let _ =
                self.connection
                    .emit_signal(None, ITEM_PATH, ITEM_INTERFACE, "NewToolTip", None);
        }
    }

    fn connect_watcher(self: &Rc<Self>) {
        let weak = Rc::downgrade(self);
        let _watcher = gio::bus_watch_name(
            gio::BusType::Session,
            WATCHER_NAME,
            gio::BusNameWatcherFlags::NONE,
            move |connection, _, _| {
                if let Some(notifier) = weak.upgrade() {
                    notifier.register_with_watcher(connection);
                }
            },
            |_, _| {},
        );
    }

    fn register_with_watcher(&self, connection: gio::DBusConnection) {
        glib::MainContext::default().spawn_local(async move {
            let _ = connection
                .call_future(
                    Some(WATCHER_NAME),
                    WATCHER_PATH,
                    WATCHER_INTERFACE,
                    "RegisterStatusNotifierItem",
                    Some(&(ITEM_PATH,).to_variant()),
                    None,
                    gio::DBusCallFlags::NO_AUTO_START,
                    5_000,
                )
                .await;
        });
    }
}

fn item_property(property: &str, presentation: &IndicatorPresentation) -> Option<glib::Variant> {
    match property {
        "Category" => Some("Hardware".to_variant()),
        "Id" => Some("soundtouch-pipewire-control".to_variant()),
        "Title" => Some("SoundTouch".to_variant()),
        "Status" => Some(presentation.state.status().to_variant()),
        "WindowId" => Some(0u32.to_variant()),
        "IconName" => Some(presentation.state.icon_name().to_variant()),
        "IconPixmap" | "OverlayIconPixmap" | "AttentionIconPixmap" => {
            Some(Vec::<(i32, i32, Vec<u8>)>::new().to_variant())
        }
        "OverlayIconName" | "AttentionMovieName" => Some(String::new().to_variant()),
        "AttentionIconName" => Some(presentation.state.attention_icon_name().to_variant()),
        "ToolTip" => Some(
            (
                presentation.state.icon_name(),
                Vec::<(i32, i32, Vec<u8>)>::new(),
                "SoundTouch",
                presentation.tooltip.as_str(),
            )
                .to_variant(),
        ),
        "ItemIsMenu" => Some(false.to_variant()),
        "Menu" => glib::variant::ObjectPath::try_from(MENU_PATH)
            .ok()
            .map(|path| path.to_variant()),
        _ => None,
    }
}

fn menu_property(property: &str) -> Option<glib::Variant> {
    match property {
        "Version" => Some(4u32.to_variant()),
        "TextDirection" => Some("ltr".to_variant()),
        "Status" => Some("normal".to_variant()),
        "IconThemePath" => Some(Vec::<String>::new().to_variant()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn static_menu_layout_has_standard_signature() {
        assert_eq!(
            menu_layout("Open", "About", "Quit").type_().as_str(),
            "(u(ia{sv}av))"
        );
        assert_eq!(menu_property("Version").unwrap().get::<u32>(), Some(4));
    }

    #[test]
    fn menu_actions_are_fixed_and_click_only() {
        assert_eq!(menu_action(1, "clicked"), Some(MenuAction::Open));
        assert_eq!(menu_action(2, "clicked"), Some(MenuAction::About));
        assert_eq!(menu_action(3, "clicked"), Some(MenuAction::Quit));
        assert_eq!(menu_action(4, "clicked"), None);
        assert_eq!(menu_action(1, "hovered"), None);
    }

    #[test]
    fn group_properties_returns_known_static_items() {
        let properties = group_properties(&[0, 1, 3, 99]);
        assert_eq!(properties.len(), 3);
        assert_eq!(properties[0].0, 0);
        assert_eq!(properties[1].0, 1);
        assert_eq!(properties[2].0, 3);
        assert_eq!(properties.to_variant().type_().as_str(), "a(ia{sv})");
    }

    #[test]
    fn item_variant_shapes_match_sni_contract() {
        let presentation = IndicatorPresentation {
            state: IndicatorState::Online,
            tooltip: "tip".into(),
        };
        assert_eq!(
            item_property("IconPixmap", &presentation)
                .unwrap()
                .type_()
                .as_str(),
            "a(iiay)"
        );
        assert_eq!(
            item_property("ToolTip", &presentation)
                .unwrap()
                .type_()
                .as_str(),
            "(sa(iiay)ss)"
        );
        assert_eq!(
            item_property("Menu", &presentation)
                .unwrap()
                .type_()
                .as_str(),
            "o"
        );
    }

    #[test]
    fn icon_properties_distinguish_all_indicator_states() {
        let property = |state, name| {
            item_property(
                name,
                &IndicatorPresentation {
                    state,
                    tooltip: "tip".into(),
                },
            )
            .unwrap()
            .get::<String>()
            .unwrap()
        };

        assert_eq!(property(IndicatorState::Online, "Status"), "Active");
        assert_eq!(
            property(IndicatorState::Online, "IconName"),
            ONLINE_ICON_NAME
        );
        assert_eq!(property(IndicatorState::Online, "AttentionIconName"), "");

        assert_eq!(
            property(IndicatorState::Degraded, "Status"),
            "NeedsAttention"
        );
        assert_eq!(
            property(IndicatorState::Degraded, "IconName"),
            DEGRADED_ICON_NAME
        );
        assert_eq!(
            property(IndicatorState::Degraded, "AttentionIconName"),
            DEGRADED_ICON_NAME
        );

        assert_eq!(
            property(IndicatorState::Unavailable, "IconName"),
            UNAVAILABLE_ICON_NAME
        );
        assert_eq!(
            property(IndicatorState::Unavailable, "AttentionIconName"),
            UNAVAILABLE_ICON_NAME
        );
    }
}
