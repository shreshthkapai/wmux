#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wmux {

using SessionId = std::uint64_t;
using WindowId = std::uint64_t;
using PaneId = std::uint64_t;

enum class SplitDirection {
  Horizontal,
  Vertical,
};

enum class PaneDirection {
  Left,
  Right,
  Up,
  Down,
};

enum class PaneTreePathStep {
  First,
  Second,
};

struct PaneSummary {
  PaneId id{0};
  std::chrono::system_clock::time_point created_at;
};

struct PaneLayoutRect {
  PaneId pane_id{0};
  int left{0};
  int top{0};
  int width{0};
  int height{0};
};

struct PaneSplitResizeTarget {
  std::vector<PaneTreePathStep> path;
  SplitDirection direction{SplitDirection::Horizontal};
  int left{0};
  int top{0};
  int width{0};
  int height{0};
};

struct PaneNode {
  enum class Kind {
    Leaf,
    Split,
  };

  Kind kind{Kind::Leaf};
  PaneId pane_id{0};
  SplitDirection direction{SplitDirection::Horizontal};
  double ratio{0.5};
  std::unique_ptr<PaneNode> first;
  std::unique_ptr<PaneNode> second;

  PaneNode() = default;
  explicit PaneNode(PaneId leaf_pane_id);
  PaneNode(const PaneNode& other);
  PaneNode& operator=(const PaneNode& other);
  PaneNode(PaneNode&&) noexcept = default;
  PaneNode& operator=(PaneNode&&) noexcept = default;
  ~PaneNode() = default;
};

struct WindowSummary {
  WindowId id{0};
  std::string name;
  PaneId active_pane_id{0};
  PaneNode pane_tree;
  std::vector<PaneSummary> panes;
  std::chrono::system_clock::time_point created_at;
};

struct SessionSummary {
  SessionId id{0};
  std::string name;
  WindowId active_window_id{0};
  std::vector<WindowSummary> windows;
  std::chrono::system_clock::time_point created_at;
};

enum class SessionError {
  None,
  EmptyName,
  DuplicateName,
  NotFound,
};

struct SessionOperationResult {
  bool ok{false};
  SessionError error{SessionError::None};
  SessionId id{0};
  WindowId window_id{0};
  PaneId pane_id{0};
};

enum class WindowError {
  None,
  EmptyName,
  DuplicateName,
  SessionNotFound,
  WindowNotFound,
  LastWindow,
};

struct WindowOperationResult {
  bool ok{false};
  WindowError error{WindowError::None};
  SessionId session_id{0};
  WindowId window_id{0};
  PaneId pane_id{0};
  WindowId removed_window_id{0};
};

enum class PaneError {
  None,
  SessionNotFound,
  WindowNotFound,
  PaneNotFound,
  LastPane,
};

struct PaneOperationResult {
  bool ok{false};
  PaneError error{PaneError::None};
  SessionId session_id{0};
  WindowId window_id{0};
  PaneId pane_id{0};
  PaneId removed_pane_id{0};
};

class SessionManager {
 public:
  SessionOperationResult create_session(std::string name);
  SessionOperationResult rename_session(std::string_view current_name, std::string new_name);
  SessionOperationResult kill_session(std::string_view name);
  WindowOperationResult create_window(SessionId session_id, std::string name);
  WindowOperationResult rename_active_window(SessionId session_id, std::string name);
  WindowOperationResult select_next_window(SessionId session_id);
  WindowOperationResult select_previous_window(SessionId session_id);
  WindowOperationResult kill_active_window(SessionId session_id);
  PaneOperationResult split_active_pane(SessionId session_id, SplitDirection direction);
  PaneOperationResult select_pane(SessionId session_id, PaneDirection direction);
  PaneOperationResult select_pane(SessionId session_id, PaneId pane_id);
  PaneOperationResult resize_active_window_split(
      SessionId session_id,
      const PaneSplitResizeTarget& target,
      int column,
      int row);
  PaneOperationResult kill_active_pane(SessionId session_id);

  bool has_session(std::string_view name) const;
  std::optional<SessionId> session_id_for_name(std::string_view name) const;
  std::optional<SessionId> only_session_id() const;
  std::optional<WindowId> active_window_id(SessionId session_id) const;
  std::optional<PaneId> active_pane_id(SessionId session_id) const;
  std::optional<WindowSummary> active_window_summary(SessionId session_id) const;
  std::size_t session_count() const;
  std::vector<SessionSummary> list_sessions() const;
  std::vector<WindowSummary> list_windows(SessionId session_id) const;

 private:
  std::unordered_map<SessionId, SessionSummary> sessions_;
  std::unordered_map<std::string, SessionId> name_index_;
  std::vector<SessionId> order_;
  SessionId next_id_{1};
  WindowId next_window_id_{1};
  PaneId next_pane_id_{1};
};

std::vector<PaneLayoutRect> compute_pane_layout_rects(
    const PaneNode& root,
    int columns,
    int rows);
std::optional<PaneSplitResizeTarget> find_pane_split_resize_target(
    const PaneNode& root,
    int column,
    int row,
    int columns,
    int rows);

}  // namespace wmux
