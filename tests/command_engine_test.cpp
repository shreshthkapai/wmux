#include "daemon_command_engine.hpp"
#include "wmux/command_engine.hpp"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace {

struct TargetFixture {
  wmux::daemon_internal::DaemonState state;
  wmux::SessionOperationResult session;
  wmux::PaneOperationResult split;
  wmux::ClientId client_id{77};
};

std::unique_ptr<TargetFixture> make_target_fixture() {
  auto fixture = std::make_unique<TargetFixture>();
  {
    std::lock_guard lock(fixture->state.mutex);
    fixture->session = fixture->state.sessions.create_session("finance");
    assert(fixture->session.ok);
    fixture->split =
        fixture->state.sessions.split_active_pane(fixture->session.id, wmux::SplitDirection::Horizontal);
    assert(fixture->split.ok);

    wmux::daemon_internal::DaemonState::AttachClientRuntime client;
    client.client.id = fixture->client_id;
    client.client.attached_session = fixture->session.id;
    client.client.active_window = fixture->session.window_id;
    client.client.active_pane = fixture->split.pane_id;
    client.client.size.columns = 100;
    client.client.size.rows = 20;
    client.session_name = "finance";
    fixture->state.attach_clients.emplace(fixture->client_id, std::move(client));
  }

  return fixture;
}

void exposes_runtime_command_names() {
  assert(wmux::runtime_command_name(wmux::RuntimeCommandKind::NewWindow) == "NewWindow");
  assert(wmux::runtime_command_name(wmux::RuntimeCommandKind::SpreadPanesEvenly) ==
         "SpreadPanesEvenly");
  assert(wmux::command_status_name(wmux::CommandStatus::UserError) == "UserError");
  assert(wmux::redraw_request_name(wmux::RedrawRequest::ActiveWindow) == "ActiveWindow");
  assert(wmux::target_kind_name(wmux::TargetKind::MousePosition) == "MousePosition");
}

void constructs_targets_without_manual_field_mutation() {
  const auto session = wmux::Target::session(12);
  assert(session.kind == wmux::TargetKind::Session);
  assert(session.session_id == 12);

  const auto mouse = wmux::Target::mouse_position(5, 9, 4);
  assert(mouse.kind == wmux::TargetKind::MousePosition);
  assert(mouse.client_id == 5);
  assert(mouse.x == 9);
  assert(mouse.y == 4);

  const auto named = wmux::Target::named("logs");
  assert(named.kind == wmux::TargetKind::Named);
  assert(named.name == "logs");
}

void maps_attach_commands_to_runtime_commands() {
  std::string error;

  const auto create = wmux::daemon_internal::runtime_command_from_attach_command(
      "new-window",
      error);
  assert(create);
  assert(create->kind == wmux::RuntimeCommandKind::NewWindow);
  assert(create->target.kind == wmux::TargetKind::Current);

  const auto next = wmux::daemon_internal::runtime_command_from_attach_command(
      "next-window",
      error);
  assert(next);
  assert(next->kind == wmux::RuntimeCommandKind::NextWindow);

  const auto select_window = wmux::daemon_internal::runtime_command_from_attach_command(
      "select-window-2",
      error);
  assert(select_window);
  assert(select_window->kind == wmux::RuntimeCommandKind::SelectWindow);
  assert(select_window->target.kind == wmux::TargetKind::Named);
  assert(select_window->target.name == "2");

  const auto split_horizontal = wmux::daemon_internal::runtime_command_from_attach_command(
      "split-horizontal",
      error);
  assert(split_horizontal);
  assert(split_horizontal->kind == wmux::RuntimeCommandKind::SplitPane);
  assert(split_horizontal->axis == wmux::SplitDirection::Horizontal);

  const auto select_left = wmux::daemon_internal::runtime_command_from_attach_command(
      "select-pane-left",
      error);
  assert(select_left);
  assert(select_left->kind == wmux::RuntimeCommandKind::SelectPane);
  assert(select_left->pane_direction == wmux::PaneDirection::Left);

  const auto spread = wmux::daemon_internal::runtime_command_from_attach_command(
      "equalize-panes",
      error);
  assert(spread);
  assert(spread->kind == wmux::RuntimeCommandKind::SpreadPanesEvenly);

  error.clear();
  const auto unknown = wmux::daemon_internal::runtime_command_from_attach_command(
      "totally-unknown",
      error);
  assert(!unknown);
  assert(error == "wmux: unknown attach command");
}

void maps_command_mode_text_to_runtime_commands() {
  std::string error;

  const auto rename_session = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "rename-session trading",
      error);
  assert(rename_session);
  assert(rename_session->kind == wmux::RuntimeCommandKind::RenameSession);
  assert(rename_session->target.kind == wmux::TargetKind::Current);
  assert(rename_session->name == "trading");

  const auto new_window = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "new-window -n logs",
      error);
  assert(new_window);
  assert(new_window->kind == wmux::RuntimeCommandKind::NewWindow);
  assert(new_window->name == "logs");

  const auto unnamed_window = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "new-window",
      error);
  assert(unnamed_window);
  assert(unnamed_window->kind == wmux::RuntimeCommandKind::NewWindow);
  assert(!unnamed_window->name);

  const auto select_window = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "select-window -t 1",
      error);
  assert(select_window);
  assert(select_window->kind == wmux::RuntimeCommandKind::SelectWindow);
  assert(select_window->target.kind == wmux::TargetKind::Named);
  assert(select_window->target.name == "1");

  const auto split_vertical = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "split-window -v",
      error);
  assert(split_vertical);
  assert(split_vertical->kind == wmux::RuntimeCommandKind::SplitPane);
  assert(split_vertical->axis == wmux::SplitDirection::Vertical);

  const auto tmux_spread = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "select-layout -E",
      error);
  assert(tmux_spread);
  assert(tmux_spread->kind == wmux::RuntimeCommandKind::SpreadPanesEvenly);

  const auto resize_left = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "resize-pane -L",
      error);
  assert(resize_left);
  assert(resize_left->kind == wmux::RuntimeCommandKind::ResizePane);
  assert(resize_left->resize_direction == wmux::ResizeDirection::Left);

  const auto bind_key = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "bind-key z new-window",
      error);
  assert(bind_key);
  assert(bind_key->kind == wmux::RuntimeCommandKind::BindKey);
  assert(bind_key->key == "z");
  assert(bind_key->value == "new-window");

  const auto unbind_key = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "unbind-key e",
      error);
  assert(unbind_key);
  assert(unbind_key->kind == wmux::RuntimeCommandKind::UnbindKey);
  assert(unbind_key->key == "e");

  error.clear();
  const auto targeted = wmux::daemon_internal::runtime_command_from_command_mode_text(
      "new-window -t finance -n logs",
      error);
  assert(!targeted);
  assert(error == "wmux: command mode operates on the attached session; omit -t <session>");
}

void maps_ipc_requests_to_runtime_commands() {
  std::string error;

  wmux::IpcRequest new_session;
  new_session.type = "NewSession";
  new_session.session_name = "finance";
  const auto create = wmux::daemon_internal::runtime_command_from_ipc_request(new_session, error);
  assert(create);
  assert(create->kind == wmux::RuntimeCommandKind::NewSession);
  assert(create->name == "finance");

  wmux::IpcRequest rename_session;
  rename_session.type = "RenameSession";
  rename_session.target_name = "finance";
  rename_session.new_name = "trading";
  const auto rename =
      wmux::daemon_internal::runtime_command_from_ipc_request(rename_session, error);
  assert(rename);
  assert(rename->kind == wmux::RuntimeCommandKind::RenameSession);
  assert(rename->target.kind == wmux::TargetKind::Named);
  assert(rename->target.name == "finance");
  assert(rename->name == "trading");

  wmux::IpcRequest split;
  split.type = "SplitWindow";
  split.session_name = "finance";
  split.split_direction = "horizontal";
  const auto split_command = wmux::daemon_internal::runtime_command_from_ipc_request(split, error);
  assert(split_command);
  assert(split_command->kind == wmux::RuntimeCommandKind::SplitPane);
  assert(split_command->target.kind == wmux::TargetKind::Named);
  assert(split_command->target.name == "finance");
  assert(split_command->axis == wmux::SplitDirection::Horizontal);

  wmux::IpcRequest select_window;
  select_window.type = "SelectWindow";
  select_window.target_name = "finance:1";
  const auto select_window_command =
      wmux::daemon_internal::runtime_command_from_ipc_request(select_window, error);
  assert(select_window_command);
  assert(select_window_command->kind == wmux::RuntimeCommandKind::SelectWindow);
  assert(select_window_command->target.kind == wmux::TargetKind::Named);
  assert(select_window_command->target.name == "finance:1");

  wmux::IpcRequest resize;
  resize.type = "ResizePane";
  resize.resize_direction = "-D";
  const auto resize_command =
      wmux::daemon_internal::runtime_command_from_ipc_request(resize, error);
  assert(resize_command);
  assert(resize_command->kind == wmux::RuntimeCommandKind::ResizePane);
  assert(resize_command->resize_direction == wmux::ResizeDirection::Down);

  wmux::IpcRequest bind;
  bind.type = "BindKey";
  bind.key_name = "z";
  bind.key_action = "new-window";
  const auto bind_command = wmux::daemon_internal::runtime_command_from_ipc_request(bind, error);
  assert(bind_command);
  assert(bind_command->kind == wmux::RuntimeCommandKind::BindKey);
  assert(bind_command->key == "z");
  assert(bind_command->value == "new-window");

  wmux::IpcRequest unbind;
  unbind.type = "UnbindKey";
  unbind.key_name = "e";
  const auto unbind_command =
      wmux::daemon_internal::runtime_command_from_ipc_request(unbind, error);
  assert(unbind_command);
  assert(unbind_command->kind == wmux::RuntimeCommandKind::UnbindKey);
  assert(unbind_command->key == "e");
}

void resolves_current_client_target() {
  auto fixture = make_target_fixture();
  const auto resolved = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::current(),
      fixture->client_id);

  assert(resolved.ok);
  assert(resolved.target.client_id == fixture->client_id);
  assert(resolved.target.session_id == fixture->session.id);
  assert(resolved.target.window_id == fixture->session.window_id);
  assert(resolved.target.pane_id == fixture->split.pane_id);
}

void resolves_explicit_session_window_and_pane_targets() {
  auto fixture = make_target_fixture();

  const auto session = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::session(fixture->session.id),
      fixture->client_id);
  assert(session.ok);
  assert(session.target.session_id == fixture->session.id);
  assert(session.target.window_id == fixture->session.window_id);
  assert(session.target.pane_id == fixture->split.pane_id);

  const auto window = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::window(fixture->session.window_id),
      fixture->client_id);
  assert(window.ok);
  assert(window.target.session_id == fixture->session.id);
  assert(window.target.window_id == fixture->session.window_id);

  const auto pane = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::pane(fixture->session.pane_id),
      fixture->client_id);
  assert(pane.ok);
  assert(pane.target.session_id == fixture->session.id);
  assert(pane.target.window_id == fixture->session.window_id);
  assert(pane.target.pane_id == fixture->session.pane_id);
}

void resolves_named_session_and_window_targets() {
  auto fixture = make_target_fixture();
  {
    std::lock_guard lock(fixture->state.mutex);
    const auto created = fixture->state.sessions.create_window(fixture->session.id, "logs");
    assert(created.ok);
  }

  const auto session = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::named("finance"),
      fixture->client_id);
  assert(session.ok);
  assert(session.target.session_id == fixture->session.id);

  const auto window = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::named("logs"),
      fixture->client_id);
  assert(window.ok);
  assert(window.target.session_id == fixture->session.id);
  assert(window.target.window_id != fixture->session.window_id);
}

void resolves_tmux_session_window_targets() {
  auto fixture = make_target_fixture();
  {
    std::lock_guard lock(fixture->state.mutex);
    const auto created = fixture->state.sessions.create_window(fixture->session.id, "logs");
    assert(created.ok);
  }

  const auto by_index = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::named("finance:1"),
      fixture->client_id);
  assert(by_index.ok);
  assert(by_index.target.session_id == fixture->session.id);
  assert(by_index.target.window_id != fixture->session.window_id);

  const auto by_name = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::named("finance:logs"),
      fixture->client_id);
  assert(by_name.ok);
  assert(by_name.target.window_id == by_index.target.window_id);
}

void resolves_mouse_position_to_pane() {
  auto fixture = make_target_fixture();
  const auto left_pane = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::mouse_position(fixture->client_id, 1, 1),
      fixture->client_id);

  assert(left_pane.ok);
  assert(left_pane.target.session_id == fixture->session.id);
  assert(left_pane.target.window_id == fixture->session.window_id);
  assert(left_pane.target.pane_id == fixture->session.pane_id);

  const auto right_pane = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::mouse_position(fixture->client_id, 99, 1),
      fixture->client_id);

  assert(right_pane.ok);
  assert(right_pane.target.pane_id == fixture->split.pane_id);
}

void reports_stale_targets_as_user_errors() {
  auto fixture = make_target_fixture();

  const auto missing_client = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::current(),
      wmux::ClientId{404});
  assert(!missing_client.ok);
  assert(missing_client.error == "wmux: target client not found");

  const auto missing_session = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::session(404),
      fixture->client_id);
  assert(!missing_session.ok);
  assert(missing_session.error == "wmux: target session not found");

  const auto missing_pane = wmux::daemon_internal::resolve_target(
      fixture->state,
      wmux::Target::pane(404),
      fixture->client_id);
  assert(!missing_pane.ok);
  assert(missing_pane.error == "wmux: target pane not found");
}

}  // namespace

void run_command_engine_tests() {
  exposes_runtime_command_names();
  constructs_targets_without_manual_field_mutation();
  maps_attach_commands_to_runtime_commands();
  maps_command_mode_text_to_runtime_commands();
  maps_ipc_requests_to_runtime_commands();
  resolves_current_client_target();
  resolves_explicit_session_window_and_pane_targets();
  resolves_named_session_and_window_targets();
  resolves_tmux_session_window_targets();
  resolves_mouse_position_to_pane();
  reports_stale_targets_as_user_errors();
}
