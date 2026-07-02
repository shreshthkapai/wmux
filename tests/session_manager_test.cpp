#include "wmux/session_manager.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
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

bool nearly_equal(double left, double right) {
  return std::abs(left - right) < 0.000001;
}

std::uint32_t next_property_value(std::uint32_t& state) {
  state = (state * 1664525U) + 1013904223U;
  return state;
}

void collect_leaf_ids_and_assert_node_invariants(
    const wmux::PaneNode& node,
    std::vector<wmux::PaneId>& leaf_ids) {
  assert(node.node_id != 0);
  if (node.kind == wmux::PaneNode::Kind::Leaf) {
    assert(node.pane_id != 0);
    assert(node.children.empty());
    assert(node.child_weights.empty());
    leaf_ids.push_back(node.pane_id);
    return;
  }

  assert(node.children.size() >= 2);
  assert(node.child_weights.size() == node.children.size());
  for (const auto weight : node.child_weights) {
    assert(std::isfinite(weight));
    assert(weight > 0.0);
  }

  for (const auto& child : node.children) {
    assert(!(child.kind == wmux::PaneNode::Kind::Split && child.direction == node.direction));
    collect_leaf_ids_and_assert_node_invariants(child, leaf_ids);
  }
}

void assert_window_layout_invariants(const wmux::WindowSummary& window) {
  std::vector<wmux::PaneId> leaf_ids;
  collect_leaf_ids_and_assert_node_invariants(window.pane_tree, leaf_ids);

  std::vector<wmux::PaneId> pane_ids;
  pane_ids.reserve(window.panes.size());
  for (const auto& pane : window.panes) {
    pane_ids.push_back(pane.id);
  }

  std::sort(leaf_ids.begin(), leaf_ids.end());
  std::sort(pane_ids.begin(), pane_ids.end());
  assert(leaf_ids == pane_ids);
  assert(std::find(pane_ids.begin(), pane_ids.end(), window.active_pane_id) != pane_ids.end());
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
  assert_window_layout_invariants(listed[0].windows[0]);
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
  assert(windows[1].session_id == created_session.id);
  assert(windows[1].index == 1);
  assert(windows[1].active_pane_id == 2);
  assert(windows[1].panes.size() == 1);
  assert(windows[1].panes[0].window_id == windows[1].id);
}

void exposes_stable_model_ids_in_summaries() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.create_window(created_session.id, "logs").ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  const auto listed = sessions.list_sessions();
  assert(listed.size() == 1);
  assert(listed[0].id == created_session.id);
  assert(listed[0].active_window_id == 2);
  assert(listed[0].windows.size() == 2);
  assert(listed[0].windows[0].id == created_session.window_id);
  assert(listed[0].windows[0].session_id == created_session.id);
  assert(listed[0].windows[0].index == 0);
  assert(listed[0].windows[1].id == 2);
  assert(listed[0].windows[1].session_id == created_session.id);
  assert(listed[0].windows[1].index == 1);
  assert(listed[0].windows[1].panes.size() == 2);
  for (const auto& pane : listed[0].windows[1].panes) {
    assert(pane.window_id == listed[0].windows[1].id);
  }
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

void selects_window_by_stable_id() {
  wmux::SessionManager sessions;
  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto logs = sessions.create_window(created_session.id, "logs");
  assert(logs.ok);
  const auto agents = sessions.create_window(created_session.id, "agents");
  assert(agents.ok);
  assert(sessions.active_window_id(created_session.id) == agents.window_id);

  const auto selected = sessions.select_window(created_session.id, logs.window_id);
  assert(selected.ok);
  assert(selected.window_id == logs.window_id);
  assert(sessions.active_window_id(created_session.id) == logs.window_id);

  const auto missing = sessions.select_window(created_session.id, 404);
  assert(!missing.ok);
  assert(missing.error == wmux::WindowError::WindowNotFound);
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
  assert(windows[0].index == 0);
  assert(windows[1].index == 1);
}

void keeps_active_ids_valid_after_window_and_pane_kills() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.create_window(created_session.id, "logs").ok);
  assert(sessions.create_window(created_session.id, "agents").ok);

  const auto killed_window = sessions.kill_active_window(created_session.id);
  assert(killed_window.ok);
  assert(sessions.active_window_id(created_session.id) == killed_window.window_id);
  assert(sessions.active_pane_id(created_session.id) == killed_window.pane_id);
  auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 2);
  assert(std::ranges::any_of(windows, [&](const auto& window) {
    return window.id == killed_window.window_id &&
           window.active_pane_id == killed_window.pane_id;
  }));

  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  const auto killed_pane = sessions.kill_active_pane(created_session.id);
  assert(killed_pane.ok);
  assert(sessions.active_window_id(created_session.id) == killed_pane.window_id);
  assert(sessions.active_pane_id(created_session.id) == killed_pane.pane_id);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert(active->id == killed_pane.window_id);
  assert(active->active_pane_id == killed_pane.pane_id);
  assert(std::ranges::any_of(active->panes, [&](const auto& pane) {
    return pane.id == killed_pane.pane_id && pane.window_id == active->id;
  }));
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
  assert(windows[0].pane_tree.children.size() == 2);
  assert(windows[0].pane_tree.child_weights.size() == 2);
  assert(windows[0].pane_tree.children[0].kind == wmux::PaneNode::Kind::Leaf);
  assert(windows[0].pane_tree.children[0].pane_id == 1);
  assert(windows[0].pane_tree.children[1].kind == wmux::PaneNode::Kind::Leaf);
  assert(windows[0].pane_tree.children[1].pane_id == 2);
}

void merges_same_direction_splits_into_parent() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  const auto split = sessions.split_active_pane(
      created_session.id,
      wmux::SplitDirection::Horizontal);

  assert(split.ok);
  assert(split.pane_id == 3);
  assert(sessions.active_pane_id(created_session.id) == 3);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 1);
  const auto& root = windows[0].pane_tree;
  assert_window_layout_invariants(windows[0]);
  assert(root.kind == wmux::PaneNode::Kind::Split);
  assert(root.direction == wmux::SplitDirection::Horizontal);
  assert(root.children.size() == 3);
  assert(root.child_weights.size() == 3);
  assert(nearly_equal(root.child_weights[0], 0.5));
  assert(nearly_equal(root.child_weights[1], 0.25));
  assert(nearly_equal(root.child_weights[2], 0.25));
  assert(root.children[0].kind == wmux::PaneNode::Kind::Leaf);
  assert(root.children[0].pane_id == 1);
  assert(root.children[1].kind == wmux::PaneNode::Kind::Leaf);
  assert(root.children[1].pane_id == 2);
  assert(root.children[2].kind == wmux::PaneNode::Kind::Leaf);
  assert(root.children[2].pane_id == 3);

  const auto rects = wmux::compute_pane_layout_rects(root, 120, 30);
  assert(rects.size() == 3);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 60);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 60);
  assert(rects[1].width == 30);
  assert(rects[2].pane_id == 3);
  assert(rects[2].left == 90);
  assert(rects[2].width == 30);
  assert_layout_covers_without_overlap(rects, 120, 30);
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
  assert_window_layout_invariants(windows[0]);
  assert(windows[0].pane_tree.kind == wmux::PaneNode::Kind::Split);
  assert(windows[0].pane_tree.children.size() == 2);
  assert(windows[0].pane_tree.children[1].kind == wmux::PaneNode::Kind::Split);
  assert(windows[0].pane_tree.children[1].direction == wmux::SplitDirection::Vertical);
  assert(windows[0].pane_tree.children[1].children.size() == 2);
  assert(windows[0].pane_tree.children[1].children[0].pane_id == 2);
  assert(windows[0].pane_tree.children[1].children[1].pane_id == 3);
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

void selects_across_same_direction_sibling_panes() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.active_pane_id(created_session.id) == 4);

  auto selected = sessions.select_pane(
      created_session.id,
      wmux::PaneDirection::Left,
      120,
      29);
  assert(selected.ok);
  assert(selected.pane_id == 3);

  selected = sessions.select_pane(created_session.id, wmux::PaneDirection::Left, 120, 29);
  assert(selected.ok);
  assert(selected.pane_id == 2);

  selected = sessions.select_pane(created_session.id, wmux::PaneDirection::Right, 120, 29);
  assert(selected.ok);
  assert(selected.pane_id == 3);
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
  assert_window_layout_invariants(*active);
  assert(active->panes.size() == 2);
  assert(active->active_pane_id == 2);
  assert(active->pane_tree.kind == wmux::PaneNode::Kind::Split);
  assert(active->pane_tree.children.size() == 2);
  assert(active->pane_tree.children[1].kind == wmux::PaneNode::Kind::Leaf);
  assert(active->pane_tree.children[1].pane_id == 2);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 2);
  assert_layout_covers_without_overlap(rects, 120, 29);
}

void kills_pane_from_same_direction_parent_without_nesting() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.active_pane_id(created_session.id) == 4);

  const auto killed = sessions.kill_active_pane(created_session.id);

  assert(killed.ok);
  assert(killed.removed_pane_id == 4);
  assert(killed.pane_id == 3);
  assert(sessions.active_pane_id(created_session.id) == 3);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert_window_layout_invariants(*active);
  assert(active->panes.size() == 3);
  const auto& root = active->pane_tree;
  assert(root.kind == wmux::PaneNode::Kind::Split);
  assert(root.direction == wmux::SplitDirection::Horizontal);
  assert(root.children.size() == 3);
  assert(root.child_weights.size() == 3);
  assert(root.children[0].kind == wmux::PaneNode::Kind::Leaf);
  assert(root.children[0].pane_id == 1);
  assert(root.children[1].kind == wmux::PaneNode::Kind::Leaf);
  assert(root.children[1].pane_id == 2);
  assert(root.children[2].kind == wmux::PaneNode::Kind::Leaf);
  assert(root.children[2].pane_id == 3);
  const auto rects = wmux::compute_pane_layout_rects(root, 120, 29);
  assert(rects.size() == 3);
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
  root.child_weights = {42.0, 1.0};
  root.children.emplace_back(1);
  root.children.emplace_back(2);

  const auto rects = wmux::compute_pane_layout_rects(root, 10, 5);

  assert(rects.size() == 2);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 9);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 9);
  assert(rects[1].width == 1);
  assert_layout_covers_without_overlap(rects, 10, 5);
}

void uses_largest_remainder_for_integer_layout() {
  wmux::PaneNode root;
  root.kind = wmux::PaneNode::Kind::Split;
  root.direction = wmux::SplitDirection::Horizontal;
  root.child_weights = {1.0, 1.0, 1.0};
  root.children.emplace_back(1);
  root.children.emplace_back(2);
  root.children.emplace_back(3);

  const auto rects = wmux::compute_pane_layout_rects(root, 10, 5);

  assert(rects.size() == 3);
  assert(rects[0].pane_id == 1);
  assert(rects[0].left == 0);
  assert(rects[0].width == 4);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 4);
  assert(rects[1].width == 3);
  assert(rects[2].pane_id == 3);
  assert(rects[2].left == 7);
  assert(rects[2].width == 3);
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
  assert(target->split_node_id == active->pane_tree.node_id);
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
  assert(target->split_node_id == active->pane_tree.children[1].node_id);
  assert(target->path.size() == 1);
  assert(target->path[0] == 1);
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

void finds_same_direction_sibling_resize_targets() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto first_boundary =
      wmux::find_pane_split_resize_target(active->pane_tree, 60, 5, 120, 29);
  assert(first_boundary);
  assert(first_boundary->path.empty());
  assert(first_boundary->child_index == 0);
  assert(first_boundary->direction == wmux::SplitDirection::Horizontal);

  const auto second_boundary =
      wmux::find_pane_split_resize_target(active->pane_tree, 90, 5, 120, 29);
  assert(second_boundary);
  assert(second_boundary->path.empty());
  assert(second_boundary->child_index == 1);
  assert(second_boundary->direction == wmux::SplitDirection::Horizontal);
}

void resizes_same_direction_sibling_boundary() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto target =
      wmux::find_pane_split_resize_target(active->pane_tree, 90, 5, 120, 29);
  assert(target);
  assert(target->child_index == 1);

  const auto resized = sessions.resize_active_window_split(created_session.id, *target, 100, 5);
  assert(resized.ok);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 3);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 60);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 60);
  assert(rects[1].width == 40);
  assert(rects[2].pane_id == 3);
  assert(rects[2].left == 100);
  assert(rects[2].width == 20);
  assert_layout_covers_without_overlap(rects, 120, 29);
}

void keyboard_resize_adjusts_active_pane_like_tmux_keys() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  auto resized = sessions.resize_active_pane(
      created_session.id,
      wmux::PaneDirection::Left,
      10,
      120,
      29);
  assert(resized.ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects.size() == 2);
  assert(rects[0].width == 50);
  assert(rects[1].left == 50);
  assert(rects[1].width == 70);
  assert_layout_covers_without_overlap(rects, 120, 29);

  resized = sessions.resize_active_pane(
      created_session.id,
      wmux::PaneDirection::Right,
      5,
      120,
      29);
  assert(resized.ok);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 29);
  assert(rects[0].width == 55);
  assert(rects[1].left == 55);
  assert(rects[1].width == 65);
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

void equalizes_nearest_parent_first() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto root_target =
      wmux::find_pane_split_resize_target(active->pane_tree, 60, 5, 120, 30);
  assert(root_target);
  assert(root_target->path.empty());
  assert(sessions.resize_active_window_split(created_session.id, *root_target, 80, 5).ok);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto nested_target =
      wmux::find_pane_split_resize_target(active->pane_tree, 90, 15, 120, 30);
  assert(nested_target);
  assert(nested_target->path.size() == 1);
  assert(sessions.resize_active_window_split(created_session.id, *nested_target, 90, 22).ok);

  const auto equalized = sessions.equalize_active_window_panes(created_session.id);
  assert(equalized.ok);
  assert(equalized.changed);
  assert(equalized.window_id == created_session.window_id);
  assert(equalized.pane_id == 3);
  assert(sessions.active_pane_id(created_session.id) == 3);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert_window_layout_invariants(*active);
  assert(active->pane_tree.kind == wmux::PaneNode::Kind::Split);
  assert(active->pane_tree.direction == wmux::SplitDirection::Horizontal);
  assert(active->pane_tree.children.size() == 2);
  assert(active->pane_tree.children[1].kind == wmux::PaneNode::Kind::Split);
  assert(active->pane_tree.children[1].direction == wmux::SplitDirection::Vertical);

  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 30);
  assert(rects.size() == 3);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 80);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 80);
  assert(rects[1].width == 40);
  assert(rects[1].height == 15);
  assert(rects[2].pane_id == 3);
  assert(rects[2].left == 80);
  assert(rects[2].width == 40);
  assert(rects[2].top == 15);
  assert(rects[2].height == 15);
  assert_layout_covers_without_overlap(rects, 120, 30);
}

void equalize_climbs_when_nearest_parent_is_even() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  const auto root_target =
      wmux::find_pane_split_resize_target(active->pane_tree, 60, 5, 120, 30);
  assert(root_target);
  assert(root_target->path.empty());
  assert(sessions.resize_active_window_split(created_session.id, *root_target, 80, 5).ok);

  const auto equalized = sessions.equalize_active_window_panes(created_session.id);
  assert(equalized.ok);
  assert(equalized.changed);
  assert(equalized.pane_id == 3);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert_window_layout_invariants(*active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 30);
  assert(rects.size() == 3);
  assert(rects[0].pane_id == 1);
  assert(rects[0].width == 60);
  assert(rects[1].pane_id == 2);
  assert(rects[1].left == 60);
  assert(rects[1].width == 60);
  assert(rects[1].height == 15);
  assert(rects[2].pane_id == 3);
  assert(rects[2].left == 60);
  assert(rects[2].width == 60);
  assert(rects[2].top == 15);
  assert(rects[2].height == 15);
  assert_layout_covers_without_overlap(rects, 120, 30);
}

void equalizes_repeated_splits_to_even_pane_sizes() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);

  auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 40);
  assert(rects.size() == 4);
  assert(rects[0].height == 20);
  assert(rects[1].height == 10);
  assert(rects[2].height == 5);
  assert(rects[3].height == 5);

  const auto equalized = sessions.equalize_active_window_panes(created_session.id);
  assert(equalized.ok);
  assert(equalized.changed);

  active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert_window_layout_invariants(*active);
  rects = wmux::compute_pane_layout_rects(active->pane_tree, 120, 40);
  assert(rects.size() == 4);
  assert(rects[0].height == 10);
  assert(rects[1].height == 10);
  assert(rects[2].height == 10);
  assert(rects[3].height == 10);
  assert_layout_covers_without_overlap(rects, 120, 40);
}

void reports_equalize_when_panes_are_already_even() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);

  const auto equalized = sessions.equalize_active_window_panes(created_session.id);
  assert(equalized.ok);
  assert(!equalized.changed);
  assert(equalized.pane_id == 2);
}

void rejects_equalize_without_splits() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto equalized = sessions.equalize_active_window_panes(created_session.id);

  assert(!equalized.ok);
  assert(equalized.error == wmux::PaneError::NoSplit);
  assert(sessions.active_pane_id(created_session.id) == created_session.pane_id);
}

void preserves_layout_invariants_through_split_kill_equalize_sequence() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Horizontal).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.select_pane(created_session.id, wmux::PaneDirection::Left, 120, 30).ok);
  assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
  assert(sessions.equalize_active_window_panes(created_session.id).ok);
  assert(sessions.kill_active_pane(created_session.id).ok);
  assert(sessions.equalize_active_window_panes(created_session.id).ok);

  const auto active = sessions.active_window_summary(created_session.id);
  assert(active);
  assert_window_layout_invariants(*active);
  const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, 127, 31);
  assert(rects.size() == active->panes.size());
  assert_layout_covers_without_overlap(rects, 127, 31);
}

void property_layout_invariants_survive_random_operations() {
  wmux::SessionManager sessions;
  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);

  std::uint32_t rng = 0x5EED1234U;
  for (int step = 0; step < 240; ++step) {
    auto active = sessions.active_window_summary(created_session.id);
    assert(active);
    assert_window_layout_invariants(*active);

    const int columns = 80 + static_cast<int>(next_property_value(rng) % 81U);
    const int rows = 24 + static_cast<int>(next_property_value(rng) % 33U);
    const auto operation = next_property_value(rng) % 6U;

    if (operation == 0U && active->panes.size() < 14) {
      const auto direction =
          (next_property_value(rng) % 2U) == 0U ? wmux::SplitDirection::Horizontal
                                                : wmux::SplitDirection::Vertical;
      assert(sessions.split_active_pane(created_session.id, direction).ok);
    } else if (operation == 1U && active->panes.size() > 1) {
      assert(sessions.kill_active_pane(created_session.id).ok);
    } else if (operation == 2U) {
      const auto equalized = sessions.equalize_active_window_panes(created_session.id);
      assert(equalized.ok || equalized.error == wmux::PaneError::NoSplit);
    } else if (operation == 3U) {
      const auto direction =
          static_cast<wmux::PaneDirection>(next_property_value(rng) % 4U);
      const auto selected = sessions.select_pane(created_session.id, direction, columns, rows);
      assert(selected.ok || selected.error == wmux::PaneError::PaneNotFound);
    } else if (operation == 4U) {
      const auto current = sessions.active_window_summary(created_session.id);
      assert(current);
      std::optional<wmux::PaneSplitResizeTarget> target;
      for (int row = 0; row < rows && !target; ++row) {
        for (int column = 0; column < columns && !target; ++column) {
          target = wmux::find_pane_split_resize_target(
              current->pane_tree,
              column,
              row,
              columns,
              rows);
        }
      }
      if (target) {
        const int column = static_cast<int>(next_property_value(rng) % static_cast<std::uint32_t>(columns));
        const int row = static_cast<int>(next_property_value(rng) % static_cast<std::uint32_t>(rows));
        const auto resized =
            sessions.resize_active_window_split(created_session.id, *target, column, row);
        assert(resized.ok || resized.error == wmux::PaneError::PaneNotFound);
      }
    } else if (active->panes.size() < 14) {
      assert(sessions.split_active_pane(created_session.id, wmux::SplitDirection::Vertical).ok);
    }

    active = sessions.active_window_summary(created_session.id);
    assert(active);
    assert_window_layout_invariants(*active);
    const auto rects = wmux::compute_pane_layout_rects(active->pane_tree, columns, rows);
    assert(rects.size() == active->panes.size());
    assert_layout_covers_without_overlap(rects, columns, rows);
  }
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
  exposes_stable_model_ids_in_summaries();
  renames_active_window();
  rejects_duplicate_window_names();
  selects_next_and_previous_window();
  selects_window_by_stable_id();
  kills_active_window_without_killing_session();
  keeps_active_ids_valid_after_window_and_pane_kills();
  rejects_killing_last_window();
  splits_active_pane();
  merges_same_direction_splits_into_parent();
  nests_pane_splits_under_active_leaf();
  rejects_split_for_missing_session();
  selects_adjacent_panes();
  selects_nested_adjacent_panes();
  selects_across_same_direction_sibling_panes();
  selects_pane_by_id();
  rejects_missing_pane_selection_by_id();
  rejects_pane_selection_without_neighbor();
  kills_active_pane_and_collapses_layout();
  kills_pane_from_same_direction_parent_without_nesting();
  rejects_killing_last_pane();
  returns_active_window_summary();
  computes_integer_pane_layout_rects();
  computes_nested_layout_without_gaps_or_overlaps();
  clamps_extreme_split_ratios_for_layout();
  uses_largest_remainder_for_integer_layout();
  handles_tiny_layout_without_invalid_rectangles();
  finds_horizontal_split_resize_target();
  resizes_horizontal_split_ratio();
  resizes_nested_vertical_split_ratio();
  finds_same_direction_sibling_resize_targets();
  resizes_same_direction_sibling_boundary();
  keyboard_resize_adjusts_active_pane_like_tmux_keys();
  rejects_resize_target_away_from_split_border();
  clamps_split_resize_ratio();
  equalizes_nearest_parent_first();
  equalize_climbs_when_nearest_parent_is_even();
  equalizes_repeated_splits_to_even_pane_sizes();
  reports_equalize_when_panes_are_already_even();
  rejects_equalize_without_splits();
  preserves_layout_invariants_through_split_kill_equalize_sequence();
  property_layout_invariants_survive_random_operations();
  resolves_only_session_id();
  rejects_empty_names();
}
