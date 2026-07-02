#include "wmux/terminal_input.hpp"

#include "wmux/resource_limits.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <sstream>

namespace wmux {
namespace {

constexpr std::string_view kBracketedPasteStart = "\x1b[200~";
constexpr std::string_view kBracketedPasteEnd = "\x1b[201~";
constexpr std::size_t kMaxPendingInputBytes = kMaxAttachInputPayloadBytes;

enum class ParseStatus {
  Parsed,
  Incomplete,
  Invalid,
};

struct ParsedEvent {
  ParseStatus status{ParseStatus::Invalid};
  TerminalInputEvent event;
  std::size_t bytes_consumed{0};
};

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::vector<std::uint8_t> debug_bytes(std::string_view bytes) {
  return {bytes.begin(), bytes.end()};
}

KeyModifiers modifier(KeyModifier value) {
  return static_cast<KeyModifiers>(value);
}

KeyModifiers add_modifier(KeyModifiers modifiers, KeyModifier value) {
  return static_cast<KeyModifiers>(modifiers | modifier(value));
}

KeyModifiers decode_xterm_modifier(int value) {
  if (value <= 1) {
    return modifier(KeyModifier::None);
  }

  const int encoded = value - 1;
  KeyModifiers modifiers = modifier(KeyModifier::None);
  if ((encoded & 1) != 0) {
    modifiers = add_modifier(modifiers, KeyModifier::Shift);
  }
  if ((encoded & 2) != 0) {
    modifiers = add_modifier(modifiers, KeyModifier::Alt);
  }
  if ((encoded & 4) != 0) {
    modifiers = add_modifier(modifiers, KeyModifier::Ctrl);
  }
  return modifiers;
}

std::vector<int> parse_csi_params(std::string_view params) {
  std::vector<int> values;
  std::size_t index = 0;
  while (index <= params.size()) {
    const auto next = params.find(';', index);
    const auto end = next == std::string_view::npos ? params.size() : next;
    int value = 0;
    if (end > index) {
      const auto first = params.data() + index;
      const auto last = params.data() + end;
      const auto [ptr, error] = std::from_chars(first, last, value);
      if (error != std::errc{} || ptr != last) {
        value = 0;
      }
    }
    values.push_back(value);
    if (next == std::string_view::npos) {
      break;
    }
    index = next + 1;
  }
  return values;
}

TerminalInputEvent key_event(
    Key key,
    std::string_view encoded,
    KeyModifiers modifiers = modifier(KeyModifier::None),
    char character = 0,
    std::uint8_t function_number = 0,
    bool printable = false,
    bool keep_raw_debug = false) {
  TerminalInputEvent event;
  event.kind = TerminalInputEventKind::Key;
  event.encoded_input.assign(encoded.begin(), encoded.end());
  event.key.key = key;
  event.key.character = character;
  event.key.function_number = function_number;
  event.key.modifiers = modifiers;
  event.key.printable = printable;
  if (keep_raw_debug) {
    event.key.raw_debug = debug_bytes(encoded);
  }
  return event;
}

ParsedEvent parsed_key(
    Key key,
    std::string_view encoded,
    KeyModifiers modifiers = modifier(KeyModifier::None),
    char character = 0,
    std::uint8_t function_number = 0,
    bool printable = false,
    bool keep_raw_debug = false) {
  return {
      ParseStatus::Parsed,
      key_event(key, encoded, modifiers, character, function_number, printable, keep_raw_debug),
      encoded.size()};
}

ParsedEvent parse_single_byte(std::string_view input) {
  const auto byte = static_cast<unsigned char>(input.front());
  const std::string_view encoded = input.substr(0, 1);
  if (byte == '\r' || byte == '\n') {
    return parsed_key(Key::Enter, encoded);
  }
  if (byte == '\t') {
    return parsed_key(Key::Tab, encoded);
  }
  if (byte == '\b' || byte == 0x7f) {
    return parsed_key(Key::Backspace, encoded);
  }
  if (byte > 0 && byte < 0x20) {
    const char character = static_cast<char>('a' + byte - 1);
    return parsed_key(Key::Char, encoded, modifier(KeyModifier::Ctrl), character);
  }
  if (byte >= 0x20 && byte < 0x7f) {
    return parsed_key(Key::Char, encoded, modifier(KeyModifier::None), static_cast<char>(byte), 0, true);
  }

  return parsed_key(Key::Unknown, encoded, modifier(KeyModifier::None), 0, 0, false, true);
}

ParsedEvent parsed_mouse(const MouseParseResult& mouse) {
  TerminalInputEvent event;
  event.kind = TerminalInputEventKind::Mouse;
  event.mouse = *mouse.event;
  return {ParseStatus::Parsed, event, mouse.bytes_consumed};
}

ParsedEvent parse_bracketed_paste(std::string_view input) {
  if (!starts_with(input, kBracketedPasteStart)) {
    return {ParseStatus::Invalid, {}, 0};
  }

  const auto end = input.find(kBracketedPasteEnd, kBracketedPasteStart.size());
  if (end == std::string_view::npos) {
    if (input.size() > kMaxPendingInputBytes) {
      return parsed_key(Key::Unknown, input.substr(0, 1), modifier(KeyModifier::None), 0, 0, false, true);
    }
    return {ParseStatus::Incomplete, {}, 0};
  }

  const auto payload = input.substr(kBracketedPasteStart.size(), end - kBracketedPasteStart.size());
  TerminalInputEvent event;
  event.kind = TerminalInputEventKind::Paste;
  event.encoded_input.assign(
      input.begin(),
      input.begin() + static_cast<std::ptrdiff_t>(end + kBracketedPasteEnd.size()));
  event.paste.bytes.assign(payload.begin(), payload.end());
  event.paste.bytes_len = event.paste.bytes.size();
  return {ParseStatus::Parsed, event, event.encoded_input.size()};
}

ParsedEvent parse_ss3(std::string_view input) {
  if (input.size() < 3) {
    return {ParseStatus::Incomplete, {}, 0};
  }

  const char final = input[2];
  const auto encoded = input.substr(0, 3);
  switch (final) {
    case 'P':
      return parsed_key(Key::Function, encoded, modifier(KeyModifier::None), 0, 1, false, true);
    case 'Q':
      return parsed_key(Key::Function, encoded, modifier(KeyModifier::None), 0, 2, false, true);
    case 'R':
      return parsed_key(Key::Function, encoded, modifier(KeyModifier::None), 0, 3, false, true);
    case 'S':
      return parsed_key(Key::Function, encoded, modifier(KeyModifier::None), 0, 4, false, true);
    default:
      return parsed_key(Key::Unknown, encoded, modifier(KeyModifier::None), 0, 0, false, true);
  }
}

ParsedEvent parse_csi(std::string_view input) {
  std::size_t final_index = 2;
  while (final_index < input.size()) {
    const auto byte = static_cast<unsigned char>(input[final_index]);
    if (byte >= 0x40 && byte <= 0x7e) {
      break;
    }
    ++final_index;
  }

  if (final_index >= input.size()) {
    if (input.size() > kMaxPendingInputBytes) {
      return parsed_key(Key::Unknown, input.substr(0, 1), modifier(KeyModifier::None), 0, 0, false, true);
    }
    return {ParseStatus::Incomplete, {}, 0};
  }

  const auto encoded = input.substr(0, final_index + 1);
  const auto params = parse_csi_params(input.substr(2, final_index - 2));
  const char final = input[final_index];
  const auto modifier_param = params.size() >= 2 ? params[1] : 1;
  const auto modifiers = decode_xterm_modifier(modifier_param);

  switch (final) {
    case 'A':
      return parsed_key(Key::Up, encoded, modifiers, 0, 0, false, true);
    case 'B':
      return parsed_key(Key::Down, encoded, modifiers, 0, 0, false, true);
    case 'C':
      return parsed_key(Key::Right, encoded, modifiers, 0, 0, false, true);
    case 'D':
      return parsed_key(Key::Left, encoded, modifiers, 0, 0, false, true);
    case 'H':
      return parsed_key(Key::Home, encoded, modifiers, 0, 0, false, true);
    case 'F':
      return parsed_key(Key::End, encoded, modifiers, 0, 0, false, true);
    case 'Z':
      return parsed_key(Key::Tab, encoded, modifier(KeyModifier::Shift), 0, 0, false, true);
    case '~': {
      const int code = params.empty() ? 0 : params[0];
      switch (code) {
        case 1:
        case 7:
          return parsed_key(Key::Home, encoded, modifiers, 0, 0, false, true);
        case 4:
        case 8:
          return parsed_key(Key::End, encoded, modifiers, 0, 0, false, true);
        case 5:
          return parsed_key(Key::PageUp, encoded, modifiers, 0, 0, false, true);
        case 6:
          return parsed_key(Key::PageDown, encoded, modifiers, 0, 0, false, true);
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
          return parsed_key(
              Key::Function,
              encoded,
              modifiers,
              0,
              static_cast<std::uint8_t>(code - 10),
              false,
              true);
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
          return parsed_key(
              Key::Function,
              encoded,
              modifiers,
              0,
              static_cast<std::uint8_t>(code - 11),
              false,
              true);
        case 23:
        case 24:
          return parsed_key(
              Key::Function,
              encoded,
              modifiers,
              0,
              static_cast<std::uint8_t>(code - 12),
              false,
              true);
        default:
          return parsed_key(Key::Unknown, encoded, modifiers, 0, 0, false, true);
      }
    }
    default:
      return parsed_key(Key::Unknown, encoded, modifiers, 0, 0, false, true);
  }
}

ParsedEvent parse_escape_sequence(std::string_view input, const TerminalInputDecoderOptions& options) {
  if (input.size() == 1) {
    return {ParseStatus::Incomplete, {}, 0};
  }

  if (options.bracketed_paste_enabled && starts_with(input, kBracketedPasteStart)) {
    return parse_bracketed_paste(input);
  }

  if (options.mouse_enabled && is_sgr_mouse_sequence_prefix(input)) {
    const auto mouse = parse_sgr_mouse_sequence(input);
    if (mouse.status == MouseParseStatus::Parsed && mouse.event) {
      return parsed_mouse(mouse);
    }
    if (mouse.status == MouseParseStatus::Incomplete) {
      return {ParseStatus::Incomplete, {}, 0};
    }
  }

  if (input[1] == '[') {
    return parse_csi(input);
  }
  if (input[1] == 'O') {
    return parse_ss3(input);
  }

  auto second = parse_single_byte(input.substr(1, 1));
  if (second.status == ParseStatus::Parsed && second.event.kind == TerminalInputEventKind::Key) {
    second.event.key.modifiers = add_modifier(second.event.key.modifiers, KeyModifier::Alt);
    second.event.encoded_input.assign(input.begin(), input.begin() + 2);
    if (!second.event.key.printable) {
      second.event.key.raw_debug = debug_bytes(second.event.encoded_input);
    }
    return {ParseStatus::Parsed, second.event, 2};
  }

  return parsed_key(Key::Unknown, input.substr(0, 2), modifier(KeyModifier::None), 0, 0, false, true);
}

ParsedEvent parse_next_event(std::string_view input, const TerminalInputDecoderOptions& options) {
  if (input.empty()) {
    return {ParseStatus::Incomplete, {}, 0};
  }

  if (input.front() == '\x1b') {
    return parse_escape_sequence(input, options);
  }

  return parse_single_byte(input);
}

std::string key_name(Key key) {
  switch (key) {
    case Key::Char:
      return "Char";
    case Key::Enter:
      return "Enter";
    case Key::Escape:
      return "Escape";
    case Key::Backspace:
      return "Backspace";
    case Key::Tab:
      return "Tab";
    case Key::Up:
      return "Up";
    case Key::Down:
      return "Down";
    case Key::Left:
      return "Left";
    case Key::Right:
      return "Right";
    case Key::Home:
      return "Home";
    case Key::End:
      return "End";
    case Key::PageUp:
      return "PageUp";
    case Key::PageDown:
      return "PageDown";
    case Key::Function:
      return "Function";
    case Key::CtrlBreak:
      return "CtrlBreak";
    case Key::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

void append_modifiers(std::ostringstream& out, KeyModifiers modifiers) {
  bool first = true;
  const auto append = [&](std::string_view name) {
    if (!first) {
      out << '+';
    }
    first = false;
    out << name;
  };

  if (has_modifier(modifiers, KeyModifier::Ctrl)) {
    append("Ctrl");
  }
  if (has_modifier(modifiers, KeyModifier::Alt)) {
    append("Alt");
  }
  if (has_modifier(modifiers, KeyModifier::Shift)) {
    append("Shift");
  }
  if (first) {
    append("None");
  }
}

}  // namespace

KeyModifiers operator|(KeyModifier lhs, KeyModifier rhs) {
  return static_cast<KeyModifiers>(static_cast<KeyModifiers>(lhs) | static_cast<KeyModifiers>(rhs));
}

bool has_modifier(KeyModifiers modifiers, KeyModifier modifier_value) {
  return (modifiers & modifier(modifier_value)) != 0;
}

TerminalInputDecodeResult decode_terminal_input(
    TerminalInputDecoderState& state,
    std::string_view bytes,
    const TerminalInputDecoderOptions& options) {
  TerminalInputDecodeResult result;
  if (state.pending.empty() && !bytes.empty()) {
    state.pending_started_at = std::chrono::steady_clock::now();
  }
  state.pending.append(bytes.begin(), bytes.end());

  std::size_t index = 0;
  while (index < state.pending.size()) {
    const auto parsed = parse_next_event(std::string_view{state.pending}.substr(index), options);
    if (parsed.status == ParseStatus::Incomplete) {
      break;
    }
    if (parsed.status == ParseStatus::Invalid || parsed.bytes_consumed == 0) {
      const auto fallback = parse_single_byte(std::string_view{state.pending}.substr(index, 1));
      result.events.push_back(fallback.event);
      ++index;
      continue;
    }

    result.events.push_back(parsed.event);
    index += parsed.bytes_consumed;
  }

  if (index > 0) {
    state.pending.erase(0, index);
    if (!state.pending.empty()) {
      state.pending_started_at = std::chrono::steady_clock::now();
    }
  }

  result.has_pending = !state.pending.empty();
  return result;
}

TerminalInputDecodeResult flush_terminal_input_decoder(TerminalInputDecoderState& state) {
  TerminalInputDecodeResult result;
  while (!state.pending.empty()) {
    const auto encoded = std::string_view{state.pending}.substr(0, 1);
    if (encoded[0] == '\x1b') {
      result.events.push_back(key_event(Key::Escape, encoded, modifier(KeyModifier::None), 0, 0, false, true));
    } else {
      result.events.push_back(parse_single_byte(encoded).event);
    }
    state.pending.erase(0, 1);
  }
  result.has_pending = false;
  return result;
}

bool terminal_input_decoder_has_pending_escape(const TerminalInputDecoderState& state) {
  return state.pending == "\x1b";
}

std::chrono::milliseconds terminal_input_pending_age(
    const TerminalInputDecoderState& state,
    std::chrono::steady_clock::time_point now) {
  if (state.pending.empty()) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - state.pending_started_at);
}

std::string key_event_debug_name(const KeyEvent& event) {
  std::ostringstream out;
  if (event.key == Key::Char) {
    out << "Char";
    if (has_modifier(event.modifiers, KeyModifier::Ctrl)) {
      out << "(Ctrl";
      if (event.character != 0) {
        out << '+' << static_cast<char>(std::toupper(static_cast<unsigned char>(event.character)));
      }
      out << ')';
    } else if (has_modifier(event.modifiers, KeyModifier::Alt)) {
      out << "(Alt+";
      out << (event.printable && event.character != 0 ? std::string{1, event.character} : "?");
      out << ')';
    } else if (event.printable && event.character != 0) {
      out << "('";
      out << event.character;
      out << "')";
    }
  } else if (event.key == Key::Function) {
    out << "F" << static_cast<int>(event.function_number);
  } else {
    out << key_name(event.key);
  }

  out << " modifiers=";
  append_modifiers(out, event.modifiers);
  return out.str();
}

std::string terminal_input_event_debug_name(const TerminalInputEvent& event) {
  switch (event.kind) {
    case TerminalInputEventKind::Key:
      return key_event_debug_name(event.key);
    case TerminalInputEventKind::Mouse:
      return "MouseEvent";
    case TerminalInputEventKind::Paste:
      return "PasteEvent bytes_len=" + std::to_string(event.paste.bytes_len);
  }
  return "Unknown";
}

}  // namespace wmux
