#include "wmux/session_manager.hpp"

#include <cassert>
#include <string>

namespace {

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

void rejects_pane_selection_without_neighbor() {
  wmux::SessionManager sessions;

  const auto created_session = sessions.create_session("finance");
  assert(created_session.ok);
  const auto selected = sessions.select_pane(created_session.id, wmux::PaneDirection::Left);

  assert(!selected.ok);
  assert(selected.error == wmux::PaneError::PaneNotFound);
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
  splits_active_pane();
  nests_pane_splits_under_active_leaf();
  rejects_split_for_missing_session();
  selects_adjacent_panes();
  selects_nested_adjacent_panes();
  rejects_pane_selection_without_neighbor();
  returns_active_window_summary();
  computes_integer_pane_layout_rects();
  resolves_only_session_id();
  rejects_empty_names();
}
