use std::fmt;

use crate::{ClientId, OptionTarget, PaneId, ServerState, SessionId, WindowId};

pub const MAX_FORMAT_BYTES: usize = 1024 * 1024;
pub const MAX_FORMAT_DEPTH: usize = 32;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct FormatContext {
    pub client: Option<ClientId>,
    pub session: Option<SessionId>,
    pub window: Option<WindowId>,
    pub pane: Option<PaneId>,
}

impl FormatContext {
    pub const fn for_client(client: ClientId) -> Self {
        Self {
            client: Some(client),
            session: None,
            window: None,
            pane: None,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FormatError {
    message: String,
    offset: Option<usize>,
}

impl FormatError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
            offset: None,
        }
    }

    fn at(message: impl Into<String>, offset: usize) -> Self {
        Self {
            message: message.into(),
            offset: Some(offset),
        }
    }
}

impl fmt::Display for FormatError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)?;
        if let Some(offset) = self.offset {
            write!(formatter, " at offset {offset}")?;
        }
        Ok(())
    }
}

impl std::error::Error for FormatError {}

pub struct FormatEngine;

impl FormatEngine {
    pub fn expand(
        state: &ServerState,
        context: FormatContext,
        input: &str,
    ) -> Result<String, FormatError> {
        if input.len() > MAX_FORMAT_BYTES {
            return Err(FormatError::new("format input exceeds 1048576 bytes"));
        }
        let context = complete_context(state, context);
        let mut output = String::with_capacity(input.len().min(MAX_FORMAT_BYTES));
        expand_into(state, context, input, 0, 0, &mut output)?;
        Ok(output)
    }
}

fn complete_context(state: &ServerState, mut context: FormatContext) -> FormatContext {
    if let Some(client) = context.client {
        if let Some((session, window, pane)) = state.active_window_and_pane_for_client(client) {
            context.session.get_or_insert(session);
            context.window.get_or_insert(window);
            context.pane.get_or_insert(pane);
        } else if context.session.is_none() {
            context.session = state
                .clients
                .get(&client)
                .and_then(|client| client.attached_session);
        }
    }
    if let Some(pane) = context.pane {
        context.window = context
            .window
            .or_else(|| state.panes.get(&pane).map(|pane| pane.window));
    }
    if context.session.is_none() {
        context.session = context.window.and_then(|window| {
            state
                .winlinks
                .values()
                .find(|winlink| winlink.window == window)
                .map(|winlink| winlink.session)
        });
    }
    context
}

fn expand_into(
    state: &ServerState,
    context: FormatContext,
    input: &str,
    depth: usize,
    base_offset: usize,
    output: &mut String,
) -> Result<(), FormatError> {
    if depth > MAX_FORMAT_DEPTH {
        return Err(FormatError::new("format recursion exceeds 32 levels"));
    }
    let bytes = input.as_bytes();
    let mut cursor = 0;
    while cursor < bytes.len() {
        if bytes[cursor] != b'#' {
            let next = input[cursor..]
                .find('#')
                .map_or(bytes.len(), |relative| cursor + relative);
            append_bounded(output, &input[cursor..next])?;
            cursor = next;
            continue;
        }
        if bytes.get(cursor + 1) == Some(&b'#') {
            append_bounded(output, "#")?;
            cursor += 2;
            continue;
        }
        if bytes.get(cursor + 1) != Some(&b'{') {
            append_bounded(output, "#")?;
            cursor += 1;
            continue;
        }
        let end = find_expression_end(input, cursor).ok_or_else(|| {
            FormatError::at("unterminated format expression", base_offset + cursor)
        })?;
        let expression = &input[cursor + 2..end];
        expand_expression(
            state,
            context,
            expression,
            depth + 1,
            base_offset + cursor + 2,
            output,
        )?;
        cursor = end + 1;
    }
    Ok(())
}

fn find_expression_end(input: &str, start: usize) -> Option<usize> {
    let bytes = input.as_bytes();
    let mut depth = 1_usize;
    let mut cursor = start + 2;
    while cursor < bytes.len() {
        if bytes[cursor] == b'#' && bytes.get(cursor + 1) == Some(&b'{') {
            depth += 1;
            cursor += 2;
            continue;
        }
        if bytes[cursor] == b'}' {
            depth -= 1;
            if depth == 0 {
                return Some(cursor);
            }
        }
        cursor += 1;
    }
    None
}

fn expand_expression(
    state: &ServerState,
    context: FormatContext,
    expression: &str,
    depth: usize,
    base_offset: usize,
    output: &mut String,
) -> Result<(), FormatError> {
    if let Some(conditional) = expression.strip_prefix('?') {
        let branches = split_conditional(conditional).ok_or_else(|| {
            FormatError::at(
                "conditional format requires condition, true branch, and false branch",
                base_offset,
            )
        })?;
        let condition = if branches[0].contains("#{") {
            let mut expanded = String::new();
            expand_into(
                state,
                context,
                branches[0],
                depth,
                base_offset + 1,
                &mut expanded,
            )?;
            expanded
        } else {
            lookup(state, context, branches[0].trim())
        };
        let selected = if truthy(&condition) {
            branches[1]
        } else {
            branches[2]
        };
        return expand_into(state, context, selected, depth, base_offset, output);
    }
    append_bounded(output, &lookup(state, context, expression.trim()))
}

fn split_conditional(input: &str) -> Option<[&str; 3]> {
    let bytes = input.as_bytes();
    let mut depth = 0_usize;
    let mut splits = Vec::with_capacity(2);
    let mut cursor = 0;
    while cursor < bytes.len() {
        if bytes[cursor] == b'#' && bytes.get(cursor + 1) == Some(&b'{') {
            depth += 1;
            cursor += 2;
            continue;
        }
        if bytes[cursor] == b'}' && depth > 0 {
            depth -= 1;
        } else if bytes[cursor] == b',' && depth == 0 && splits.len() < 2 {
            splits.push(cursor);
        }
        cursor += 1;
    }
    match splits.as_slice() {
        [first, second] => Some([
            &input[..*first],
            &input[*first + 1..*second],
            &input[*second + 1..],
        ]),
        _ => None,
    }
}

fn lookup(state: &ServerState, context: FormatContext, name: &str) -> String {
    match name {
        "client_id" => context.client.map(|id| id.raw().to_string()),
        "session_id" => context.session.map(|id| format!("${}", id.raw())),
        "session_name" => context
            .session
            .and_then(|id| state.sessions.get(&id))
            .map(|session| session.name.clone()),
        "window_id" => context.window.map(|id| format!("@{}", id.raw())),
        "window_name" => context
            .window
            .and_then(|id| state.windows.get(&id))
            .map(|window| window.name.clone()),
        "window_index" => context.window.and_then(|window| {
            state
                .winlinks
                .values()
                .find(|winlink| {
                    winlink.window == window
                        && context
                            .session
                            .is_none_or(|session| winlink.session == session)
                })
                .map(|winlink| winlink.index.to_string())
        }),
        "pane_id" => context.pane.map(|id| format!("%{}", id.raw())),
        "pane_width" => context
            .pane
            .and_then(|id| state.panes.get(&id))
            .map(|pane| pane.rect.cols.to_string()),
        "pane_height" => context
            .pane
            .and_then(|id| state.panes.get(&id))
            .map(|pane| pane.rect.rows.to_string()),
        _ => option_value(state, context, name),
    }
    .unwrap_or_default()
}

fn option_value(state: &ServerState, context: FormatContext, name: &str) -> Option<String> {
    let target = context
        .pane
        .map(OptionTarget::Pane)
        .or_else(|| context.window.map(OptionTarget::Window))
        .or_else(|| context.session.map(OptionTarget::Session))
        .or_else(|| context.client.map(OptionTarget::Client))
        .unwrap_or(OptionTarget::Server);
    state
        .option(target, name)
        .ok()
        .map(|value| value.to_string())
}

fn truthy(value: &str) -> bool {
    !value.is_empty() && !matches!(value, "0" | "off" | "false")
}

fn append_bounded(output: &mut String, text: &str) -> Result<(), FormatError> {
    if output
        .len()
        .checked_add(text.len())
        .is_none_or(|size| size > MAX_FORMAT_BYTES)
    {
        return Err(FormatError::new("format output exceeds 1048576 bytes"));
    }
    output.push_str(text);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{FormatContext, FormatEngine, MAX_FORMAT_BYTES, MAX_FORMAT_DEPTH};
    use crate::{OptionTarget, ServerState};

    fn state_and_context() -> (ServerState, FormatContext) {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 120, 40);
        state.attach_client(client, created.session).unwrap();
        state
            .options
            .set(OptionTarget::Window(created.window), "@branch", "main")
            .unwrap();
        (state, FormatContext::for_client(client))
    }

    #[test]
    fn expands_stable_target_values_options_literals_and_conditionals() {
        let (state, context) = state_and_context();
        let output = FormatEngine::expand(
            &state,
            context,
            "## #{session_name} #{window_name} #{pane_width}x#{pane_height} #{@branch} #{?session_name,yes,no} #{missing}",
        )
        .unwrap();

        assert_eq!(output, "# work 0 120x40 main yes ");
    }

    #[test]
    fn nested_conditionals_expand_only_the_selected_branch() {
        let (state, context) = state_and_context();
        let output = FormatEngine::expand(
            &state,
            context,
            "#{?@branch,#{?missing,bad,#{window_name}},also-bad}",
        )
        .unwrap();

        assert_eq!(output, "0");
    }

    #[test]
    fn malformed_input_reports_the_source_offset() {
        let (state, context) = state_and_context();
        let error = FormatEngine::expand(&state, context, "before #{session_name")
            .unwrap_err()
            .to_string();

        assert!(error.contains("offset 7"), "{error}");
    }

    #[test]
    fn recursion_input_and_output_are_bounded() {
        let (mut state, context) = state_and_context();
        let mut nested = "ok".to_string();
        for _ in 0..=MAX_FORMAT_DEPTH {
            nested = format!("#{{?session_name,{nested},no}}");
        }
        assert!(FormatEngine::expand(&state, context, &nested)
            .unwrap_err()
            .to_string()
            .contains("32 levels"));
        assert!(
            FormatEngine::expand(&state, context, &"x".repeat(MAX_FORMAT_BYTES + 1))
                .unwrap_err()
                .to_string()
                .contains("input exceeds")
        );
        state
            .options
            .set(
                OptionTarget::Server,
                "@huge",
                &"x".repeat(crate::MAX_OPTION_STRING_BYTES),
            )
            .unwrap();
        let expansion = "#{@huge}".repeat(17);
        assert!(FormatEngine::expand(&state, context, &expansion)
            .unwrap_err()
            .to_string()
            .contains("output exceeds"));
    }
}
