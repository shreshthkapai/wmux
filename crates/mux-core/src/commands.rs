//! OS-neutral command parsing primitives.
//!
//! This is intentionally small for Phase 2. The important boundary is that
//! command text is parsed before execution, and execution is not mixed into IPC.

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ParsedCommand {
    ServerStatus,
    ListClients,
    NewSession { name: Option<String> },
    AttachSession { target: Option<String> },
    DisplayMessage { message: String },
    KillServer,
    ShowMessages,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandParseError {
    message: String,
}

impl CommandParseError {
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CommandParser;

impl CommandParser {
    pub fn parse(&self, input: &str) -> Result<ParsedCommand, CommandParseError> {
        let input = input.trim();
        if input.is_empty() {
            return Err(CommandParseError::new("empty command"));
        }

        let mut words = input.split_whitespace();
        let first = words.next().expect("input is not empty");
        match first {
            "server" => match words.next() {
                Some("status") if words.next().is_none() => Ok(ParsedCommand::ServerStatus),
                Some("stop") if words.next().is_none() => Ok(ParsedCommand::KillServer),
                Some(other) => Err(CommandParseError::new(format!(
                    "unknown server command: {other}"
                ))),
                None => Ok(ParsedCommand::ServerStatus),
            },
            "list-clients" | "ls-clients" => {
                require_no_args(input, words, ParsedCommand::ListClients)
            }
            "new" | "new-session" => Ok(ParsedCommand::NewSession {
                name: parse_named_arg(input),
            }),
            "attach" | "attach-session" => Ok(ParsedCommand::AttachSession {
                target: parse_named_arg(input),
            }),
            "display-message" | "display" => Ok(ParsedCommand::DisplayMessage {
                message: rest_after_first_word(input).unwrap_or_default().to_string(),
            }),
            "kill-server" => require_no_args(input, words, ParsedCommand::KillServer),
            "show-messages" => require_no_args(input, words, ParsedCommand::ShowMessages),
            other => Err(CommandParseError::new(format!("unknown command: {other}"))),
        }
    }
}

fn require_no_args<'a>(
    input: &str,
    mut remaining: impl Iterator<Item = &'a str>,
    command: ParsedCommand,
) -> Result<ParsedCommand, CommandParseError> {
    if remaining.next().is_none() {
        Ok(command)
    } else {
        Err(CommandParseError::new(format!(
            "unexpected arguments for command: {input}"
        )))
    }
}

fn rest_after_first_word(input: &str) -> Option<&str> {
    let first_len = input.split_whitespace().next()?.len();
    let rest = input[first_len..].trim_start();
    Some(rest)
}

fn parse_named_arg(input: &str) -> Option<String> {
    let words = input.split_whitespace().collect::<Vec<_>>();
    words.windows(2).find_map(|pair| match pair {
        ["-s", value] | ["-t", value] => Some((*value).to_string()),
        _ => None,
    })
}

#[cfg(test)]
mod tests {
    use super::{CommandParser, ParsedCommand};

    #[test]
    fn parses_phase_2_commands() {
        let parser = CommandParser;

        assert_eq!(
            parser.parse("server status"),
            Ok(ParsedCommand::ServerStatus)
        );
        assert_eq!(parser.parse("list-clients"), Ok(ParsedCommand::ListClients));
        assert_eq!(
            parser.parse("new-session -s main"),
            Ok(ParsedCommand::NewSession {
                name: Some("main".to_string())
            })
        );
        assert_eq!(
            parser.parse("attach-session -t main"),
            Ok(ParsedCommand::AttachSession {
                target: Some("main".to_string())
            })
        );
        assert_eq!(
            parser.parse("display-message hello world"),
            Ok(ParsedCommand::DisplayMessage {
                message: "hello world".to_string()
            })
        );
        assert_eq!(parser.parse("kill-server"), Ok(ParsedCommand::KillServer));
        assert_eq!(parser.parse("server stop"), Ok(ParsedCommand::KillServer));
        assert_eq!(
            parser.parse("show-messages"),
            Ok(ParsedCommand::ShowMessages)
        );
    }

    #[test]
    fn rejects_unknown_commands() {
        let parser = CommandParser;

        assert_eq!(
            parser.parse("new-session"),
            Ok(ParsedCommand::NewSession { name: None })
        );
        assert!(parser.parse("list-clients extra").is_err());
    }
}
