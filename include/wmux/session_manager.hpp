#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

struct SessionSummary {
  std::uint64_t id{0};
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
};

class SessionManager {
 public:
  SessionOperationResult create_session(std::string name);
  SessionOperationResult rename_session(std::string_view current_name, std::string new_name);
  SessionOperationResult kill_session(std::string_view name);

  bool has_session(std::string_view name) const;
  std::vector<SessionSummary> list_sessions() const;

 private:
  std::optional<std::size_t> find_index(std::string_view name) const;

  std::vector<SessionSummary> sessions_;
  std::uint64_t next_id_{1};
};

}  // namespace wmux
