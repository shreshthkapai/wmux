#include "wmux/session_manager.hpp"

#include <algorithm>
#include <utility>

namespace wmux {

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

  const auto id = session.id;
  name_index_.emplace(session.name, id);
  sessions_.emplace(id, std::move(session));
  order_.push_back(id);

  return {true, SessionError::None, id};
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

}  // namespace wmux
