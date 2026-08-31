use std::ffi::OsString;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum MixerPreference {
    #[default]
    Automatic,
    Pwvucontrol,
    Pavucontrol,
    Wiremix,
}

impl MixerPreference {
    pub const ALL: [Self; 4] = [
        Self::Automatic,
        Self::Pwvucontrol,
        Self::Pavucontrol,
        Self::Wiremix,
    ];

    pub fn id(self) -> &'static str {
        match self {
            Self::Automatic => "auto",
            Self::Pwvucontrol => "pwvucontrol",
            Self::Pavucontrol => "pavucontrol",
            Self::Wiremix => "wiremix",
        }
    }

    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|choice| choice.id() == id)
    }

    pub fn index(self) -> u32 {
        Self::ALL
            .iter()
            .position(|choice| *choice == self)
            .unwrap_or(0) as u32
    }

    pub fn from_index(index: u32) -> Self {
        Self::ALL.get(index as usize).copied().unwrap_or_default()
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MixerCommand {
    pub program: PathBuf,
    pub args: Vec<OsString>,
}

impl MixerCommand {
    pub fn launch(&self) -> Result<(), glib::Error> {
        let mut argv = Vec::with_capacity(self.args.len() + 1);
        argv.push(self.program.as_os_str());
        argv.extend(self.args.iter().map(OsString::as_os_str));
        gio::Subprocess::newv(&argv, gio::SubprocessFlags::NONE)?;
        Ok(())
    }
}

pub fn resolve(preference: MixerPreference) -> Option<MixerCommand> {
    resolve_with(preference, |program| glib::find_program_in_path(program))
}

fn resolve_with(
    preference: MixerPreference,
    find: impl Fn(&str) -> Option<PathBuf>,
) -> Option<MixerCommand> {
    let graphical = |program: &str| {
        find(program).map(|path| MixerCommand {
            program: path,
            args: Vec::new(),
        })
    };

    match preference {
        MixerPreference::Automatic => graphical("pwvucontrol")
            .or_else(|| graphical("pavucontrol"))
            .or_else(|| wiremix_command(&find)),
        MixerPreference::Pwvucontrol => graphical("pwvucontrol"),
        MixerPreference::Pavucontrol => graphical("pavucontrol"),
        MixerPreference::Wiremix => wiremix_command(&find),
    }
}

fn wiremix_command(find: &impl Fn(&str) -> Option<PathBuf>) -> Option<MixerCommand> {
    let wiremix = find("wiremix")?;

    for terminal in [
        "xfce4-terminal",
        "kgx",
        "gnome-terminal",
        "konsole",
        "xterm",
    ] {
        let Some(program) = find(terminal) else {
            continue;
        };

        let args = match terminal {
            "xfce4-terminal" => vec![
                OsString::from("--disable-server"),
                OsString::from("--command"),
                wiremix.clone().into_os_string(),
            ],
            "kgx" | "gnome-terminal" => {
                vec![OsString::from("--"), wiremix.clone().into_os_string()]
            }
            "konsole" | "xterm" => {
                vec![OsString::from("-e"), wiremix.clone().into_os_string()]
            }
            _ => unreachable!(),
        };

        return Some(MixerCommand { program, args });
    }

    None
}

pub fn preference_path() -> PathBuf {
    glib::user_config_dir()
        .join("soundtouch-pipewire")
        .join("control.conf")
}

pub fn load_preference(path: &Path) -> MixerPreference {
    fs::read_to_string(path)
        .ok()
        .and_then(|contents| parse_preference(&contents))
        .unwrap_or_default()
}

pub fn save_preference(path: &Path, preference: MixerPreference) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }

    let temporary = path.with_extension("conf.tmp");
    let mut options = OpenOptions::new();
    options.create(true).truncate(true).write(true);
    #[cfg(unix)]
    options.mode(0o600);

    let mut file = options.open(&temporary)?;
    writeln!(file, "mixer={}", preference.id())?;
    file.sync_all()?;
    drop(file);
    fs::rename(temporary, path)
}

fn parse_preference(contents: &str) -> Option<MixerPreference> {
    contents
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .find_map(|line| line.strip_prefix("mixer="))
        .and_then(MixerPreference::from_id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsStr;

    fn fake_finder<'a>(available: &'a [&'a str]) -> impl Fn(&str) -> Option<PathBuf> + 'a {
        move |program| {
            available
                .contains(&program)
                .then(|| PathBuf::from(format!("/usr/bin/{program}")))
        }
    }

    #[test]
    fn parses_known_preference_and_rejects_unknown_value() {
        assert_eq!(
            parse_preference("# generated\nmixer=wiremix\n"),
            Some(MixerPreference::Wiremix)
        );
        assert_eq!(parse_preference("mixer=unknown\n"), None);
    }

    #[test]
    fn automatic_prefers_native_pipewire_mixer() {
        let command = resolve_with(
            MixerPreference::Automatic,
            fake_finder(&["pavucontrol", "pwvucontrol"]),
        )
        .expect("a mixer should resolve");
        assert_eq!(command.program, Path::new("/usr/bin/pwvucontrol"));
    }

    #[test]
    fn wiremix_is_wrapped_in_an_available_terminal() {
        let command = resolve_with(
            MixerPreference::Wiremix,
            fake_finder(&["wiremix", "xfce4-terminal"]),
        )
        .expect("wiremix with a terminal should resolve");

        assert_eq!(command.program, Path::new("/usr/bin/xfce4-terminal"));
        assert_eq!(
            command.args,
            [
                OsStr::new("--disable-server"),
                OsStr::new("--command"),
                OsStr::new("/usr/bin/wiremix")
            ]
        );
    }

    #[test]
    fn explicit_missing_mixer_does_not_silently_fall_back() {
        assert_eq!(
            resolve_with(MixerPreference::Pwvucontrol, fake_finder(&["pavucontrol"])),
            None
        );
    }
}
