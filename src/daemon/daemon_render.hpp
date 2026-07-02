#pragma once

#include "wmux/ipc_protocol.hpp"
#include "wmux/pty_output.hpp"
#include "wmux/session_manager.hpp"
#include "wmux/status_line.hpp"
#include "wmux/ui_theme.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace wmux {
class PtyProcess;
}

namespace wmux::daemon_internal {

struct RenderPane {
  PaneLayoutRect rect;
  bool active{false};
  std::shared_ptr<PtyProcess> shell;
};

struct ActiveWindowFrame {
  WindowId window_id{0};
  PaneId active_pane_id{0};
  std::string session_name;
  std::string window_name;
  std::size_t window_index{0};
  int columns{120};
  int rows{30};
  bool status_bar_enabled{true};
  std::vector<RenderPane> panes;
};

struct PaneViewportState {
  std::size_t offset{0};
  std::size_t observed_line_count{0};
};

using PaneViewportStates = std::unordered_map<PaneId, PaneViewportState>;

struct RenderFrameOptions {
  bool clear_terminal{true};
  bool draw_borders{true};
  bool draw_status{true};
  std::unordered_set<PaneId> dirty_panes;
};

struct RenderDiffCell {
  std::string text;
  TerminalCellWidth width{TerminalCellWidth::Narrow};
  TerminalAttributes attributes;
  bool accent_overlay{false};
};

struct RenderDiffRow {
  int row{0};
  int column{0};
  int width{0};
  std::vector<RenderDiffCell> cells;
};

struct RenderDiffPane {
  PaneLayoutRect rect;
  std::size_t first_visible_line{0};
  std::size_t viewport_offset{0};
  std::vector<RenderDiffRow> body_rows;
};

struct RenderDiffState {
  int columns{0};
  int rows{0};
  bool status_bar_enabled{true};
  bool initialized{false};
  std::unordered_map<PaneId, RenderDiffPane> panes;
  std::string status_line;
};

struct RenderStatus {
  StatusState state;
  StatusLineMode mode{StatusLineMode::Normal};
  bool mouse_enabled{false};
  bool mouse_drag_active{false};
  UiTheme ui;
};

struct CopyModePoint {
  std::size_t line{0};
  std::size_t column{0};
};

struct CopyModeState {
  bool active{false};
  PaneId pane_id{0};
  std::size_t cursor_row{0};
  std::size_t cursor_column{0};
  bool selection_active{false};
  CopyModePoint selection_anchor;
};

int body_width(const PaneLayoutRect& rect);
int body_height(const PaneLayoutRect& rect);

std::string render_frame(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    std::string_view status_override);

std::string render_frame(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status);

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    std::string_view status_override,
    const RenderFrameOptions& options);

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options);

std::string render_frame_update(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options,
    RenderDiffState& diff_state);

void reset_render_diff_state(RenderDiffState& diff_state);

void update_viewport_states(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode);

bool apply_active_viewport_scroll(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    AttachScrollAction action);

void clamp_copy_mode_cursor(
    CopyModeState& copy_mode,
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots);

bool apply_copy_mode_action(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    PaneViewportStates& viewport_states,
    CopyModeState& copy_mode,
    AttachCopyModeAction action,
    std::string& copied_text);

}  // namespace wmux::daemon_internal
