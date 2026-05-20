#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wmux {

using SessionId = std::uint64_t;

struct SessionSummary {
  SessionId id{0};
  std::string name;
  std::chrono::system_clock::time_point created_at;
};

enum class SessionError {
  None,
  EmptyName,
  DuplicateName,
  NotFound,
};

struct SessionOperationResult {
  bool ok{false};
  SessionError error{SessionError::None};
  SessionId id{0};
};

class SessionManager {
 public:
  SessionOperationResult create_session(std::string name);
  SessionOperationResult rename_session(std::string_view current_name, std::string new_name);
  SessionOperationResult kill_session(std::string_view name);

  bool has_session(std::string_view name) const;
  std::optional<SessionId> session_id_for_name(std::string_view name) const;
  std::size_t session_count() const;
  std::vector<SessionSummary> list_sessions() const;

 private:
  std::unordered_map<SessionId, SessionSummary> sessions_;
  std::unordered_map<std::string, SessionId> name_index_;
  std::vector<SessionId> order_;
  SessionId next_id_{1};
};

}  // namespace wmux
