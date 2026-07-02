#include "daemon_command_engine.hpp"

#include "wmux/command_mode.hpp"
#include "wmux/commands.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace wmux::daemon_internal {
namespace {

constexpr std::size_t kMaxCommandModeBytes = 4096;

struct ResolveContext {
  const DaemonState& state;
  std::optional<ClientId> current_client_id;
};

bool pane_exists_in_window(const WindowSummary& window, PaneId pane_id) {
  return std::any_of(window.panes.begin(), window.panes.end(), [&](const auto& pane) {
    return pane.id == pane_id;
  });
}

std::optional<std::size_t> parse_index(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  for (const char character : value) {
    if (!std::isdigit(static_cast<unsigned char>(character))) {
      return std::nullopt;
    }
  }

  std::size_t parsed{};
  const auto* first = value.data();
  const auto* last = value.data() + value.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return std::nullopt;
  }

  return parsed;
}

std::optional<SplitDirection> parse_runtime_split_direction(std::string_view direction) {
  if (direction == "horizontal") {
    return SplitDirection::Horizontal;
  }
  if (direction == "vertical") {
    return SplitDirection::Vertical;
  }
  return std::nullopt;
}

std::vector<std::string_view> command_args_as_views(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.push_back(arg);
  }
  return views;
}

bool reject_extra_command_mode_args(
    const std::vector<std::string>& args,
    std::string_view command_name,
    std::string& error) {
  if (args.size() == 1) {
    return false;
  }

  error = "wmux: ";
  error += command_name;
  error += " does not accept arguments yet";
  return true;
}

Target target_from_optional_session_name(const std::string& session_name) {
  if (session_name.empty()) {
    return Target::current();
  }

  return Target::named(session_name);
}

std::optional<SessionSummary> find_session_by_id(const SessionManager& sessions, SessionId id) {
  const auto listed = sessions.list_sessions();
  const auto found = std::find_if(listed.begin(), listed.end(), [&](const auto& session) {
    return session.id == id;
  });
  if (found == listed.end()) {
    return std::nullopt;
  }

  return *found;
}

std::optional<WindowSummary> find_window_by_id(
    const SessionSummary& session,
    WindowId window_id) {
  const auto found = std::find_if(session.windows.begin(), session.windows.end(), [&](const auto& window) {
    return window.id == window_id;
  });
  if (found == session.windows.end()) {
    return std::nullopt;
  }

  return *found;
}

TargetResolutionResult user_error(std::string message) {
  TargetResolutionResult result;
  result.error = std::move(message);
  return result;
}

TargetResolutionResult resolved(
    SessionId session_id,
    WindowId window_id,
    std::optional<PaneId> pane_id,
    std::optional<ClientId> client_id) {
  TargetResolutionResult result;
  result.ok = true;
  result.target.session_id = session_id;
  result.target.window_id = window_id;
  result.target.pane_id = pane_id;
  result.target.client_id = client_id;
  return result;
}

TargetResolutionResult resolve_session_summary(
    const SessionSummary& session,
    std::optional<ClientId> client_id) {
  if (session.active_window_id == 0) {
    return user_error("wmux: session has no active window");
  }

  const auto window = find_window_by_id(session, session.active_window_id);
  if (!window) {
    return user_error("wmux: active window no longer exists");
  }

  return resolved(session.id, window->id, window->active_pane_id, client_id);
}

TargetResolutionResult resolve_session_id(
    const ResolveContext& context,
    SessionId session_id,
    std::optional<ClientId> client_id) {
  const auto session = find_session_by_id(context.state.sessions, session_id);
  if (!session) {
    return user_error("wmux: target session not found");
  }

  return resolve_session_summary(*session, client_id);
}

TargetResolutionResult resolve_window_id(
    const ResolveContext& context,
    WindowId window_id,
    std::optional<ClientId> client_id) {
  for (const auto& session : context.state.sessions.list_sessions()) {
    if (const auto window = find_window_by_id(session, window_id)) {
      return resolved(session.id, window->id, window->active_pane_id, client_id);
    }
  }

  return user_error("wmux: target window not found");
}

TargetResolutionResult resolve_pane_id(
    const ResolveContext& context,
    PaneId pane_id,
    std::optional<ClientId> client_id) {
  for (const auto& session : context.state.sessions.list_sessions()) {
    for (const auto& window : session.windows) {
      if (pane_exists_in_window(window, pane_id)) {
        return resolved(session.id, window.id, pane_id, client_id);
      }
    }
  }

  return user_error("wmux: target pane not found");
}

TargetResolutionResult resolve_attached_client(
    const ResolveContext& context,
    ClientId client_id) {
  const auto client = context.state.attach_clients.find(client_id);
  if (client == context.state.attach_clients.end()) {
    return user_error("wmux: target client not found");
  }

  const auto attached_session = client->second.client.attached_session;
  if (!attached_session) {
    return user_error("wmux: target client is not attached to a session");
  }

  const auto session = find_session_by_id(context.state.sessions, *attached_session);
  if (!session) {
    return user_error("wmux: attached session no longer exists");
  }

  const auto window_id = client->second.client.active_window.value_or(session->active_window_id);
  if (window_id == 0) {
    return user_error("wmux: target client has no active window");
  }

  const auto window = find_window_by_id(*session, window_id);
  if (!window) {
    return user_error("wmux: active window no longer exists");
  }

  const auto pane_id = client->second.client.active_pane.value_or(window->active_pane_id);
  if (pane_id != 0 && !pane_exists_in_window(*window, pane_id)) {
    return user_error("wmux: active pane no longer exists");
  }

  return resolved(session->id, window->id, pane_id == 0 ? std::nullopt : std::optional{pane_id}, client_id);
}

TargetResolutionResult resolve_current(const ResolveContext& context) {
  if (!context.current_client_id) {
    return user_error("wmux: current target requires an attached client");
  }

  return resolve_attached_client(context, *context.current_client_id);
}

TargetResolutionResult resolve_mouse_position(
    const ResolveContext& context,
    const Target& target) {
  const auto client_id = target.client_id != 0
                             ? std::optional<ClientId>{target.client_id}
                             : context.current_client_id;
  if (!client_id) {
    return user_error("wmux: mouse target requires an attached client");
  }
  if (target.x == 0 || target.y == 0) {
    return user_error("wmux: mouse target is outside the pane area");
  }

  const auto current = resolve_attached_client(context, *client_id);
  if (!current.ok) {
    return current;
  }

  const auto client = context.state.attach_clients.find(*client_id);
  if (client == context.state.attach_clients.end()) {
    return user_error("wmux: target client not found");
  }

  const int columns = client->second.client.size.columns > 0
                          ? static_cast<int>(client->second.client.size.columns)
                          : 120;
  const int rows = client->second.client.size.rows > 0
                       ? static_cast<int>(client->second.client.size.rows)
                       : 30;
  const int pane_rows =
      std::max(1, rows - (context.state.config.values.status_bar_enabled && rows > 1 ? 1 : 0));
  const int column = static_cast<int>(target.x) - 1;
  const int row = static_cast<int>(target.y) - 1;
  if (column < 0 || column >= columns || row < 0 || row >= pane_rows) {
    return user_error("wmux: mouse target is outside the pane area");
  }

  const auto session = find_session_by_id(context.state.sessions, current.target.session_id);
  if (!session) {
    return user_error("wmux: attached session no longer exists");
  }
  const auto window = find_window_by_id(*session, current.target.window_id);
  if (!window) {
    return user_error("wmux: active window no longer exists");
  }

  const auto rects = compute_pane_layout_rects(window->pane_tree, columns, pane_rows);
  const auto hit = std::find_if(rects.begin(), rects.end(), [&](const auto& rect) {
    return column >= rect.left &&
           column < rect.left + rect.width &&
           row >= rect.top &&
           row < rect.top + rect.height;
  });
  if (hit == rects.end()) {
    return user_error("wmux: mouse target is outside the pane area");
  }

  return resolved(session->id, window->id, hit->pane_id, *client_id);
}

TargetResolutionResult resolve_named(
    const ResolveContext& context,
    std::string_view name) {
  if (name.empty()) {
    return user_error("wmux: target name cannot be empty");
  }

  if (const auto session_id = context.state.sessions.session_id_for_name(name)) {
    return resolve_session_id(context, *session_id, context.current_client_id);
  }

  std::vector<ResolvedTarget> window_matches;
  const auto requested_index = parse_index(name);
  const auto current = context.current_client_id ? resolve_current(context) : TargetResolutionResult{};
  const std::optional<SessionId> preferred_session_id =
      current.ok ? std::optional{current.target.session_id} : std::nullopt;

  for (const auto& session : context.state.sessions.list_sessions()) {
    for (const auto& window : session.windows) {
      const bool name_matches = window.name == name;
      const bool index_matches = requested_index && window.index == *requested_index;
      if (!name_matches && !index_matches) {
        continue;
      }

      window_matches.push_back(
          ResolvedTarget{session.id, window.id, window.active_pane_id, context.current_client_id});
    }
  }

  if (preferred_session_id) {
    const auto preferred = std::find_if(
        window_matches.begin(),
        window_matches.end(),
        [&](const auto& candidate) { return candidate.session_id == *preferred_session_id; });
    if (preferred != window_matches.end()) {
      TargetResolutionResult result;
      result.ok = true;
      result.target = *preferred;
      return result;
    }
  }

  if (window_matches.size() == 1) {
    TargetResolutionResult result;
    result.ok = true;
    result.target = window_matches.front();
    return result;
  }
  if (window_matches.size() > 1) {
    return user_error("wmux: target name is ambiguous");
  }

  if (const auto pane_index = parse_index(name)) {
    for (const auto& session : context.state.sessions.list_sessions()) {
      for (const auto& window : session.windows) {
        if (*pane_index < window.panes.size()) {
          const auto pane_id = window.panes[*pane_index].id;
          return resolved(session.id, window.id, pane_id, context.current_client_id);
        }
      }
    }
  }

  return user_error("wmux: target not found");
}

TargetResolutionResult resolve_target_locked(
    const DaemonState& state,
    const Target& target,
    std::optional<ClientId> current_client_id) {
  const ResolveContext context{state, current_client_id};
  switch (target.kind) {
    case TargetKind::Current:
      return resolve_current(context);
    case TargetKind::Session:
      return resolve_session_id(context, target.session_id, current_client_id);
    case TargetKind::Window:
      return resolve_window_id(context, target.window_id, current_client_id);
    case TargetKind::Pane:
      return resolve_pane_id(context, target.pane_id, current_client_id);
    case TargetKind::Client:
      return resolve_attached_client(context, target.client_id);
    case TargetKind::MousePosition:
      return resolve_mouse_position(context, target);
    case TargetKind::Named:
      return resolve_named(context, target.name);
  }

  return user_error("wmux: unknown target kind");
}

std::optional<RuntimeCommand> runtime_command_from_command_line(
    const CommandLine& command,
    bool command_mode,
    std::string& error) {
  RuntimeCommand runtime;
  runtime.target = target_from_optional_session_name(command.session_name);

  if (command_mode && !command.session_name.empty()) {
    error = "wmux: command mode operates on the attached session; omit -t <session>";
    return std::nullopt;
  }

  switch (command.kind) {
    case CommandKind::NewWindow:
      runtime.kind = RuntimeCommandKind::NewWindow;
      runtime.name = command.window_name;
      return runtime;

    case CommandKind::RenameWindow:
      runtime.kind = RuntimeCommandKind::RenameWindow;
      runtime.name = command.window_name;
      return runtime;

    case CommandKind::SplitWindow: {
      const auto direction = parse_runtime_split_direction(command.split_direction);
      if (!direction) {
        error = "wmux: split-window requires one of -h or -v";
        return std::nullopt;
      }
      runtime.kind = RuntimeCommandKind::SplitPane;
      runtime.axis = *direction;
      return runtime;
    }

    case CommandKind::SetOption:
      runtime.kind = RuntimeCommandKind::SetOption;
      runtime.option_scope = OptionScope::Global;
      runtime.key = command.option_name;
      runtime.value = command.option_value;
      return runtime;

    case CommandKind::BindKey:
      runtime.kind = RuntimeCommandKind::BindKey;
      runtime.option_scope = OptionScope::Global;
      runtime.key = command.key_name;
      runtime.value = command.key_action;
      return runtime;

    case CommandKind::UnbindKey:
      runtime.kind = RuntimeCommandKind::UnbindKey;
      runtime.option_scope = OptionScope::Global;
      runtime.key = command.key_name;
      return runtime;

    default:
      break;
  }

  error = command_mode ? "wmux: command is not supported in command mode yet"
                       : "wmux: command is not supported by the runtime command engine";
  return std::nullopt;
}

}  // namespace

TargetResolutionResult resolve_target(
    DaemonState& state,
    const Target& target,
    std::optional<ClientId> current_client_id) {
  std::lock_guard lock(state.mutex);
  return resolve_target_locked(state, target, current_client_id);
}

std::optional<RuntimeCommand> runtime_command_from_attach_command(
    std::string_view command,
    std::string& error) {
  RuntimeCommand runtime;
  runtime.target = Target::current();

  if (command == "new-window") {
    runtime.kind = RuntimeCommandKind::NewWindow;
    return runtime;
  }
  if (command == "next-window") {
    runtime.kind = RuntimeCommandKind::NextWindow;
    return runtime;
  }
  if (command == "previous-window") {
    runtime.kind = RuntimeCommandKind::PreviousWindow;
    return runtime;
  }
  if (command == "split-horizontal") {
    runtime.kind = RuntimeCommandKind::SplitPane;
    runtime.axis = SplitDirection::Horizontal;
    return runtime;
  }
  if (command == "split-vertical") {
    runtime.kind = RuntimeCommandKind::SplitPane;
    runtime.axis = SplitDirection::Vertical;
    return runtime;
  }
  if (command == "select-pane-left") {
    runtime.kind = RuntimeCommandKind::SelectPane;
    runtime.pane_direction = PaneDirection::Left;
    return runtime;
  }
  if (command == "select-pane-right") {
    runtime.kind = RuntimeCommandKind::SelectPane;
    runtime.pane_direction = PaneDirection::Right;
    return runtime;
  }
  if (command == "select-pane-up") {
    runtime.kind = RuntimeCommandKind::SelectPane;
    runtime.pane_direction = PaneDirection::Up;
    return runtime;
  }
  if (command == "select-pane-down") {
    runtime.kind = RuntimeCommandKind::SelectPane;
    runtime.pane_direction = PaneDirection::Down;
    return runtime;
  }
  if (command == "kill-pane") {
    runtime.kind = RuntimeCommandKind::KillPane;
    return runtime;
  }
  if (command == "equalize-panes") {
    runtime.kind = RuntimeCommandKind::SpreadPanesEvenly;
    return runtime;
  }

  error = "wmux: unknown attach command";
  return std::nullopt;
}

std::optional<RuntimeCommand> runtime_command_from_command_mode_text(
    std::string_view command_text,
    std::string& error) {
  if (command_text.size() > kMaxCommandModeBytes) {
    error = "wmux: command is too long";
    return std::nullopt;
  }

  const auto parsed_text = parse_command_prompt_text(command_text);
  if (!parsed_text.ok) {
    error = parsed_text.error;
    return std::nullopt;
  }

  if (parsed_text.args.empty()) {
    error = "wmux: empty command";
    return std::nullopt;
  }

  RuntimeCommand runtime;
  runtime.target = Target::current();

  const auto command_name = std::string_view{parsed_text.args[0]};
  if (command_name == "rename-session") {
    if (parsed_text.args.size() != 2 || parsed_text.args[1].empty()) {
      error = "wmux: rename-session requires <new>";
      return std::nullopt;
    }
    runtime.kind = RuntimeCommandKind::RenameSession;
    runtime.name = parsed_text.args[1];
    return runtime;
  }

  if (command_name == "kill-pane") {
    if (reject_extra_command_mode_args(parsed_text.args, command_name, error)) {
      return std::nullopt;
    }
    runtime.kind = RuntimeCommandKind::KillPane;
    return runtime;
  }

  if (command_name == "kill-window") {
    if (reject_extra_command_mode_args(parsed_text.args, command_name, error)) {
      return std::nullopt;
    }
    runtime.kind = RuntimeCommandKind::KillWindow;
    return runtime;
  }

  if (command_name == "equalize-panes") {
    if (reject_extra_command_mode_args(parsed_text.args, command_name, error)) {
      return std::nullopt;
    }
    runtime.kind = RuntimeCommandKind::SpreadPanesEvenly;
    return runtime;
  }

  if (command_name == "select-layout") {
    if (parsed_text.args.size() == 2 && parsed_text.args[1] == "-E") {
      runtime.kind = RuntimeCommandKind::SpreadPanesEvenly;
      return runtime;
    }
    error = "wmux: select-layout supports only -E in command mode";
    return std::nullopt;
  }

  const auto args = command_args_as_views(parsed_text.args);
  const auto command = parse_command_line(args);
  if (command.kind == CommandKind::Unknown) {
    error = "wmux: " + command.error;
    return std::nullopt;
  }

  return runtime_command_from_command_line(command, true, error);
}

std::optional<RuntimeCommand> runtime_command_from_ipc_request(
    const IpcRequest& request,
    std::string& error) {
  RuntimeCommand runtime;

  if (request.type == "NewSession") {
    runtime.kind = RuntimeCommandKind::NewSession;
    runtime.name = request.session_name;
    return runtime;
  }

  if (request.type == "RenameSession") {
    runtime.kind = RuntimeCommandKind::RenameSession;
    runtime.target = Target::named(request.target_name);
    runtime.name = request.new_name;
    return runtime;
  }

  if (request.type == "KillSession") {
    runtime.kind = RuntimeCommandKind::KillSession;
    runtime.target = Target::named(request.session_name);
    return runtime;
  }

  if (request.type == "NewWindow") {
    runtime.kind = RuntimeCommandKind::NewWindow;
    runtime.target = target_from_optional_session_name(request.session_name);
    runtime.name = request.window_name;
    return runtime;
  }

  if (request.type == "RenameWindow") {
    runtime.kind = RuntimeCommandKind::RenameWindow;
    runtime.target = target_from_optional_session_name(request.session_name);
    runtime.name = request.window_name;
    return runtime;
  }

  if (request.type == "SplitWindow") {
    const auto direction = parse_runtime_split_direction(request.split_direction);
    if (!direction) {
      error = "wmux: split-window requires one of -h or -v";
      return std::nullopt;
    }
    runtime.kind = RuntimeCommandKind::SplitPane;
    runtime.target = target_from_optional_session_name(request.session_name);
    runtime.axis = *direction;
    return runtime;
  }

  if (request.type == "SetOption") {
    runtime.kind = RuntimeCommandKind::SetOption;
    runtime.option_scope = OptionScope::Global;
    runtime.key = request.option_name;
    runtime.value = request.option_value;
    return runtime;
  }

  if (request.type == "BindKey") {
    runtime.kind = RuntimeCommandKind::BindKey;
    runtime.option_scope = OptionScope::Global;
    runtime.key = request.key_name;
    runtime.value = request.key_action;
    return runtime;
  }

  if (request.type == "UnbindKey") {
    runtime.kind = RuntimeCommandKind::UnbindKey;
    runtime.option_scope = OptionScope::Global;
    runtime.key = request.key_name;
    return runtime;
  }

  error = "wmux: request is not a runtime command";
  return std::nullopt;
}

}  // namespace wmux::daemon_internal
