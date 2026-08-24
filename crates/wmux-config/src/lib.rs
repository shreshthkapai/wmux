use std::{
    collections::BTreeMap,
    env, fs,
    io::{self, ErrorKind},
    path::PathBuf,
};

pub mod theme;

pub use theme::{
    parse_theme_document, ThemeError, ThemePatch, ThemeSources, UiConfig, MAX_THEME_DOCUMENT_BYTES,
    THEME_SCHEMA_VERSION,
};

pub const CONFIG_ENV: &str = "WMUX_CONFIG";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WmuxConfig {
    pub agent_compat: bool,
    pub agent_ui: AgentUi,
    pub pane_env: BTreeMap<String, String>,
    ui: UiConfig,
    command_source: ConfigCommandSource,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ConfigCommandSource {
    text: String,
    first_line: usize,
}

impl ConfigCommandSource {
    pub fn text(&self) -> &str {
        &self.text
    }

    pub const fn first_line(&self) -> usize {
        self.first_line
    }

    pub fn is_empty(&self) -> bool {
        self.text.trim().is_empty()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AgentUi {
    Plain,
    Rich,
    Custom,
}

impl Default for WmuxConfig {
    fn default() -> Self {
        Self {
            agent_compat: true,
            agent_ui: AgentUi::Plain,
            pane_env: agent_env(AgentUi::Plain),
            ui: UiConfig::default(),
            command_source: ConfigCommandSource {
                text: String::new(),
                first_line: 1,
            },
        }
    }
}

impl WmuxConfig {
    pub fn load_or_create() -> io::Result<Self> {
        let path = config_path();
        match fs::read_to_string(&path) {
            Ok(config) => parse_config(&config)
                .map(|mut config| {
                    if let Some(directory) = path.parent() {
                        config.ui.resolve_relative_theme_file(directory);
                    }
                    config
                })
                .map_err(invalid_config),
            Err(error) if error.kind() == ErrorKind::NotFound => {
                if let Some(parent) = path.parent() {
                    fs::create_dir_all(parent)?;
                }
                fs::write(&path, default_config_text())?;
                parse_config(default_config_text())
                    .map(|mut config| {
                        if let Some(directory) = path.parent() {
                            config.ui.resolve_relative_theme_file(directory);
                        }
                        config
                    })
                    .map_err(invalid_config)
            }
            Err(error) => Err(error),
        }
    }

    pub fn pane_environment(&self, pane_id: u64) -> Vec<(String, String)> {
        let mut env = self.pane_env.clone();
        env.insert("WMUX".to_string(), "1".to_string());
        env.insert("WMUX_PANE".to_string(), pane_id.to_string());
        env.into_iter().collect()
    }

    pub fn command_source(&self) -> &ConfigCommandSource {
        &self.command_source
    }

    pub fn ui(&self) -> &UiConfig {
        &self.ui
    }
}

pub fn config_path() -> PathBuf {
    if let Some(path) = env::var_os(CONFIG_ENV) {
        return PathBuf::from(path);
    }
    if let Some(appdata) = env::var_os("APPDATA") {
        return PathBuf::from(appdata).join("wmux").join("config.wmux");
    }
    if let Some(user_profile) = env::var_os("USERPROFILE") {
        return PathBuf::from(user_profile)
            .join(".config")
            .join("wmux")
            .join("config.wmux");
    }
    PathBuf::from("config.wmux")
}

pub fn default_config_text() -> &'static str {
    r#"# wmux config
# This file is read by the wmux server when it starts.
#
# agent_compat provides predictable terminal capabilities and color settings
# for terminal coding agents running inside wmux.
agent_compat = true

# agent_ui:
#   plain  - conservative 256-color profile without rich color or box UI
#   rich   - 256-color profile with truecolor enabled
#   custom - only use pane.env.* entries below
agent_ui = plain

pane.env.TERM = xterm-256color
pane.env.TERM_PROGRAM = wmux
pane.env.TERM_PROGRAM_VERSION = wmux
pane.env.NO_COLOR = 1
pane.env.CLICOLOR = 0
pane.env.CLICOLOR_FORCE = 0

# UI defaults inherit the terminal foreground/background and do not animate.
# ui.theme = default
# ui.theme_file = ""
# ui.theme_provider = ""
# ui.border.style = single
# ui.border.foreground = default
# ui.border.background = default
# ui.active_border.style = heavy
# ui.active_border.foreground = default
# ui.active_border.background = default
# ui.status.style = reverse
# ui.status.foreground = default
# ui.status.background = default
# ui.animation = off
# ui.animation_target = both
# ui.animation_fps = 12
# ui.animation_playback = loop
"#
}

pub fn parse_config(input: &str) -> Result<WmuxConfig, String> {
    let mut agent_compat = None;
    let mut agent_ui = None;
    let mut explicit_env = BTreeMap::new();
    let mut ui = UiConfig::default();
    let mut command_text = String::with_capacity(input.len());

    for (line_index, raw_line) in input.lines().enumerate() {
        let line = strip_comment(raw_line).trim();
        if line.is_empty() {
            command_text.push('\n');
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            command_text.push_str(raw_line);
            command_text.push('\n');
            continue;
        };
        let key = key.trim();
        if key.chars().any(char::is_whitespace) {
            command_text.push_str(raw_line);
            command_text.push('\n');
            continue;
        }
        let value = unquote(value.trim()).to_string();
        if key == "agent_compat" {
            agent_compat = Some(parse_bool(&value).ok_or_else(|| {
                format!(
                    "line {}: agent_compat must be true or false",
                    line_index + 1
                )
            })?);
        } else if key == "agent_ui" {
            agent_ui = Some(parse_agent_ui(&value).ok_or_else(|| {
                format!(
                    "line {}: agent_ui must be plain, rich, or custom",
                    line_index + 1
                )
            })?);
        } else if let Some(name) = key.strip_prefix("pane.env.") {
            let name = name.trim();
            if name.is_empty() || name.contains('=') || name.contains('\0') {
                return Err(format!("line {}: invalid environment name", line_index + 1));
            }
            explicit_env.insert(name.to_string(), value);
        } else if key.starts_with("ui.") {
            ui.apply_config_option(key, &value)
                .map_err(|error| format!("line {}: {error}", line_index + 1))?;
        } else {
            return Err(format!("line {}: unknown option {key}", line_index + 1));
        }
        command_text.push('\n');
    }

    let agent_compat = agent_compat.unwrap_or(true);
    let agent_ui = agent_ui.unwrap_or(AgentUi::Plain);
    let mut pane_env = BTreeMap::new();
    pane_env.extend(explicit_env);
    if agent_compat && agent_ui != AgentUi::Custom {
        pane_env
            .retain(|key, value| !(key.ends_with("MUX") && key != "WMUX" && value == "wmux,0,0"));
        apply_agent_env(&mut pane_env, agent_ui);
    }

    Ok(WmuxConfig {
        agent_compat,
        agent_ui,
        pane_env,
        ui,
        command_source: ConfigCommandSource {
            text: command_text,
            first_line: 1,
        },
    })
}

fn agent_env(agent_ui: AgentUi) -> BTreeMap<String, String> {
    match agent_ui {
        AgentUi::Plain => BTreeMap::from([
            ("TERM".to_string(), "xterm-256color".to_string()),
            ("TERM_PROGRAM".to_string(), "wmux".to_string()),
            ("TERM_PROGRAM_VERSION".to_string(), "wmux".to_string()),
            ("COLORTERM".to_string(), String::new()),
            ("NO_COLOR".to_string(), "1".to_string()),
            ("FORCE_COLOR".to_string(), "0".to_string()),
            ("CLICOLOR".to_string(), "0".to_string()),
            ("CLICOLOR_FORCE".to_string(), "0".to_string()),
        ]),
        AgentUi::Rich => BTreeMap::from([
            ("TERM".to_string(), "xterm-256color".to_string()),
            ("TERM_PROGRAM".to_string(), "wmux".to_string()),
            ("TERM_PROGRAM_VERSION".to_string(), "wmux".to_string()),
            ("COLORTERM".to_string(), "truecolor".to_string()),
        ]),
        AgentUi::Custom => BTreeMap::new(),
    }
}

fn apply_agent_env(pane_env: &mut BTreeMap<String, String>, agent_ui: AgentUi) {
    for key in [
        "TERM",
        "TERM_PROGRAM",
        "TERM_PROGRAM_VERSION",
        "COLORTERM",
        "NO_COLOR",
        "FORCE_COLOR",
        "CLICOLOR",
        "CLICOLOR_FORCE",
    ] {
        pane_env.remove(key);
    }
    pane_env.extend(agent_env(agent_ui));
}

fn strip_comment(line: &str) -> &str {
    let mut quoted = false;
    for (index, ch) in line.char_indices() {
        match ch {
            '"' => quoted = !quoted,
            '#' if !quoted => return &line[..index],
            _ => {}
        }
    }
    line
}

fn unquote(value: &str) -> &str {
    value
        .strip_prefix('"')
        .and_then(|value| value.strip_suffix('"'))
        .unwrap_or(value)
}

fn parse_bool(value: &str) -> Option<bool> {
    match value {
        "true" | "on" | "yes" | "1" => Some(true),
        "false" | "off" | "no" | "0" => Some(false),
        _ => None,
    }
}

fn parse_agent_ui(value: &str) -> Option<AgentUi> {
    match value {
        "plain" | "minimal" | "ascii" => Some(AgentUi::Plain),
        "rich" | "truecolor" => Some(AgentUi::Rich),
        "custom" | "none" => Some(AgentUi::Custom),
        _ => None,
    }
}

fn invalid_config(error: String) -> io::Error {
    io::Error::new(ErrorKind::InvalidData, error)
}

#[cfg(test)]
mod tests {
    use super::{parse_config, AgentUi, WmuxConfig};

    #[test]
    fn defaults_identify_wmux_without_legacy_aliases() {
        let config = WmuxConfig::default();

        assert_eq!(
            config.pane_env.get("TERM"),
            Some(&"xterm-256color".to_string())
        );
        assert_eq!(config.agent_ui, AgentUi::Plain);
        assert_eq!(
            config.pane_env.get("TERM_PROGRAM"),
            Some(&"wmux".to_string())
        );
        assert_eq!(config.pane_env.get("NO_COLOR"), Some(&"1".to_string()));
        assert_eq!(
            config
                .pane_env
                .keys()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "CLICOLOR",
                "CLICOLOR_FORCE",
                "COLORTERM",
                "FORCE_COLOR",
                "NO_COLOR",
                "TERM",
                "TERM_PROGRAM",
                "TERM_PROGRAM_VERSION",
            ]
        );
    }

    #[test]
    fn parser_can_disable_agent_compat_defaults() {
        let config = parse_config(
            r#"
            agent_compat = false
            pane.env.FOO = bar
            "#,
        )
        .expect("config parses");

        assert!(!config.agent_compat);
        assert_eq!(config.pane_env.get("FOO"), Some(&"bar".to_string()));
        assert_eq!(config.pane_env.get("TERM"), None);
    }

    #[test]
    fn absent_agent_ui_overrides_old_generated_env_to_plain_profile() {
        let config = parse_config(
            r#"
            agent_compat = true
            pane.env.TERM = legacy-256color
            pane.env.COLORTERM = truecolor
            "#,
        )
        .expect("config parses");

        assert_eq!(config.agent_ui, AgentUi::Plain);
        assert_eq!(
            config.pane_env.get("TERM"),
            Some(&"xterm-256color".to_string())
        );
        assert_eq!(config.pane_env.get("COLORTERM"), Some(&String::new()));
        assert_eq!(config.pane_env.get("NO_COLOR"), Some(&"1".to_string()));
    }

    #[test]
    fn generated_legacy_multiplexer_alias_is_removed() {
        let config = parse_config(
            r#"
            agent_compat = true
            pane.env.OLDMUX = wmux,0,0
            "#,
        )
        .expect("config parses");

        assert!(!config.pane_env.contains_key("OLDMUX"));
    }

    #[test]
    fn rich_agent_ui_uses_wmux_identity_and_truecolor() {
        let config = parse_config("agent_ui = rich").expect("config parses");

        assert_eq!(config.agent_ui, AgentUi::Rich);
        assert_eq!(
            config.pane_env.get("TERM"),
            Some(&"xterm-256color".to_string())
        );
        assert_eq!(
            config.pane_env.get("TERM_PROGRAM"),
            Some(&"wmux".to_string())
        );
        assert_eq!(
            config.pane_env.get("COLORTERM"),
            Some(&"truecolor".to_string())
        );
    }

    #[test]
    fn pane_environment_adds_per_pane_identity() {
        let env = WmuxConfig::default().pane_environment(7);

        assert!(env.iter().any(|(key, value)| key == "WMUX" && value == "1"));
        assert!(env
            .iter()
            .any(|(key, value)| key == "WMUX_PANE" && value == "7"));
        assert_eq!(
            env.iter().map(|(key, _)| key.as_str()).collect::<Vec<_>>(),
            vec![
                "CLICOLOR",
                "CLICOLOR_FORCE",
                "COLORTERM",
                "FORCE_COLOR",
                "NO_COLOR",
                "TERM",
                "TERM_PROGRAM",
                "TERM_PROGRAM_VERSION",
                "WMUX",
                "WMUX_PANE",
            ]
        );
    }

    #[test]
    fn functional_lines_are_retained_with_original_source_lines() {
        let config = parse_config(
            "agent_compat = true\n\
             set-option -g buffer-limit 60\n\
             # a comment\n\
             bind-key q display-message '#{session_name}'\n",
        )
        .unwrap();

        assert_eq!(
            config.command_source().text(),
            "\nset-option -g buffer-limit 60\n\nbind-key q display-message '#{session_name}'\n"
        );
        assert_eq!(config.command_source().first_line(), 1);
    }

    #[test]
    fn invalid_bootstrap_assignment_is_not_misclassified_as_a_command() {
        let error = parse_config("agent_compat = maybe\n").unwrap_err();
        assert_eq!(error, "line 1: agent_compat must be true or false");
    }
}
