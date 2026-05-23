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
using WindowId = std::uint64_t;

struct WindowSummary {
  WindowId id{0};
  std::string name;
  std::chrono::system_clock::time_point created_at;
};

struct SessionSummary {
  SessionId id{0};
  std::string name;
  WindowId active_window_id{0};
  std::vector<WindowSummary> windows;
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
  WindowId window_id{0};
};

enum class WindowError {
  None,
  EmptyName,
  DuplicateName,
  SessionNotFound,
  WindowNotFound,
};

struct WindowOperationResult {
  bool ok{false};
  WindowError error{WindowError::None};
  SessionId session_id{0};
  WindowId window_id{0};
};

class SessionManager {
 public:
  SessionOperationResult create_session(std::string name);
  SessionOperationResult rename_session(std::string_view current_name, std::string new_name);
  SessionOperationResult kill_session(std::string_view name);
  WindowOperationResult create_window(SessionId session_id, std::string name);
  WindowOperationResult rename_active_window(SessionId session_id, std::string name);
  WindowOperationResult select_next_window(SessionId session_id);
  WindowOperationResult select_previous_window(SessionId session_id);

  bool has_session(std::string_view name) const;
  std::optional<SessionId> session_id_for_name(std::string_view name) const;
  std::optional<SessionId> only_session_id() const;
  std::optional<WindowId> active_window_id(SessionId session_id) const;
  std::size_t session_count() const;
  std::vector<SessionSummary> list_sessions() const;
  std::vector<WindowSummary> list_windows(SessionId session_id) const;

 private:
  std::unordered_map<SessionId, SessionSummary> sessions_;
  std::unordered_map<std::string, SessionId> name_index_;
  std::vector<SessionId> order_;
  SessionId next_id_{1};
  WindowId next_window_id_{1};
};

}  // namespace wmux
