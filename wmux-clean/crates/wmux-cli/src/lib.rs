use std::{error::Error, fmt};

use wmux_core::resolve_command_name;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StartupPolicy {
    StartIfMissing,
    RequireExisting,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConfigAction {
    Path,
    Show,
    Effective,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServerInvocation {
    pub argv: Vec<String>,
    pub attached: bool,
    pub startup: StartupPolicy,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Invocation {
    Help,
    Version,
    Config(ConfigAction),
    Server(ServerInvocation),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CliError(String);

impl fmt::Display for CliError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for CliError {}

pub const HELP: &str = "Usage: wmux [--help|-h] [--version|-V] [command [arguments]]\n\
\n\
Commands:\n\
  bind-key|bind [-nr] [-T table] key command [arguments]\n\
  confirm-before|confirm [-p prompt] command [arguments]\n\
  config path|show|effective\n\
  list-keys|lsk [-T table]\n\
  refresh-client\n\
  send-keys|send [-l] [-N repeat] [-t target-pane] key [key ...]\n\
  send-prefix [-t target-pane]\n\
  switch-client [-lnp | -t target-session]\n\
  unbind-key|unbind [-an] [-T table] [key]\n";

pub fn version_line() -> String {
    format!("wmux {}", env!("CARGO_PKG_VERSION"))
}

pub fn parse(args: &[String]) -> Result<Invocation, CliError> {
    match args {
        [] => {
            return Ok(Invocation::Server(ServerInvocation {
                argv: vec!["new-session".to_string()],
                attached: true,
                startup: StartupPolicy::StartIfMissing,
            }))
        }
        [argument] if matches!(argument.as_str(), "--help" | "-h") => return Ok(Invocation::Help),
        [argument] if matches!(argument.as_str(), "--version" | "-V") => {
            return Ok(Invocation::Version)
        }
        [config, command] if config == "config" => {
            return match command.as_str() {
                "path" => Ok(Invocation::Config(ConfigAction::Path)),
                "show" => Ok(Invocation::Config(ConfigAction::Show)),
                "effective" => Ok(Invocation::Config(ConfigAction::Effective)),
                _ => parse_server(args),
            }
        }
        [argument, ..] if argument.starts_with('-') => {
            return Err(CliError(format!("unknown option: {argument}")))
        }
        _ => {}
    }

    parse_server(args)
}

fn parse_server(args: &[String]) -> Result<Invocation, CliError> {
    let canonical = resolve_command_name(&args[0]).map_err(|error| CliError(error.to_string()))?;
    let mut argv = args.to_vec();
    argv[0] = canonical.to_string();

    Ok(Invocation::Server(ServerInvocation {
        attached: matches!(canonical, "attach-session")
            || (canonical == "new-session" && !args.iter().any(|argument| argument == "-d")),
        startup: if matches!(canonical, "new-session" | "attach-session" | "start-server") {
            StartupPolicy::StartIfMissing
        } else {
            StartupPolicy::RequireExisting
        },
        argv,
    }))
}

#[cfg(test)]
mod tests {
    use super::{parse, ConfigAction, Invocation, ServerInvocation, StartupPolicy};

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_string()).collect()
    }

    fn server(values: &[&str]) -> ServerInvocation {
        match parse(&args(values)).unwrap() {
            Invocation::Server(invocation) => invocation,
            other => panic!("expected server invocation, got {other:?}"),
        }
    }

    #[test]
    fn bare_wmux_starts_an_attached_new_session() {
        assert_eq!(
            parse(&args(&[])).unwrap(),
            Invocation::Server(ServerInvocation {
                argv: args(&["new-session"]),
                attached: true,
                startup: StartupPolicy::StartIfMissing,
            })
        );
    }

    #[test]
    fn help_and_version_are_local_invocations() {
        assert_eq!(parse(&args(&["--help"])).unwrap(), Invocation::Help);
        assert_eq!(parse(&args(&["-V"])).unwrap(), Invocation::Version);
    }

    #[test]
    fn startup_policy_is_conservative() {
        assert_eq!(
            server(&["new-session", "-d"]).startup,
            StartupPolicy::StartIfMissing
        );
        assert_eq!(
            server(&["attach-session"]).startup,
            StartupPolicy::StartIfMissing
        );
        assert_eq!(
            server(&["list-sessions"]).startup,
            StartupPolicy::RequireExisting
        );
        assert_eq!(
            server(&["kill-server"]).startup,
            StartupPolicy::RequireExisting
        );
    }

    #[test]
    fn unknown_top_level_options_are_rejected_before_command_resolution() {
        assert!(parse(&args(&["--unknown-option"]))
            .unwrap_err()
            .to_string()
            .contains("unknown option"));
    }

    #[test]
    fn config_subcommands_remain_local() {
        assert_eq!(
            parse(&args(&["config", "path"])).unwrap(),
            Invocation::Config(ConfigAction::Path)
        );
        assert_eq!(
            parse(&args(&["config", "show"])).unwrap(),
            Invocation::Config(ConfigAction::Show)
        );
        assert_eq!(
            parse(&args(&["config", "effective"])).unwrap(),
            Invocation::Config(ConfigAction::Effective)
        );
    }

    #[test]
    fn aliases_and_unique_prefixes_are_canonicalized_before_dispatch() {
        let new_session = server(&["new", "-s", "work"]);
        assert_eq!(new_session.argv, args(&["new-session", "-s", "work"]));
        assert!(new_session.attached);

        let attach_session = server(&["a", "-t", "work"]);
        assert_eq!(attach_session.argv, args(&["attach-session", "-t", "work"]));
        assert!(attach_session.attached);

        let list_sessions = server(&["ls"]);
        assert_eq!(list_sessions.argv, args(&["list-sessions"]));
        assert!(!list_sessions.attached);

        let bind = server(&["bind", "q", "list-sessions"]);
        assert_eq!(bind.argv, args(&["bind-key", "q", "list-sessions"]));

        let list_keys = server(&["lsk"]);
        assert_eq!(list_keys.argv, args(&["list-keys"]));

        let unbind = server(&["unbind", "q"]);
        assert_eq!(unbind.argv, args(&["unbind-key", "q"]));

        let send = server(&["send", "Enter"]);
        assert_eq!(send.argv, args(&["send-keys", "Enter"]));

        let confirm = server(&["confirm", "kill-pane"]);
        assert_eq!(confirm.argv, args(&["confirm-before", "kill-pane"]));

        let switch = server(&["switchc", "-n"]);
        assert_eq!(switch.argv, args(&["switch-client", "-n"]));
    }
}
