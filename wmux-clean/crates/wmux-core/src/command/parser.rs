use super::{
    lexer::{lex, SourcePosition, SourceSpan, Token, TokenKind},
    parse_single_command, Command, CommandList, CommandParseError,
};

pub const MAX_COMMANDS: usize = 256;

pub fn parse_command_text(input: &str) -> Result<CommandList, CommandParseError> {
    parse_tokens(lex(input)?)
}

pub fn parse_command_argv(argv: &[String]) -> Result<CommandList, CommandParseError> {
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
    parse_tokens(tokens)
}

pub fn parse_command(argv: &[String]) -> Result<Command, CommandParseError> {
    let list = parse_command_argv(argv)?;
    if list.len() != 1 {
        return Err(CommandParseError::new("expected one command"));
    }
    Ok(list[0].clone())
}

fn parse_tokens(tokens: Vec<Token>) -> Result<CommandList, CommandParseError> {
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
                finish_command(&mut parsed, &mut words, &mut command_span)?;
            }
        }
    }
    finish_command(&mut parsed, &mut words, &mut command_span)?;
    CommandList::new(parsed)
}

fn finish_command(
    parsed: &mut Vec<super::Command>,
    words: &mut Vec<String>,
    span: &mut Option<SourceSpan>,
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
    let command = parse_single_command(words).map_err(|error| error.with_span(command_span))?;
    parsed.push(command);
    words.clear();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{parse_command_argv, parse_command_text, MAX_COMMANDS};
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
}
