#include "wmux/session_manager.hpp"

#include <utility>

namespace wmux {

std::optional<std::size_t> SessionManager::find_index(std::string_view name) const {
  for (std::size_t i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i].name == name) {
      return i;
    }
  }

  return std::nullopt;
}

SessionOperationResult SessionManager::create_session(std::string name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  if (find_index(name)) {
    return {false, SessionError::DuplicateName};
  }

  SessionSummary session;
  session.id = next_id_++;
  session.name = std::move(name);
  session.created_at = std::chrono::system_clock::now();
  sessions_.push_back(std::move(session));

  return {true, SessionError::None};
}

SessionOperationResult SessionManager::rename_session(
    std::string_view current_name,
    std::string new_name) {
  if (current_name.empty() || new_name.empty()) {
    return {false, SessionError::EmptyName};
  }

  const auto current_index = find_index(current_name);
  if (!current_index) {
    return {false, SessionError::NotFound};
  }

  if (current_name != new_name && find_index(new_name)) {
    return {false, SessionError::DuplicateName};
  }

  sessions_[*current_index].name = std::move(new_name);
  return {true, SessionError::None};
}

SessionOperationResult SessionManager::kill_session(std::string_view name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  const auto index = find_index(name);
  if (!index) {
    return {false, SessionError::NotFound};
  }

  sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(*index));
  return {true, SessionError::None};
}

bool SessionManager::has_session(std::string_view name) const {
  return find_index(name).has_value();
}

std::vector<SessionSummary> SessionManager::list_sessions() const {
  return sessions_;
}

}  // namespace wmux
