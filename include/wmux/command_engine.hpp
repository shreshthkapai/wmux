#pragma once

#include "wmux/session_manager.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wmux {

enum class RuntimeCommandKind {
  NewSession,
  RenameSession,
  KillSession,
  NewWindow,
  RenameWindow,
  NextWindow,
  PreviousWindow,
  KillWindow,
  SplitPane,
  KillPane,
  SelectPane,
  ResizePane,
  SpreadPanesEvenly,
  EnterCopyMode,
  ExitCopyMode,
  CopySelection,
  PasteBuffer,
  SetOption,
  BindKey,
  UnbindKey,
  DisplayMessage,
  DetachClient,
  AttachSession,
};

enum class CommandStatus {
  Success,
  UserError,
  InternalError,
  NoOp,
};

enum class RedrawRequest {
  None,
  ActivePane,
  ActiveWindow,
  Client,
  AllClients,
};

enum class ResizeDirection {
  Left,
  Right,
  Up,
  Down,
};

enum class OptionScope {
  Global,
  Session,
  Window,
  Pane,
};

enum class TargetKind {
  Current,
  Session,
  Window,
  Pane,
  Client,
  MousePosition,
  Named,
};

struct Target {
  TargetKind kind{TargetKind::Current};
  SessionId session_id{0};
  WindowId window_id{0};
  PaneId pane_id{0};
  ClientId client_id{0};
  std::uint16_t x{0};
  std::uint16_t y{0};
  std::string name;

  static Target current();
  static Target session(SessionId id);
  static Target window(WindowId id);
  static Target pane(PaneId id);
  static Target client(ClientId id);
  static Target mouse_position(ClientId id, std::uint16_t column, std::uint16_t row);
  static Target named(std::string value);
};

struct ResolvedTarget {
  SessionId session_id{0};
  WindowId window_id{0};
  std::optional<PaneId> pane_id;
  std::optional<ClientId> client_id;
};

struct RuntimeCommand {
  RuntimeCommandKind kind{RuntimeCommandKind::DisplayMessage};
  Target target{Target::current()};
  std::optional<std::string> name;
  std::optional<std::string> shell;
  SplitDirection axis{SplitDirection::Horizontal};
  PaneDirection pane_direction{PaneDirection::Right};
  ResizeDirection resize_direction{ResizeDirection::Right};
  std::uint16_t amount{0};
  std::optional<PaneSplitResizeTarget> split_resize_target;
  int mouse_column{0};
  int mouse_row{0};
  bool confirm{false};
  OptionScope option_scope{OptionScope::Global};
  std::string key;
  std::string value;
  std::string message;
};

std::string_view runtime_command_name(RuntimeCommandKind kind);
std::string_view command_status_name(CommandStatus status);
std::string_view redraw_request_name(RedrawRequest redraw);
std::string_view target_kind_name(TargetKind kind);

}  // namespace wmux
