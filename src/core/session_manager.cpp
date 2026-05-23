#include "wmux/session_manager.hpp"

#include <algorithm>
#include <utility>

namespace wmux {
namespace {

bool contains_window_name(const SessionSummary& session, std::string_view name) {
  return std::any_of(session.windows.begin(), session.windows.end(), [&](const auto& window) {
    return window.name == name;
  });
}

WindowOperationResult missing_session(SessionId session_id) {
  return {false, WindowError::SessionNotFound, session_id};
}

}  // namespace

SessionOperationResult SessionManager::create_session(std::string name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  if (name_index_.contains(name)) {
    return {false, SessionError::DuplicateName};
  }

  SessionSummary session;
  session.id = next_id_++;
  session.name = std::move(name);
  session.created_at = std::chrono::system_clock::now();

  WindowSummary initial_window;
  initial_window.id = next_window_id_++;
  initial_window.name = "0";
  initial_window.created_at = session.created_at;
  session.active_window_id = initial_window.id;
  session.windows.push_back(std::move(initial_window));

  const auto id = session.id;
  const auto window_id = session.active_window_id;
  name_index_.emplace(session.name, id);
  sessions_.emplace(id, std::move(session));
  order_.push_back(id);

  return {true, SessionError::None, id, window_id};
}

SessionOperationResult SessionManager::rename_session(
    std::string_view current_name,
    std::string new_name) {
  if (current_name.empty() || new_name.empty()) {
    return {false, SessionError::EmptyName};
  }

  const auto id = session_id_for_name(current_name);
  if (!id) {
    return {false, SessionError::NotFound};
  }

  const auto session = sessions_.find(*id);
  if (session == sessions_.end()) {
    return {false, SessionError::NotFound};
  }

  if (session->second.name != new_name && name_index_.contains(new_name)) {
    return {false, SessionError::DuplicateName};
  }

  if (session->second.name != new_name) {
    name_index_.erase(session->second.name);
    session->second.name = std::move(new_name);
    name_index_.emplace(session->second.name, *id);
  }

  return {true, SessionError::None, *id};
}

SessionOperationResult SessionManager::kill_session(std::string_view name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  const auto id = session_id_for_name(name);
  if (!id) {
    return {false, SessionError::NotFound};
  }

  const auto session = sessions_.find(*id);
  if (session != sessions_.end()) {
    name_index_.erase(session->second.name);
    sessions_.erase(session);
  }

  order_.erase(std::remove(order_.begin(), order_.end(), *id), order_.end());
  return {true, SessionError::None, *id};
}

WindowOperationResult SessionManager::create_window(SessionId session_id, std::string name) {
  if (name.empty()) {
    return {false, WindowError::EmptyName, session_id};
  }

  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (contains_window_name(session->second, name)) {
    return {false, WindowError::DuplicateName, session_id};
  }

  WindowSummary window;
  window.id = next_window_id_++;
  window.name = std::move(name);
  window.created_at = std::chrono::system_clock::now();

  const auto window_id = window.id;
  session->second.windows.push_back(std::move(window));
  session->second.active_window_id = window_id;
  return {true, WindowError::None, session_id, window_id};
}

WindowOperationResult SessionManager::rename_active_window(
    SessionId session_id,
    std::string name) {
  if (name.empty()) {
    return {false, WindowError::EmptyName, session_id};
  }

  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  const auto active_id = session->second.active_window_id;
  auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == active_id; });
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (active->name != name && contains_window_name(session->second, name)) {
    return {false, WindowError::DuplicateName, session_id};
  }

  active->name = std::move(name);
  return {true, WindowError::None, session_id, active_id};
}

WindowOperationResult SessionManager::select_next_window(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == session->second.active_window_id; });
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  ++active;
  if (active == session->second.windows.end()) {
    active = session->second.windows.begin();
  }
  session->second.active_window_id = active->id;
  return {true, WindowError::None, session_id, active->id};
}

WindowOperationResult SessionManager::select_previous_window(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == session->second.active_window_id; });
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (active == session->second.windows.begin()) {
    active = session->second.windows.end();
  }
  --active;
  session->second.active_window_id = active->id;
  return {true, WindowError::None, session_id, active->id};
}

bool SessionManager::has_session(std::string_view name) const {
  return session_id_for_name(name).has_value();
}

std::optional<SessionId> SessionManager::session_id_for_name(std::string_view name) const {
  const auto found = name_index_.find(std::string{name});
  if (found == name_index_.end()) {
    return std::nullopt;
  }

  return found->second;
}

std::optional<SessionId> SessionManager::only_session_id() const {
  if (order_.size() != 1) {
    return std::nullopt;
  }

  const auto session = sessions_.find(order_.front());
  if (session == sessions_.end()) {
    return std::nullopt;
  }

  return session->first;
}

std::optional<WindowId> SessionManager::active_window_id(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end() || session->second.active_window_id == 0) {
    return std::nullopt;
  }

  return session->second.active_window_id;
}

std::size_t SessionManager::session_count() const {
  return sessions_.size();
}

std::vector<SessionSummary> SessionManager::list_sessions() const {
  std::vector<SessionSummary> listed;
  listed.reserve(order_.size());
  for (const auto id : order_) {
    const auto session = sessions_.find(id);
    if (session != sessions_.end()) {
      listed.push_back(session->second);
    }
  }

  return listed;
}

std::vector<WindowSummary> SessionManager::list_windows(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return {};
  }

  return session->second.windows;
}

}  // namespace wmux
