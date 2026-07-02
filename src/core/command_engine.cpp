#include "wmux/command_engine.hpp"

#include <utility>

namespace wmux {

Target Target::current() {
  return {};
}

Target Target::session(SessionId id) {
  Target target;
  target.kind = TargetKind::Session;
  target.session_id = id;
  return target;
}

Target Target::window(WindowId id) {
  Target target;
  target.kind = TargetKind::Window;
  target.window_id = id;
  return target;
}

Target Target::pane(PaneId id) {
  Target target;
  target.kind = TargetKind::Pane;
  target.pane_id = id;
  return target;
}

Target Target::client(ClientId id) {
  Target target;
  target.kind = TargetKind::Client;
  target.client_id = id;
  return target;
}

Target Target::mouse_position(ClientId id, std::uint16_t column, std::uint16_t row) {
  Target target;
  target.kind = TargetKind::MousePosition;
  target.client_id = id;
  target.x = column;
  target.y = row;
  return target;
}

Target Target::named(std::string value) {
  Target target;
  target.kind = TargetKind::Named;
  target.name = std::move(value);
  return target;
}

std::string_view runtime_command_name(RuntimeCommandKind kind) {
  switch (kind) {
    case RuntimeCommandKind::NewSession:
      return "NewSession";
    case RuntimeCommandKind::RenameSession:
      return "RenameSession";
    case RuntimeCommandKind::KillSession:
      return "KillSession";
    case RuntimeCommandKind::NewWindow:
      return "NewWindow";
    case RuntimeCommandKind::RenameWindow:
      return "RenameWindow";
    case RuntimeCommandKind::NextWindow:
      return "NextWindow";
    case RuntimeCommandKind::PreviousWindow:
      return "PreviousWindow";
    case RuntimeCommandKind::SelectWindow:
      return "SelectWindow";
    case RuntimeCommandKind::KillWindow:
      return "KillWindow";
    case RuntimeCommandKind::SplitPane:
      return "SplitPane";
    case RuntimeCommandKind::KillPane:
      return "KillPane";
    case RuntimeCommandKind::SelectPane:
      return "SelectPane";
    case RuntimeCommandKind::ResizePane:
      return "ResizePane";
    case RuntimeCommandKind::SpreadPanesEvenly:
      return "SpreadPanesEvenly";
    case RuntimeCommandKind::EnterCopyMode:
      return "EnterCopyMode";
    case RuntimeCommandKind::ExitCopyMode:
      return "ExitCopyMode";
    case RuntimeCommandKind::CopySelection:
      return "CopySelection";
    case RuntimeCommandKind::PasteBuffer:
      return "PasteBuffer";
    case RuntimeCommandKind::SetOption:
      return "SetOption";
    case RuntimeCommandKind::BindKey:
      return "BindKey";
    case RuntimeCommandKind::UnbindKey:
      return "UnbindKey";
    case RuntimeCommandKind::DisplayMessage:
      return "DisplayMessage";
    case RuntimeCommandKind::DetachClient:
      return "DetachClient";
    case RuntimeCommandKind::AttachSession:
      return "AttachSession";
  }

  return "Unknown";
}

std::string_view command_status_name(CommandStatus status) {
  switch (status) {
    case CommandStatus::Success:
      return "Success";
    case CommandStatus::UserError:
      return "UserError";
    case CommandStatus::InternalError:
      return "InternalError";
    case CommandStatus::NoOp:
      return "NoOp";
  }

  return "Unknown";
}

std::string_view redraw_request_name(RedrawRequest redraw) {
  switch (redraw) {
    case RedrawRequest::None:
      return "None";
    case RedrawRequest::ActivePane:
      return "ActivePane";
    case RedrawRequest::ActiveWindow:
      return "ActiveWindow";
    case RedrawRequest::Client:
      return "Client";
    case RedrawRequest::AllClients:
      return "AllClients";
  }

  return "Unknown";
}

std::string_view target_kind_name(TargetKind kind) {
  switch (kind) {
    case TargetKind::Current:
      return "Current";
    case TargetKind::Session:
      return "Session";
    case TargetKind::Window:
      return "Window";
    case TargetKind::Pane:
      return "Pane";
    case TargetKind::Client:
      return "Client";
    case TargetKind::MousePosition:
      return "MousePosition";
    case TargetKind::Named:
      return "Named";
  }

  return "Unknown";
}

}  // namespace wmux
