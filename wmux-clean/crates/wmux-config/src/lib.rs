use std::{
    collections::BTreeMap,
    env, fs,
    io::{self, ErrorKind},
    path::PathBuf,
};

pub const CONFIG_ENV: &str = "WMUX_CONFIG";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WmuxConfig {
    pub agent_compat: bool,
    pub agent_ui: AgentUi,
    pub pane_env: BTreeMap<String, String>,
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
        }
    }
}

impl WmuxConfig {
    pub fn load_or_create() -> io::Result<Self> {
        let path = config_path();
        match fs::read_to_string(&path) {
            Ok(config) => parse_config(&config).map_err(invalid_config),
            Err(error) if error.kind() == ErrorKind::NotFound => {
                if let Some(parent) = path.parent() {
                    fs::create_dir_all(parent)?;
                }
                fs::write(&path, default_config_text())?;
                parse_config(default_config_text()).map_err(invalid_config)
            }
            Err(error) => Err(error),
        }
    }

    pub fn pane_environment(&self, pane_id: u64) -> Vec<(String, String)> {
        let mut env = self.pane_env.clone();
        env.insert("WMUX".to_string(), "1".to_string());
        env.insert("WMUX_PANE".to_string(), pane_id.to_string());
        if self.agent_compat {
            env.entry("TMUX_PANE".to_string())
                .or_insert_with(|| format!("%{pane_id}"));
        }
        env.into_iter().collect()
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
# agent_compat mirrors the tmux child-process environment enough for terminal
# coding agents to detect that they are running inside a multiplexer.
agent_compat = true

# agent_ui:
#   plain  - screen/tmux compatible, asks CLIs to avoid rich color/box UI
#   rich   - tmux-256color + truecolor
#   custom - only use pane.env.* entries below
agent_ui = plain

pane.env.TERM = screen-256color
pane.env.TERM_PROGRAM = tmux
pane.env.TERM_PROGRAM_VERSION = wmux
pane.env.TMUX = wmux,0,0
pane.env.NO_COLOR = 1
pane.env.CLICOLOR = 0
pane.env.CLICOLOR_FORCE = 0
"#
}

pub fn parse_config(input: &str) -> Result<WmuxConfig, String> {
    let mut agent_compat = None;
    let mut agent_ui = None;
    let mut explicit_env = BTreeMap::new();

    for (line_index, raw_line) in input.lines().enumerate() {
        let line = strip_comment(raw_line).trim();
        if line.is_empty() {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            return Err(format!("line {}: expected key = value", line_index + 1));
        };
        let key = key.trim();
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
        } else {
            return Err(format!("line {}: unknown option {key}", line_index + 1));
        }
    }

    let agent_compat = agent_compat.unwrap_or(true);
    let agent_ui = agent_ui.unwrap_or(AgentUi::Plain);
    let mut pane_env = BTreeMap::new();
    pane_env.extend(explicit_env);
    if agent_compat && agent_ui != AgentUi::Custom {
        apply_agent_env(&mut pane_env, agent_ui);
    }

    Ok(WmuxConfig {
        agent_compat,
        agent_ui,
        pane_env,
    })
}

fn agent_env(agent_ui: AgentUi) -> BTreeMap<String, String> {
    match agent_ui {
        AgentUi::Plain => BTreeMap::from([
            ("TERM".to_string(), "screen-256color".to_string()),
            ("TERM_PROGRAM".to_string(), "tmux".to_string()),
            ("TERM_PROGRAM_VERSION".to_string(), "wmux".to_string()),
            ("TMUX".to_string(), "wmux,0,0".to_string()),
            ("COLORTERM".to_string(), String::new()),
            ("NO_COLOR".to_string(), "1".to_string()),
            ("FORCE_COLOR".to_string(), "0".to_string()),
            ("CLICOLOR".to_string(), "0".to_string()),
            ("CLICOLOR_FORCE".to_string(), "0".to_string()),
        ]),
        AgentUi::Rich => BTreeMap::from([
            ("TERM".to_string(), "tmux-256color".to_string()),
            ("TERM_PROGRAM".to_string(), "tmux".to_string()),
            ("TERM_PROGRAM_VERSION".to_string(), "wmux".to_string()),
            ("COLORTERM".to_string(), "truecolor".to_string()),
            ("TMUX".to_string(), "wmux,0,0".to_string()),
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
        "TMUX",
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
    fn defaults_enable_plain_tmux_style_agent_compatibility() {
        let config = WmuxConfig::default();

        assert_eq!(
            config.pane_env.get("TERM"),
            Some(&"screen-256color".to_string())
        );
        assert_eq!(config.agent_ui, AgentUi::Plain);
        assert_eq!(
            config.pane_env.get("TERM_PROGRAM"),
            Some(&"tmux".to_string())
        );
        assert_eq!(config.pane_env.get("NO_COLOR"), Some(&"1".to_string()));
        assert_eq!(config.pane_env.get("TMUX"), Some(&"wmux,0,0".to_string()));
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
            pane.env.TERM = tmux-256color
            pane.env.COLORTERM = truecolor
            "#,
        )
        .expect("config parses");

        assert_eq!(config.agent_ui, AgentUi::Plain);
        assert_eq!(
            config.pane_env.get("TERM"),
            Some(&"screen-256color".to_string())
        );
        assert_eq!(config.pane_env.get("COLORTERM"), Some(&String::new()));
        assert_eq!(config.pane_env.get("NO_COLOR"), Some(&"1".to_string()));
    }

    #[test]
    fn rich_agent_ui_uses_tmux_256color_and_truecolor() {
        let config = parse_config("agent_ui = rich").expect("config parses");

        assert_eq!(config.agent_ui, AgentUi::Rich);
        assert_eq!(
            config.pane_env.get("TERM"),
            Some(&"tmux-256color".to_string())
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
            .any(|(key, value)| key == "TMUX_PANE" && value == "%7"));
    }
}
