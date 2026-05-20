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
  assert(sessions.has_session("finance"));
  assert(sessions.session_id_for_name("finance") == 1);
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

void rejects_empty_names() {
  wmux::SessionManager sessions;

  const auto created = sessions.create_session("");
  const auto renamed = sessions.rename_session("", "trading");
  const auto killed = sessions.kill_session("");

  assert(!created.ok);
  assert(created.error == wmux::SessionError::EmptyName);
  assert(!renamed.ok);
  assert(renamed.error == wmux::SessionError::EmptyName);
  assert(!killed.ok);
  assert(killed.error == wmux::SessionError::EmptyName);
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
  rejects_empty_names();
}
