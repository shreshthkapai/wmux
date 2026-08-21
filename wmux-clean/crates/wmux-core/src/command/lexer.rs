use std::{fmt, iter::Peekable, str::CharIndices};

pub const MAX_COMMAND_BYTES: usize = 1024 * 1024;
pub const MAX_TOKENS: usize = 4_096;
pub const MAX_TOKEN_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourcePosition {
    pub offset: usize,
    pub line: usize,
    pub column: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourceSpan {
    pub start: SourcePosition,
    pub end: SourcePosition,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandParseError {
    pub message: String,
    pub span: Option<SourceSpan>,
}

impl CommandParseError {
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
            span: None,
        }
    }

    pub(crate) fn at(message: impl Into<String>, span: SourceSpan) -> Self {
        Self {
            message: message.into(),
            span: Some(span),
        }
    }

    pub(crate) fn with_span(mut self, span: SourceSpan) -> Self {
        if self.span.is_none() {
            self.span = Some(span);
        }
        self
    }
}

impl fmt::Display for CommandParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)?;
        if let Some(span) = self.span {
            write!(
                formatter,
                " at line {}, column {}",
                span.start.line, span.start.column
            )?;
        }
        Ok(())
    }
}

impl std::error::Error for CommandParseError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TokenKind {
    Word(String),
    Separator,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Token {
    pub kind: TokenKind,
    pub span: SourceSpan,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Quote {
    Single,
    Double,
}

struct Cursor<'a> {
    chars: Peekable<CharIndices<'a>>,
    offset: usize,
    line: usize,
    column: usize,
    len: usize,
}

impl<'a> Cursor<'a> {
    fn new(input: &'a str) -> Self {
        Self {
            chars: input.char_indices().peekable(),
            offset: 0,
            line: 1,
            column: 1,
            len: input.len(),
        }
    }

    fn position(&self) -> SourcePosition {
        SourcePosition {
            offset: self.offset,
            line: self.line,
            column: self.column,
        }
    }

    fn peek(&mut self) -> Option<char> {
        self.chars.peek().map(|(_, ch)| *ch)
    }

    fn next(&mut self) -> Option<(SourcePosition, char)> {
        let (offset, ch) = self.chars.next()?;
        let position = SourcePosition {
            offset,
            line: self.line,
            column: self.column,
        };
        self.offset = offset + ch.len_utf8();
        if ch == '\n' {
            self.line += 1;
            self.column = 1;
        } else {
            self.column += 1;
        }
        Some((position, ch))
    }

    fn end_position(&self) -> SourcePosition {
        debug_assert_eq!(self.offset, self.len);
        self.position()
    }
}

pub fn lex(input: &str) -> Result<Vec<Token>, CommandParseError> {
    if input.len() > MAX_COMMAND_BYTES {
        return Err(CommandParseError::new(
            "command input exceeds 1048576 bytes",
        ));
    }

    let mut cursor = Cursor::new(input);
    let mut tokens = Vec::new();
    let mut token = String::new();
    let mut token_start = None;
    let mut token_started = false;
    let mut quote = None;
    let mut quote_start = None;

    while let Some((position, mut ch)) = cursor.next() {
        if quote.is_none() && ch == '\r' {
            if cursor.peek() == Some('\n') {
                let _ = cursor.next();
            }
            ch = '\n';
        }

        if quote.is_none() && matches!(ch, ' ' | '\t') {
            finish_word(
                &mut tokens,
                &mut token,
                &mut token_start,
                &mut token_started,
                position,
            )?;
            continue;
        }

        if quote.is_none() && matches!(ch, '\n' | ';') {
            finish_word(
                &mut tokens,
                &mut token,
                &mut token_start,
                &mut token_started,
                position,
            )?;
            push_separator(&mut tokens, position, cursor.position(), ch == ';')?;
            continue;
        }

        if quote.is_none() && !token_started && ch == '#' {
            while let Some((comment_position, comment_char)) = cursor.next() {
                if comment_char == '\n' {
                    push_separator(&mut tokens, comment_position, cursor.position(), false)?;
                    break;
                }
            }
            continue;
        }

        if ch == '\'' && quote != Some(Quote::Double) {
            match quote {
                Some(Quote::Single) => {
                    quote = None;
                    quote_start = None;
                }
                None => {
                    quote = Some(Quote::Single);
                    quote_start = Some(position);
                    token_start.get_or_insert(position);
                    token_started = true;
                }
                Some(Quote::Double) => unreachable!(),
            }
            continue;
        }

        if ch == '"' && quote != Some(Quote::Single) {
            match quote {
                Some(Quote::Double) => {
                    quote = None;
                    quote_start = None;
                }
                None => {
                    quote = Some(Quote::Double);
                    quote_start = Some(position);
                    token_start.get_or_insert(position);
                    token_started = true;
                }
                Some(Quote::Single) => unreachable!(),
            }
            continue;
        }

        token_start.get_or_insert(position);
        token_started = true;
        if ch == '\\' && quote != Some(Quote::Single) {
            append_escape(&mut token, &mut cursor, position)?;
        } else {
            append_char(&mut token, ch, position, cursor.position())?;
        }
    }

    let end = cursor.end_position();
    if let Some(open) = quote_start {
        let message = match quote {
            Some(Quote::Single) => "unterminated single quote",
            Some(Quote::Double) => "unterminated double quote",
            None => unreachable!(),
        };
        return Err(CommandParseError::at(
            message,
            SourceSpan { start: open, end },
        ));
    }
    finish_word(
        &mut tokens,
        &mut token,
        &mut token_start,
        &mut token_started,
        end,
    )?;
    while matches!(
        tokens.last().map(|token| &token.kind),
        Some(TokenKind::Separator)
    ) {
        tokens.pop();
    }
    Ok(tokens)
}

fn finish_word(
    tokens: &mut Vec<Token>,
    token: &mut String,
    token_start: &mut Option<SourcePosition>,
    token_started: &mut bool,
    end: SourcePosition,
) -> Result<(), CommandParseError> {
    if !*token_started {
        return Ok(());
    }
    let start = token_start.take().expect("started token has a position");
    push_token(
        tokens,
        Token {
            kind: TokenKind::Word(std::mem::take(token)),
            span: SourceSpan { start, end },
        },
    )?;
    *token_started = false;
    Ok(())
}

fn push_separator(
    tokens: &mut Vec<Token>,
    start: SourcePosition,
    end: SourcePosition,
    explicit: bool,
) -> Result<(), CommandParseError> {
    if !explicit
        && (tokens.is_empty()
            || matches!(
                tokens.last().map(|token| &token.kind),
                Some(TokenKind::Separator)
            ))
    {
        return Ok(());
    }
    push_token(
        tokens,
        Token {
            kind: TokenKind::Separator,
            span: SourceSpan { start, end },
        },
    )
}

fn push_token(tokens: &mut Vec<Token>, token: Token) -> Result<(), CommandParseError> {
    if tokens.len() == MAX_TOKENS {
        return Err(CommandParseError::at(
            "command input exceeds 4096 tokens",
            token.span,
        ));
    }
    tokens.push(token);
    Ok(())
}

fn append_char(
    token: &mut String,
    ch: char,
    start: SourcePosition,
    end: SourcePosition,
) -> Result<(), CommandParseError> {
    if token.len() + ch.len_utf8() > MAX_TOKEN_BYTES {
        return Err(CommandParseError::at(
            "command token exceeds 65536 bytes",
            SourceSpan { start, end },
        ));
    }
    token.push(ch);
    Ok(())
}

fn append_escape(
    token: &mut String,
    cursor: &mut Cursor<'_>,
    slash: SourcePosition,
) -> Result<(), CommandParseError> {
    let Some((position, escaped)) = cursor.next() else {
        return Err(CommandParseError::at(
            "dangling escape",
            SourceSpan {
                start: slash,
                end: cursor.position(),
            },
        ));
    };
    let decoded = match escaped {
        'a' => '\u{07}',
        'b' => '\u{08}',
        'e' => '\u{1b}',
        'f' => '\u{0c}',
        'n' => '\n',
        'r' => '\r',
        's' => ' ',
        't' => '\t',
        'v' => '\u{0b}',
        'u' => return append_unicode_escape(token, cursor, slash, 4),
        'U' => return append_unicode_escape(token, cursor, slash, 8),
        other => other,
    };
    append_char(token, decoded, position, cursor.position())
}

fn append_unicode_escape(
    token: &mut String,
    cursor: &mut Cursor<'_>,
    slash: SourcePosition,
    digits: usize,
) -> Result<(), CommandParseError> {
    let mut value = 0_u32;
    for _ in 0..digits {
        let Some((_, ch)) = cursor.next() else {
            return Err(invalid_unicode_escape(slash, cursor.position()));
        };
        let Some(digit) = ch.to_digit(16) else {
            return Err(invalid_unicode_escape(slash, cursor.position()));
        };
        value = (value << 4) | digit;
    }
    let Some(ch) = char::from_u32(value) else {
        return Err(invalid_unicode_escape(slash, cursor.position()));
    };
    append_char(token, ch, slash, cursor.position())
}

fn invalid_unicode_escape(start: SourcePosition, end: SourcePosition) -> CommandParseError {
    CommandParseError::at("invalid Unicode escape", SourceSpan { start, end })
}

#[cfg(test)]
mod tests {
    use super::{lex, TokenKind, MAX_COMMAND_BYTES, MAX_TOKENS, MAX_TOKEN_BYTES};

    fn values(input: &str) -> Vec<String> {
        lex(input)
            .unwrap()
            .into_iter()
            .map(|token| match token.kind {
                TokenKind::Word(value) => value,
                TokenKind::Separator => ";".to_string(),
            })
            .collect()
    }

    #[test]
    fn quotes_empty_arguments_comments_and_chains_are_tokenized() {
        assert_eq!(
            values("rename-window 'two words'; new-window \"\" # ignored\nlist-sessions foo#bar",),
            [
                "rename-window",
                "two words",
                ";",
                "new-window",
                "",
                ";",
                "list-sessions",
                "foo#bar",
            ]
        );
    }

    #[test]
    fn escapes_decode_to_literal_utf8() {
        assert_eq!(
            values(r"rename-window \e\n\s\t\u03bb\U0001F642"),
            ["rename-window", "\u{1b}\n \t\u{03bb}\u{1f642}"]
        );
    }

    #[test]
    fn syntax_error_span_starts_at_the_unclosed_quote() {
        let error = lex("new-window -n 'broken").unwrap_err();
        assert_eq!(error.message, "unterminated single quote");
        let span = error.span.unwrap();
        assert_eq!(span.start.offset, 14);
        assert_eq!(span.start.line, 1);
        assert_eq!(span.start.column, 15);
    }

    #[test]
    fn exact_lexer_limits_are_enforced() {
        let too_large = "x".repeat(MAX_COMMAND_BYTES + 1);
        assert_eq!(
            lex(&too_large).unwrap_err().message,
            "command input exceeds 1048576 bytes"
        );

        let too_many = std::iter::repeat_n("x", MAX_TOKENS + 1)
            .collect::<Vec<_>>()
            .join(" ");
        assert_eq!(
            lex(&too_many).unwrap_err().message,
            "command input exceeds 4096 tokens"
        );

        let token_too_large = "x".repeat(MAX_TOKEN_BYTES + 1);
        assert_eq!(
            lex(&token_too_large).unwrap_err().message,
            "command token exceeds 65536 bytes"
        );
    }
}
