#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wmux {

using SessionId = std::uint64_t;
using WindowId = std::uint64_t;
using PaneId = std::uint64_t;
using ClientId = std::uint64_t;
using NodeId = std::uint64_t;
using RequestId = std::uint64_t;

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

using PaneTreePathStep = std::size_t;

struct PaneSummary {
  PaneId id{0};
  WindowId window_id{0};
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
  NodeId split_node_id{0};
  std::vector<PaneTreePathStep> path;
  std::size_t child_index{0};
  SplitDirection direction{SplitDirection::Horizontal};
  int left{0};
  int top{0};
  int width{0};
  int height{0};
};

struct PaneNode {
  // Snapshot of the internal LayoutArena tree. This is used for summaries,
  // rendering helpers, and tests; Window owns the authoritative LayoutArena.
  // Layout tree invariants:
  // - Leaf nodes own exactly one PaneId and have no children.
  // - Split nodes have at least two children and one positive finite weight per child.
  // - Same-axis split children are flattened by mutation paths.
  // - Rectangles are derived from the tree; they are not stored here.
  enum class Kind {
    Leaf,
    Split,
  };

  NodeId node_id{0};
  Kind kind{Kind::Leaf};
  PaneId pane_id{0};
  SplitDirection direction{SplitDirection::Horizontal};
  std::vector<PaneNode> children;
  std::vector<double> child_weights;

  PaneNode() = default;
  explicit PaneNode(PaneId leaf_pane_id);
  PaneNode(const PaneNode& other) = default;
  PaneNode& operator=(const PaneNode& other) = default;
  PaneNode(PaneNode&&) noexcept = default;
  PaneNode& operator=(PaneNode&&) noexcept = default;
  ~PaneNode() = default;
};

struct WindowSummary {
  WindowId id{0};
  SessionId session_id{0};
  std::size_t index{0};
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

struct SessionOptions {};

struct WindowOptions {};

struct LayoutNode {
  enum class Kind {
    Leaf,
    Split,
  };

  NodeId id{0};
  std::optional<NodeId> parent_id;
  Kind kind{Kind::Leaf};
  PaneId pane_id{0};
  SplitDirection direction{SplitDirection::Horizontal};
  std::vector<NodeId> children;
  std::vector<double> child_weights;
};

struct LayoutArena {
  std::unordered_map<NodeId, LayoutNode> nodes;
  std::unordered_map<PaneId, NodeId> pane_leaf_index;
  NodeId root_id{0};
  NodeId next_node_id{1};
};

enum class PaneModeState {
  Normal,
};

struct Pane {
  PaneId id{0};
  WindowId window_id{0};
  PaneModeState mode_state{PaneModeState::Normal};
  std::optional<int> last_exit_code;
  std::chrono::system_clock::time_point created_at;
};

struct Window {
  WindowId id{0};
  SessionId session_id{0};
  // User-facing display index. WindowId remains the internal identity.
  std::size_t index{0};
  std::string name;
  std::unordered_map<PaneId, Pane> panes;
  std::vector<PaneId> pane_order;
  PaneId active_pane_id{0};
  NodeId layout_root{0};
  LayoutArena layout;
  WindowOptions options;
  std::chrono::system_clock::time_point created_at;
};

struct Session {
  SessionId id{0};
  std::string name;
  std::vector<WindowId> windows;
  WindowId active_window_id{0};
  SessionOptions options;
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
  NoSplit,
};

struct PaneOperationResult {
  bool ok{false};
  PaneError error{PaneError::None};
  SessionId session_id{0};
  WindowId window_id{0};
  PaneId pane_id{0};
  PaneId removed_pane_id{0};
  bool changed{false};
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
  WindowOperationResult select_window(SessionId session_id, WindowId window_id);
  WindowOperationResult kill_active_window(SessionId session_id);
  PaneOperationResult split_active_pane(SessionId session_id, SplitDirection direction);
  PaneOperationResult select_pane(SessionId session_id, PaneDirection direction);
  PaneOperationResult select_pane(
      SessionId session_id,
      PaneDirection direction,
      int columns,
      int rows);
  PaneOperationResult select_pane(SessionId session_id, PaneId pane_id);
  PaneOperationResult resize_active_window_split(
      SessionId session_id,
      const PaneSplitResizeTarget& target,
      int column,
      int row);
  PaneOperationResult resize_active_pane(
      SessionId session_id,
      PaneDirection direction,
      std::uint16_t amount,
      int columns,
      int rows);
  PaneOperationResult equalize_active_window_panes(SessionId session_id);
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
  std::unordered_map<SessionId, Session> sessions_;
  std::unordered_map<WindowId, Window> windows_;
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
