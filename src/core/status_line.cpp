#include "wmux/status_line.hpp"

#include <algorithm>

namespace wmux {

std::string_view status_line_mode_name(StatusLineMode mode) {
  switch (mode) {
    case StatusLineMode::Normal:
      return "normal";
    case StatusLineMode::Prefix:
      return "prefix";
    case StatusLineMode::CommandPrompt:
      return "command";
    case StatusLineMode::Copy:
      return "copy";
    case StatusLineMode::MouseDrag:
      return "mouse";
  }

  return "unknown";
}

std::string status_sanitize_text(std::string_view value) {
  constexpr std::size_t kMaxStatusBytes = 4096;
  std::string sanitized;
  sanitized.reserve(std::min(value.size(), kMaxStatusBytes));
  for (const char byte : value.substr(0, kMaxStatusBytes)) {
    sanitized.push_back(byte >= ' ' && byte <= '~' ? byte : ' ');
  }
  return sanitized;
}

void status_set_temporary(
    StatusState& state,
    std::string_view message,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds ttl) {
  const auto text = status_sanitize_text(message);
  if (text.empty()) {
    status_clear_temporary(state);
    return;
  }

  state.temporary_message = TempMessage{text, now, now + ttl, true};
}

void status_set_persistent(
    StatusState& state,
    std::string_view message,
    std::chrono::steady_clock::time_point now) {
  const auto text = status_sanitize_text(message);
  if (text.empty()) {
    status_clear_temporary(state);
    return;
  }

  state.temporary_message = TempMessage{text, now, {}, false};
}

void status_clear_temporary(StatusState& state) {
  state.temporary_message.reset();
}

bool status_expire_temporary(StatusState& state, std::chrono::steady_clock::time_point now) {
  if (!state.temporary_message || !state.temporary_message->expires ||
      now < state.temporary_message->expires_at) {
    return false;
  }

  state.temporary_message.reset();
  return true;
}

bool status_has_visible_temporary(
    const StatusState& state,
    std::chrono::steady_clock::time_point now) {
  return state.temporary_message &&
         (!state.temporary_message->expires || now < state.temporary_message->expires_at);
}

std::optional<std::chrono::steady_clock::time_point> status_next_expiry(
    const StatusState& state) {
  if (!state.temporary_message || !state.temporary_message->expires) {
    return std::nullopt;
  }

  return state.temporary_message->expires_at;
}

std::string format_status_line(const StatusState& state, int columns) {
  std::string left = status_sanitize_text(state.permanent_left.text);
  std::string right = status_sanitize_text(state.permanent_right.text);
  const std::string message =
      state.temporary_message ? status_sanitize_text(state.temporary_message->text) : std::string{};

  if (!message.empty()) {
    if (!left.empty()) {
      left += " | ";
    }
    left += message;
  }

  if (columns <= 0 || right.empty()) {
    return left;
  }

  const auto target_columns = static_cast<std::size_t>(columns);
  if (left.size() + right.size() + 1 >= target_columns) {
    return left;
  }

  left.append(target_columns - left.size() - right.size(), ' ');
  left += right;
  return left;
}

}  // namespace wmux
