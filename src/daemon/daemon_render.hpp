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
  std::uint64_t layout_generation{0};
  PaneId active_pane_id{0};
  std::string session_name;
  std::string window_name;
  std::size_t window_index{0};
  int columns{120};
  int rows{30};
  int pane_rows{0};
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
  bool draw_pane_bodies{true};
  std::unordered_set<PaneId> dirty_panes;
  bool force_body_repaint{false};
  bool preserve_layout_cache{false};
  bool repaint_body_on_geometry_change{false};
};

struct RenderFrameStats {
  std::size_t visible_pane_rows{0};
  std::size_t rows_considered{0};
  std::size_t rows_emitted{0};
  std::size_t rows_skipped_generation_cache{0};
  std::size_t rows_skipped_empty_default{0};
  std::size_t body_bytes_emitted{0};
  std::size_t border_status_bytes_emitted{0};
  std::size_t cursor_bytes_emitted{0};
  std::size_t alternate_screen_panes{0};
};

struct RenderDiffCell {
  std::string text;
  TerminalCellWidth width{TerminalCellWidth::Narrow};
  TerminalAttributes attributes;
  bool accent_overlay{false};
};

enum class SceneRowKind {
  PaneBody,
  Border,
  Status,
  Overlay,
  EmptyOutside,
};

struct SceneSpan {
  int column{0};
  std::vector<RenderDiffCell> cells;
};

struct SceneRow {
  SceneRowKind kind{SceneRowKind::PaneBody};
  PaneId pane_id{0};
  int row{0};
  int column{0};
  int width{0};
  std::vector<SceneSpan> spans;
};

struct SceneCursor {
  bool known{false};
  bool visible{false};
  PaneId pane_id{0};
  int row{0};
  int column{0};
  int style{0};
};

struct ScenePane {
  PaneId pane_id{0};
  PaneLayoutRect rect;
  bool active{false};
  int body_left{0};
  int body_top{0};
  int body_width{0};
  int body_height{0};
  std::size_t first_visible_line{0};
  std::size_t viewport_offset{0};
  bool alternate_screen{false};
  std::vector<SceneRow> body_rows;
};

struct SceneBorder {
  std::vector<SceneRow> rows;
};

struct SceneStatus {
  bool visible{false};
  int row{0};
  std::string text;
  std::vector<SceneRow> rows;
};

struct VisibleScene {
  WindowId window_id{0};
  std::uint64_t layout_generation{0};
  PaneId active_pane_id{0};
  int columns{0};
  int rows{0};
  bool status_bar_enabled{true};
  std::vector<ScenePane> panes;
  SceneBorder borders;
  SceneStatus status;
  SceneCursor cursor;
  std::vector<SceneRow> empty_rows;
};

struct RenderDiffRow {
  int row{0};
  int column{0};
  int width{0};
  std::vector<RenderDiffCell> cells;
};

struct EncodedRowCache {
  std::uint64_t generation{0};
  std::uint64_t style_generation{0};
  int width{0};
  std::string encoded;
};

struct RenderDiffPane {
  PaneLayoutRect rect;
  std::size_t first_visible_line{0};
  std::size_t viewport_offset{0};
  std::vector<RenderDiffRow> body_rows;
  std::vector<EncodedRowCache> encoded_rows;
};

struct ClientPhysicalBaseline {
  WindowId window_id{0};
  std::uint64_t layout_generation{0};
  int columns{0};
  int rows{0};
  bool status_bar_enabled{true};
  bool baseline_valid{false};
  bool initialized{false};
  bool scene_valid{false};
  VisibleScene scene;
  std::unordered_map<PaneId, RenderDiffPane> panes;
  std::string status_line;
};

using RenderDiffState = ClientPhysicalBaseline;

struct RenderStatus {
  StatusState state;
  StatusLineMode mode{StatusLineMode::Normal};
  bool mouse_enabled{false};
  bool mouse_drag_active{false};
  bool synchronized_output_supported{false};
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
int body_width(const PaneLayoutRect& rect, int frame_columns);
int body_height(const PaneLayoutRect& rect, int frame_rows);

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

std::string render_live_frame_update(
    const ActiveWindowFrame& frame,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status,
    const RenderFrameOptions& options,
    RenderDiffState& diff_state,
    std::unordered_map<PaneId, std::uint64_t>& next_sequences,
    RenderFrameStats* stats = nullptr);

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

VisibleScene build_visible_scene(
    const ActiveWindowFrame& frame,
    const std::unordered_map<PaneId, PtyOutputSnapshot>& snapshots,
    const PaneViewportStates& viewport_states,
    const CopyModeState& copy_mode,
    const RenderStatus& status);

}  // namespace wmux::daemon_internal
