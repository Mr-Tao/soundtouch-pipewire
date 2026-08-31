use gettextrs::{LocaleCategory, bindtextdomain, gettext, setlocale, textdomain};

const GETTEXT_PACKAGE: &str = "soundtouch-pipewire-control";
const DEFAULT_LOCALE_DIR: &str = "/usr/share/locale";

pub fn init() {
    setlocale(LocaleCategory::LcAll, "");

    let locale_dir = option_env!("SOUNDTOUCH_PIPEWIRE_LOCALEDIR").unwrap_or(DEFAULT_LOCALE_DIR);
    if let Err(error) = bindtextdomain(GETTEXT_PACKAGE, locale_dir) {
        eprintln!("failed to bind gettext domain: {error}");
    }
    if let Err(error) = textdomain(GETTEXT_PACKAGE) {
        eprintln!("failed to select gettext domain: {error}");
    }
}

pub fn tr(message: &str) -> String {
    gettext(message)
}
