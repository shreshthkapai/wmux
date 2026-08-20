//! Shared command-line parsing surface.

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CliCommand {
    ServerStart,
    ServerStatus,
    ServerStop,
    ResetTerminal,
    ListClients,
    NewSession { name: Option<String> },
    AttachSession { target: Option<String> },
    Raw(Vec<String>),
}

pub fn parse_args(args: impl IntoIterator<Item = String>) -> CliCommand {
    let mut args = args.into_iter();
    let Some(first) = args.next() else {
        return CliCommand::ServerStatus;
    };

    match first.as_str() {
        "server" => match args.next().as_deref() {
            Some("start") => CliCommand::ServerStart,
            Some("stop") => CliCommand::ServerStop,
            Some("status") | None => CliCommand::ServerStatus,
            _ => CliCommand::ServerStatus,
        },
        "list-clients" | "ls-clients" => CliCommand::ListClients,
        "reset-terminal" | "reset-tty" => CliCommand::ResetTerminal,
        "new" | "new-session" => CliCommand::NewSession {
            name: parse_target_name(args.collect()),
        },
        "attach" | "attach-session" => CliCommand::AttachSession {
            target: parse_target_name(args.collect()),
        },
        _ => {
            let mut raw = vec![first];
            raw.extend(args);
            CliCommand::Raw(raw)
        }
    }
}

fn parse_target_name(args: Vec<String>) -> Option<String> {
    args.windows(2)
        .find(|pair| pair[0] == "-s" || pair[0] == "-t")
        .map(|pair| pair[1].clone())
}

#[cfg(test)]
mod tests {
    use super::{parse_args, CliCommand};

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_string()).collect()
    }

    #[test]
    fn parses_phase_3_user_flows() {
        assert_eq!(
            parse_args(args(&["new-session", "-s", "main"])),
            CliCommand::NewSession {
                name: Some("main".to_string())
            }
        );
        assert_eq!(
            parse_args(args(&["attach-session", "-t", "main"])),
            CliCommand::AttachSession {
                target: Some("main".to_string())
            }
        );
        assert_eq!(
            parse_args(args(&["attach"])),
            CliCommand::AttachSession { target: None }
        );
    }

    #[test]
    fn parses_server_lifecycle_commands() {
        assert_eq!(
            parse_args(args(&["server", "start"])),
            CliCommand::ServerStart
        );
        assert_eq!(
            parse_args(args(&["server", "status"])),
            CliCommand::ServerStatus
        );
        assert_eq!(
            parse_args(args(&["server", "stop"])),
            CliCommand::ServerStop
        );
        assert_eq!(
            parse_args(args(&["reset-terminal"])),
            CliCommand::ResetTerminal
        );
    }
}
