use super::{
    lexer::{lex, SourcePosition, SourceSpan, Token, TokenKind},
    parse_single_command, Command, CommandList, CommandParseError,
};

pub const MAX_COMMANDS: usize = 256;
pub const MAX_NESTED_COMMAND_DEPTH: usize = 32;

pub fn parse_command_text(input: &str) -> Result<CommandList, CommandParseError> {
    parse_command_text_at(input, 0)
}

pub fn parse_command_argv(argv: &[String]) -> Result<CommandList, CommandParseError> {
    parse_command_argv_at(argv, 0)
}

fn parse_command_text_at(input: &str, depth: usize) -> Result<CommandList, CommandParseError> {
    check_depth(depth)?;
    parse_tokens(lex(input)?, depth)
}

fn parse_command_argv_at(argv: &[String], depth: usize) -> Result<CommandList, CommandParseError> {
    check_depth(depth)?;
    let mut offset = 0;
    let tokens = argv
        .iter()
        .map(|argument| {
            let start = SourcePosition {
                offset,
                line: 1,
                column: offset + 1,
            };
            offset += argument.len();
            let end = SourcePosition {
                offset,
                line: 1,
                column: offset + 1,
            };
            offset += 1;
            Token {
                kind: TokenKind::Word(argument.clone()),
                span: SourceSpan { start, end },
            }
        })
        .collect();
    parse_tokens(tokens, depth)
}

pub fn parse_command(argv: &[String]) -> Result<Command, CommandParseError> {
    let list = parse_command_argv(argv)?;
    if list.len() != 1 {
        return Err(CommandParseError::new("expected one command"));
    }
    Ok(list[0].clone())
}

pub(super) fn parse_nested_command(
    argv: &[String],
    depth: usize,
) -> Result<CommandList, CommandParseError> {
    let nested = depth + 1;
    if argv.len() == 1 {
        parse_command_text_at(&argv[0], nested)
    } else {
        parse_command_argv_at(argv, nested)
    }
}

pub fn quote_argument(argument: &str) -> String {
    if !argument.is_empty()
        && argument
            .chars()
            .all(|character| !character.is_whitespace() && !";#'\"\\".contains(character))
    {
        return argument.to_string();
    }

    let mut quoted = String::with_capacity(argument.len() + 2);
    quoted.push('"');
    for character in argument.chars() {
        match character {
            '\\' | '"' => {
                quoted.push('\\');
                quoted.push(character);
            }
            '\n' => quoted.push_str("\\n"),
            '\r' => quoted.push_str("\\r"),
            '\t' => quoted.push_str("\\t"),
            character => quoted.push(character),
        }
    }
    quoted.push('"');
    quoted
}

fn check_depth(depth: usize) -> Result<(), CommandParseError> {
    if depth > MAX_NESTED_COMMAND_DEPTH {
        Err(CommandParseError::new(
            "nested command depth exceeds 32 levels",
        ))
    } else {
        Ok(())
    }
}

fn parse_tokens(tokens: Vec<Token>, depth: usize) -> Result<CommandList, CommandParseError> {
    if tokens.is_empty() {
        return Err(CommandParseError::new("empty command"));
    }

    let mut parsed = Vec::new();
    let mut words = Vec::new();
    let mut command_span = None;

    for token in tokens {
        match token.kind {
            TokenKind::Word(word) => {
                command_span.get_or_insert(token.span);
                words.push(word);
            }
            TokenKind::Separator => {
                if words.is_empty() {
                    return Err(CommandParseError::at("empty command", token.span));
                }
                finish_command(&mut parsed, &mut words, &mut command_span, depth)?;
            }
        }
    }
    finish_command(&mut parsed, &mut words, &mut command_span, depth)?;
    CommandList::new(parsed)
}

fn finish_command(
    parsed: &mut Vec<super::Command>,
    words: &mut Vec<String>,
    span: &mut Option<SourceSpan>,
    depth: usize,
) -> Result<(), CommandParseError> {
    if words.is_empty() {
        return Ok(());
    }
    if parsed.len() == MAX_COMMANDS {
        return Err(CommandParseError::at(
            "command list exceeds 256 commands",
            span.take().expect("nonempty command has a span"),
        ));
    }
    let command_span = span.take().expect("nonempty command has a span");
    let command =
        parse_single_command(words, depth).map_err(|error| error.with_span(command_span))?;
    parsed.push(command);
    words.clear();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        parse_command_argv, parse_command_text, quote_argument, MAX_COMMANDS,
        MAX_NESTED_COMMAND_DEPTH,
    };
    use crate::Command;

    #[test]
    fn text_and_argv_share_resolution_without_losing_empty_arguments() {
        let text = parse_command_text("rename-window ''").unwrap();
        let argv = parse_command_argv(&["rename-window".into(), "".into()]).unwrap();
        assert_eq!(text, argv);
        assert_eq!(text.len(), 1);
        assert!(matches!(&text[0], Command::RenameWindow { name } if name.is_empty()));
    }

    #[test]
    fn semicolon_chains_preserve_order_aliases_and_quoted_values() {
        let list =
            parse_command_text("neww -n one; rename-window 'two words'; list-sessions").unwrap();
        assert_eq!(list.len(), 3);
        assert!(matches!(&list[0], Command::NewWindow { name: Some(name) } if name == "one"));
        assert!(matches!(&list[1], Command::RenameWindow { name } if name == "two words"));
        assert!(matches!(&list[2], Command::ListSessions));
    }

    #[test]
    fn invalid_command_makes_the_whole_list_fail() {
        let error =
            parse_command_text("new-window -n valid; no-such-command; list-sessions").unwrap_err();
        assert_eq!(error.message, "unknown command: no-such-command");
        assert_eq!(error.span.unwrap().start.offset, 21);
    }

    #[test]
    fn explicit_empty_command_between_semicolons_is_rejected() {
        let error = parse_command_text("list-sessions;;new-window").unwrap_err();
        assert_eq!(error.message, "empty command");
        assert_eq!(error.span.unwrap().start.offset, 14);
    }

    #[test]
    fn exact_command_list_limit_is_enforced() {
        let accepted = std::iter::repeat_n("list-sessions", MAX_COMMANDS)
            .collect::<Vec<_>>()
            .join(";");
        assert_eq!(parse_command_text(&accepted).unwrap().len(), MAX_COMMANDS);

        let rejected = format!("{accepted};list-sessions");
        assert_eq!(
            parse_command_text(&rejected).unwrap_err().message,
            "command list exceeds 256 commands"
        );
    }

    #[test]
    fn quoted_arguments_roundtrip_and_nested_command_depth_is_bounded() {
        for argument in [
            "",
            "plain",
            "two words",
            "semi;colon",
            "#hash",
            "a\\b\"c",
            "λ",
        ] {
            let parsed =
                parse_command_text(&format!("rename-window {}", quote_argument(argument))).unwrap();
            assert!(matches!(&parsed[0], Command::RenameWindow { name } if name == argument));
        }

        let mut argv = Vec::new();
        for _ in 0..=MAX_NESTED_COMMAND_DEPTH {
            argv.extend(["bind-key".to_string(), "x".to_string()]);
        }
        argv.push("list-sessions".to_string());
        assert_eq!(
            parse_command_argv(&argv).unwrap_err().message,
            "nested command depth exceeds 32 levels"
        );
    }
}
