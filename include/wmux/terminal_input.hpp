#pragma once

#include "wmux/mouse_input.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

enum class Key {
  Char,
  Enter,
  Escape,
  Backspace,
  Tab,
  Up,
  Down,
  Left,
  Right,
  Home,
  End,
  PageUp,
  PageDown,
  Function,
  CtrlBreak,
  Unknown,
};

enum class KeyModifier : std::uint8_t {
  None = 0,
  Shift = 1 << 0,
  Alt = 1 << 1,
  Ctrl = 1 << 2,
};

using KeyModifiers = std::uint8_t;

struct KeyEvent {
  Key key{Key::Unknown};
  char character{0};
  std::uint8_t function_number{0};
  KeyModifiers modifiers{static_cast<KeyModifiers>(KeyModifier::None)};
  bool printable{false};
  std::optional<std::vector<std::uint8_t>> raw_debug;
};

struct PasteEvent {
  std::string bytes;
  std::size_t bytes_len{0};
};

enum class TerminalInputEventKind {
  Key,
  Mouse,
  Paste,
};

struct TerminalInputEvent {
  TerminalInputEventKind kind{TerminalInputEventKind::Key};
  KeyEvent key;
  MouseEvent mouse;
  PasteEvent paste;
  std::string encoded_input;
};

struct TerminalInputDecoderOptions {
  bool mouse_enabled{false};
  bool bracketed_paste_enabled{true};
  std::chrono::milliseconds escape_time{50};
};

struct TerminalInputDecoderState {
  std::string pending;
  std::chrono::steady_clock::time_point pending_started_at{};
};

struct TerminalInputDecodeResult {
  std::vector<TerminalInputEvent> events;
  bool has_pending{false};
};

KeyModifiers operator|(KeyModifier lhs, KeyModifier rhs);
bool has_modifier(KeyModifiers modifiers, KeyModifier modifier);

TerminalInputDecodeResult decode_terminal_input(
    TerminalInputDecoderState& state,
    std::string_view bytes,
    const TerminalInputDecoderOptions& options);

TerminalInputDecodeResult flush_terminal_input_decoder(TerminalInputDecoderState& state);

bool terminal_input_decoder_has_pending_escape(const TerminalInputDecoderState& state);
std::chrono::milliseconds terminal_input_pending_age(
    const TerminalInputDecoderState& state,
    std::chrono::steady_clock::time_point now);

std::string key_event_debug_name(const KeyEvent& event);
std::string terminal_input_event_debug_name(const TerminalInputEvent& event);

}  // namespace wmux
