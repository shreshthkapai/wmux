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

  const auto listed = sessions.list_sessions();
  assert(listed.size() == 1);
  assert(listed[0].id == 1);
  assert(listed[0].name == "finance");
  assert(listed[0].active_window_id == 1);
  assert(listed[0].windows.size() == 1);
  assert(listed[0].windows[0].id == 1);
  assert(listed[0].windows[0].name == "0");
  assert(sessions.has_session("finance"));
  assert(sessions.session_id_for_name("finance") == 1);
  assert(sessions.active_window_id(1) == 1);
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
  assert(sessions.active_window_id(created_session.id) == 2);

  const auto windows = sessions.list_windows(created_session.id);
  assert(windows.size() == 2);
  assert(windows[0].name == "0");
  assert(windows[1].name == "logs");
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
  resolves_only_session_id();
  rejects_empty_names();
}
