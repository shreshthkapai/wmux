#include "wmux/session_manager.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace {

void assert_layout_covers_without_overlap(
    const std::vector<wmux::PaneLayoutRect>& rects,
    int columns,
    int rows) {
  std::vector<bool> occupied(static_cast<std::size_t>(columns * rows), false);

  for (const auto& rect : rects) {
    assert(rect.left >= 0);
    assert(rect.top >= 0);
    assert(rect.width > 0);
    assert(rect.height > 0);
    assert(rect.left + rect.width <= columns);
    assert(rect.top + rect.height <= rows);

    for (int row = rect.top; row < rect.top + rect.height; ++row) {
      for (int column = rect.left; column < rect.left + rect.width; ++column) {
        const auto offset = static_cast<std::size_t>((row * columns) + column);
        assert(!occupied[offset]);
        occupied[offset] = true;
      }
    }
  }

  for (const bool cell : occupied) {
    assert(cell);
  }
}

void expects_create_and_list_sessions() {
  wmux::SessionManager sessions;

  const auto created = sessions.create_session("finance");
  assert(created.ok);
  assert(created.error == wmux::SessionError::None);
  assert(created.id == 1);
  assert(created.window_id == 1);
  assert(created.pane_id == 1);

  const auto listed = sessions.list_sessions();
  assert(listed.size() == 1);
  assert(listed[0].id == 1);
  assert(listed[0].name == "finance");
  assert(listed[0].active_window_id == 1);
  assert(listed[0].windows.size() == 1);
  assert(listed[0].windows[0].id == 1);
  assert(listed[0].windows[0].name == "0");
  assert(listed[0].windows[0].active_pane_id == 1);
  assert(listed[0].windows[0].panes.size() == 1);
  assert(listed[0].windows[0].panes[0].id == 1);
  assert(listed[0].windows[0].pane_tree.kind == wmux::PaneNode::Kind::Leaf);
  assert(listed[0].windows[0].pane_tree.pane_id == 1);
  assert(sessions.has_session("finance"));
  assert(sessions.session_id_for_name("finance") == 1);
  assert(sessions.active_window_id(1) == 1);
  assert(sessions.active_pane_id(1) == 1);
  assert(sessions.session_count() == 1);
}

void rejects_duplicate_session_names() {
  wmux::SessionManager sessions;

  assert(sessions.create_session("finance").ok);
  const auto duplicate = sessions.create_session("finance");

  assert(!duplicate.ok);
  assert(duplicate.error == wmux::SessionError::DuplicateName);
  assert(sessions.list_sessions().size() == 1);
}

void renames_existing_session() {
  wmux::SessionManager sessions;

  assert(sessions.create_session("finance").ok);
  const auto renamed = sessions.rename_session("finance", "trading");

  assert(renamed.ok);
  assert(renamed.id == 1);
  assert(!sessions.has_session("finance"));
  assert(sessions.has_session("trading"));
  assert(sessions.session_id_for_name("trading") == 1);
}

void keeps_session_id_stable_across_rename() {
  wmux::SessionManager sessions;

  const auto created = sessions.create_session("finance");
  assert(created.ok);
  const auto renamed = sessions.rename_session("finance", "trading");

  assert(renamed.ok);
  assert(renamed.id == created.id);
  const auto listed = sessions.list_sessions();
  assert(listed.size() == 1);
  assert(listed[0].id == created.id);
  assert(listed[0].name == "trading");
}

void rejects_rename_to_existing_name() {
  wmux::SessionManager sessions;

  assert(sessions.create_session("finance").ok);
  assert(sessions.create_session("trading").ok);
  const auto renamed = sessions.rename_session("finance", "trading");

  assert(!renamed.ok);
  assert(renamed.error == wmux::SessionError::DuplicateName);
  assert(sessions.has_session("finance"));
  assert(sessions.has_session("trading"));
}

void rejects_missing_session_rename() {
  wmux::SessionManager sessions;

  const auto renamed = sessions.rename_session("finance", "trading");

  assert(!renamed.ok);
  assert(renamed.error == wmux::SessionError::NotFound);
}

void kills_existing_session() {
  wmux::SessionManager sessions;

  assert(sessions.create_session("finance").ok);
  const auto killed = sessions.kill_session("finance");

  assert(killed.ok);
  assert(killed.id == 1);
  assert(sessions.list_sessions().empty());
  assert(sessions.session_count() == 0);
}

void rejects_missing_session_kill() {
  wmux::SessionManager sessions;

  const auto killed = sessions.kill_session("finance");

  assert(!killed.ok);
  assert(killed.error == wmux::SessionError::NotFound);
}

void creates_windows_inside_session() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto created_window = sessions.create_window(created_session.id, "logs");

  assert(created_window.ok);
  assert(created_window.session_id == created_session.id);
  assert(created_window.window_id == 2);
  assert(created_window.pane_id == 2);
  assert(sessions.active_window_id(created_session.id) == 2);
  assert(sessions.active_pane_id(created_session.id) == 2);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 2);
  assert(windows[0].name == "0");
  assert(windows[1].name == "logs");
  assert(windows[1].active_pane_id == 2);
  assert(windows[1].panes.size() == 1);
}

void renames_active_window() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto renamed = sessions.rename_active_window(created_session.id, "agents");

  assert(renamed.ok);
  assert(renamed.window_id == created_session.window_id);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 1);
  assert(windows[0].name == "agents");
}

void rejects_duplicate_window_names() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.create_window(created_session.id, "logs").ok);
  const auto duplicate = sessions.create_window(created_session.id, "logs");
  const auto duplicate_rename = sessions.rename_active_window(created_session.id, "0");

  assert(!duplicate.ok);
  assert(duplicate.error == wmux::WindowError::DuplicateName);
  assert(!duplicate_rename.ok);
  assert(duplicate_rename.error == wmux::WindowError::DuplicateName);
}

void selects_next_and_previous_window() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.create_window(created_session.id, "logs").ok);
  assert(sessions.create_window(created_session.id, "agents").ok);
  assert(sessions.active_window_id(created_session.id) == 3);

  const auto next = sessions.select_next_window(created_session.id);
  assert(next.ok);
  assert(next.window_id == 1);
  assert(sessions.active_window_id(created_session.id) == 1);

  const auto previous = sessions.select_previous_window(created_session.id);
  assert(previous.ok);
  assert(previous.window_id == 3);
  assert(sessions.active_window_id(created_session.id) == 3);
}

void kills_active_window_without_killing_session() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.create_window(created_session.id, "logs").ok);
  assert(sessions.create_window(created_session.id, "agents").ok);

  const auto killed = sessions.kill_active_window(created_session.id);

  assert(killed.ok);
  assert(killed.removed_window_id == 3);
  assert(killed.window_id == 2);
  assert(sessions.session_count() == 1);
  assert(sessions.active_window_id(created_session.id) == 2);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 2);
  assert(windows[0].name == "0");
  assert(windows[1].name == "logs");
}

void rejects_killing_last_window() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto killed = sessions.kill_active_window(created_session.id);

  assert(!killed.ok);
  assert(killed.error == wmux::WindowError::LastWindow);
  assert(sessions.session_count() == 1);
  assert(sessions.active_window_id(created_session.id) == created_session.window_id);
}

void splits_active_pane() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto split = sessions.split_active_pane(
      created_session.id,
      wmux::SplitDirection::Horizontal);

  assert(split.ok);
  assert(split.window_id == created_session.window_id);
  assert(split.pane_id == 2);
  assert(sessions.active_pane_id(created_session.id) == 2);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 1);
  assert(windows[0].panes.size() == 2);
  assert(windows[0].active_pane_id == 2);
  assert(windows[0].pane_tree.kind == wmux::PaneNode::Kind::Split);
  assert(windows[0].pane_tree.direction == wmux::SplitDirection::Horizontal);
  assert(windows[0].pane_tree.first);
  assert(windows[0].pane_tree.second);
  assert(windows[0].pane_tree.first->kind == wmux::PaneNode::Kind::Leaf);
  assert(windows[0].pane_tree.first->pane_id == 1);
  assert(windows[0].pane_tree.second->kind == wmux::PaneNode::Kind::Leaf);
  assert(windows[0].pane_tree.second->pane_id == 2);
}

void nests_pane_splits_under_active_leaf() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  const auto second_split = sessions.split_active_pane(
      created_session.id,
      wmux::SplitDirection::Vertical);

  assert(second_split.ok);
  assert(second_split.pane_id == 3);
  assert(sessions.active_pane_id(created_session.id) == 3);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 1);
  assert(windows[0].panes.size() == 3);
  assert(windows[0].pane_tree.kind == wmux::PaneNode::Kind::Split);
  assert(windows[0].pane_tree.second);
  assert(windows[0].pane_tree.second->kind == wmux::PaneNode::Kind::Split);
  assert(windows[0].pane_tree.second->direction == wmux::SplitDirection::Vertical);
  assert(windows[0].pane_tree.second->first->pane_id == 2);
  assert(windows[0].pane_tree.second->second->pane_id == 3);
}

void rejects_split_for_missing_session() {
  wmux::SessionManager sessions;

  const auto split = sessions.split_active_pane(42, wmux::SplitDirection::Horizontal);

  assert(!split.ok);
  assert(split.error == wmux::PaneError::SessionNotFound);
}

void selects_adjacent_panes() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.active_pane_id(created_session.id) == 2);

  const auto left = sessions.select_pane(created_session.id, wmux::PaneDirection::Left);
  assert(left.ok);
  assert(left.pane_id == 1);
  assert(sessions.active_pane_id(created_session.id) == 1);

  const auto right = sessions.select_pane(created_session.id, wmux::PaneDirection::Right);
  assert(right.ok);
  assert(right.pane_id == 2);
  assert(sessions.active_pane_id(created_session.id) == 2);
}

void selects_nested_adjacent_panes() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.active_pane_id(created_session.id) == 3);

  const auto up = sessions.select_pane(created_session.id, wmux::PaneDirection::Up);
  assert(up.ok);
  assert(up.pane_id == 2);
  assert(sessions.active_pane_id(created_session.id) == 2);

  const auto down = sessions.select_pane(created_session.id, wmux::PaneDirection::Down);
  assert(down.ok);
  assert(down.pane_id == 3);
  assert(sessions.active_pane_id(created_session.id) == 3);

  const auto left = sessions.select_pane(created_session.id, wmux::PaneDirection::Left);
  assert(left.ok);
  assert(left.pane_id == 1);
}

void selects_pane_by_id() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.active_pane_id(created_session.id) == 2);

  const auto selected = sessions.select_pane(created_session.id, created_session.pane_id);

  assert(selected.ok);
  assert(selected.pane_id == created_session.pane_id);
  assert(sessions.active_pane_id(created_session.id) == created_session.pane_id);
}

void rejects_missing_pane_selection_by_id() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto selected = sessions.select_pane(created_session.id, 42);

  assert(!selected.ok);
  assert(selected.error == wmux::PaneError::PaneNotFound);
  assert(sessions.active_pane_id(created_session.id) == created_session.pane_id);
}

void rejects_pane_selection_without_neighbor() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto selected = sessions.select_pane(created_session.id, wmux::PaneDirection::Left);

  assert(!selected.ok);
  assert(selected.error == wmux::PaneError::PaneNotFound);
  assert(sessions.active_pane_id(created_session.id) == created_session.pane_id);
}

void kills_active_pane_and_collapses_layout() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.active_pane_id(created_session.id) == 3);

  const auto killed = sessions.kill_active_pane(created_session.id);

  assert(killed.ok);
  assert(killed.removed_pane_id == 3);
  assert(killed.pane_id == 2);
  assert(sessions.active_pane_id(created_session.id) == 2);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert(active->panes.size() == 2);
  assert(active->active_pane_id == 2);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 2);
  assert_layout_covers_without_overlap(rects, 120, 29);
}

void rejects_killing_last_pane() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto killed = sessions.kill_active_pane(created_session.id);

  assert(!killed.ok);
  assert(killed.error == wmux::PaneError::LastPane);
  assert(sessions.active_pane_id(created_session.id) == created_session.pane_id);
}

void returns_active_window_summary() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.create_window(created_session.id, "logs").ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert(active->name == "logs");
  assert(active->id == 2);
  assert(active->active_pane_id == 2);
  assert(!sessions.active_window_summary(42));
}

void computes_integer_pane_layout_rects() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 3);

  assert(rects[0].pane_id == 1);
  assert(rects[0].left == 0);
  assert(rects[0].top == 0);
  assert(rects[0].width == 60);
  assert(rects[0].height == 29);

  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 60);
  assert(rects[1].top == 0);
  assert(rects[1].width == 60);
  assert(rects[1].height == 15);

  assert(rects[2].pane_id == 3);
  assert(rects[2].left == 60);
  assert(rects[2].top == 15);
  assert(rects[2].width == 60);
  assert(rects[2].height == 14);
}

void computes_nested_layout_without_gaps_or_overlaps() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.select_pane(created_session.id, wmux::PaneDirection::Left).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 101, 37);

  assert(rects.size() == 5);
  assert_layout_covers_without_overlap(rects, 101, 37);
}

void clamps_extreme_split_ratios_for_layout() {
  wmux::PaneNode root;
  root.kind = wmux::PaneNode::Kind::Split;
  root.direction = wmux::SplitDirection::Horizontal;
  root.ratio = 42.0;
  root.first = std::make_unique<wmux::PaneNode>(1);
  root.second = std::make_unique<wmux::PaneNode>(2);

  const auto rects = wmux::compute_pane_layout_rects(root, 10, 5);

  assert(rects.size() == 2);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 9);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 9);
  assert(rects[1].width == 1);
  assert_layout_covers_without_overlap(rects, 10, 5);
}

void handles_tiny_layout_without_invalid_rectangles() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 2, 1);

  assert(!rects.empty());
  assert_layout_covers_without_overlap(rects, 2, 1);
}

void finds_horizontal_split_resize_target() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto target = wmux::find_pane_split_resize_target(active->pane_tree, 60, 5, 120, 29);

  assert(target);
  assert(target->path.empty());
  assert(target->direction == wmux::SplitDirection::Horizontal);
  assert(target->left == 0);
  assert(target->top == 0);
  assert(target->width == 120);
  assert(target->height == 29);
}

void resizes_horizontal_split_ratio() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto target = wmux::find_pane_split_resize_target(active->pane_tree, 60, 5, 120, 29);
  assert(target);

  const auto resized = sessions.resize_active_window_split(created_session.id, *target, 80, 5);
  assert(resized.ok);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 2);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 80);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 80);
  assert(rects[1].width == 40);
  assert_layout_covers_without_overlap(rects, 120, 29);
}

void resizes_nested_vertical_split_ratio() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto target = wmux::find_pane_split_resize_target(active->pane_tree, 70, 15, 120, 29);
  assert(target);
  assert(target->path.size() == 1);
  assert(target->path[0] == wmux::PaneTreePathStep::Second);
  assert(target->direction == wmux::SplitDirection::Vertical);
  assert(target->left == 60);
  assert(target->top == 0);
  assert(target->width == 60);
  assert(target->height == 29);

  const auto resized = sessions.resize_active_window_split(created_session.id, *target, 70, 22);
  assert(resized.ok);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 3);
  assert(rects[1].pane_id == 2);
  assert(rects[1].height == 22);
  assert(rects[2].pane_id == 3);
  assert(rects[2].top == 22);
  assert(rects[2].height == 7);
  assert_layout_covers_without_overlap(rects, 120, 29);
}

void rejects_resize_target_away_from_split_border() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert(!wmux::find_pane_split_resize_target(active->pane_tree, 10, 5, 120, 29));
}

void clamps_split_resize_ratio() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto target = wmux::find_pane_split_resize_target(active->pane_tree, 60, 5, 120, 29);
  assert(target);

  const auto resized = sessions.resize_active_window_split(created_session.id, *target, 0, 5);
  assert(resized.ok);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 2);
  assert(rects[0].width == 6);
  assert(rects[1].left == 6);
  assert(rects[1].width == 114);
  assert_layout_covers_without_overlap(rects, 120, 29);
}

void resolves_only_session_id() {
  wmux::SessionManager sessions;

  assert(!sessions.only_session_id());
  const auto finance = sessions.create_session("finance");
  assert(finance.ok);
  assert(sessions.only_session_id() == finance.id);
  assert(sessions.create_session("trading").ok);
  assert(!sessions.only_session_id());
}

void rejects_empty_names() {
  wmux::SessionManager sessions;

  const auto created = sessions.create_session("");
  const auto renamed = sessions.rename_session("", "trading");
  const auto killed = sessions.kill_session("");
  const auto window = sessions.create_window(42, "");

  assert(!created.ok);
  assert(created.error == wmux::SessionError::EmptyName);
  assert(!renamed.ok);
  assert(renamed.error == wmux::SessionError::EmptyName);
  assert(!killed.ok);
  assert(killed.error == wmux::SessionError::EmptyName);
  assert(!window.ok);
  assert(window.error == wmux::WindowError::EmptyName);
}

}  // namespace

void run_session_manager_tests() {
  expects_create_and_list_sessions();
  rejects_duplicate_session_names();
  renames_existing_session();
  keeps_session_id_stable_across_rename();
  rejects_rename_to_existing_name();
  rejects_missing_session_rename();
  kills_existing_session();
  rejects_missing_session_kill();
  creates_windows_inside_session();
  renames_active_window();
  rejects_duplicate_window_names();
  selects_next_and_previous_window();
  kills_active_window_without_killing_session();
  rejects_killing_last_window();
  splits_active_pane();
  nests_pane_splits_under_active_leaf();
  rejects_split_for_missing_session();
  selects_adjacent_panes();
  selects_nested_adjacent_panes();
  selects_pane_by_id();
  rejects_missing_pane_selection_by_id();
  rejects_pane_selection_without_neighbor();
  kills_active_pane_and_collapses_layout();
  rejects_killing_last_pane();
  returns_active_window_summary();
  computes_integer_pane_layout_rects();
  computes_nested_layout_without_gaps_or_overlaps();
  clamps_extreme_split_ratios_for_layout();
  handles_tiny_layout_without_invalid_rectangles();
  finds_horizontal_split_resize_target();
  resizes_horizontal_split_ratio();
  resizes_nested_vertical_split_ratio();
  rejects_resize_target_away_from_split_border();
  clamps_split_resize_ratio();
  resolves_only_session_id();
  rejects_empty_names();
}
