#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace wmux {

enum class StatusLineMode {
  Normal,
  Prefix,
  CommandPrompt,
  Copy,
  MouseDrag,
};

struct StatusSegment {
  std::string text;
};

struct TempMessage {
  std::string text;
  std::chrono::steady_clock::time_point created_at{};
  std::chrono::steady_clock::time_point expires_at{};
  bool expires{true};
};

struct StatusState {
  StatusSegment permanent_left;
  StatusSegment permanent_right;
  std::optional<TempMessage> temporary_message;
};

constexpr auto kDefaultStatusMessageTtl = std::chrono::milliseconds{3000};
constexpr auto kPrefixStatusMessageTtl = std::chrono::milliseconds{1000};

std::string_view status_line_mode_name(StatusLineMode mode);
std::string status_sanitize_text(std::string_view value);
void status_set_temporary(
    StatusState& state,
    std::string_view message,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds ttl = kDefaultStatusMessageTtl);
void status_set_persistent(
    StatusState& state,
    std::string_view message,
    std::chrono::steady_clock::time_point now);
void status_clear_temporary(StatusState& state);
bool status_expire_temporary(StatusState& state, std::chrono::steady_clock::time_point now);
bool status_has_visible_temporary(
    const StatusState& state,
    std::chrono::steady_clock::time_point now);
std::optional<std::chrono::steady_clock::time_point> status_next_expiry(
    const StatusState& state);
std::string format_status_line(const StatusState& state, int columns);

}  // namespace wmux
