#include "wmux/ipc_protocol.hpp"

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <utility>

namespace wmux {
namespace {

std::string escape_json_string(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());

  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }

  return escaped;
}

std::string unescape_json_string(std::string_view value) {
  std::string unescaped;
  unescaped.reserve(value.size());

  bool escaping = false;
  for (const char ch : value) {
    if (!escaping) {
      if (ch == '\\') {
        escaping = true;
      } else {
        unescaped += ch;
      }
      continue;
    }

    switch (ch) {
      case 'n':
        unescaped += '\n';
        break;
      case 'r':
        unescaped += '\r';
        break;
      case 't':
        unescaped += '\t';
        break;
      case '\\':
      case '"':
        unescaped += ch;
        break;
      default:
        unescaped += ch;
        break;
    }
    escaping = false;
  }

  return unescaped;
}

void append_json_field(std::ostringstream& out, std::string_view key, std::string_view value) {
  out << ",\"" << key << "\":\"" << escape_json_string(value) << '"';
}

void append_json_bool(std::ostringstream& out, std::string_view key, bool value) {
  out << ",\"" << key << "\":" << (value ? "true" : "false");
}

void append_json_uint(std::ostringstream& out, std::string_view key, std::uint16_t value) {
  out << ",\"" << key << "\":" << value;
}

std::size_t skip_json_ws(std::string_view json, std::size_t index) {
  while (index < json.size() &&
         std::isspace(static_cast<unsigned char>(json[index])) != 0) {
    ++index;
  }
  return index;
}

std::optional<std::pair<std::string, std::size_t>> parse_json_string_at(
    std::string_view json,
    std::size_t index) {
  if (index >= json.size() || json[index] != '"') {
    return std::nullopt;
  }

  const auto value_start = index + 1;
  bool escaping = false;
  for (std::size_t i = value_start; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaping) {
      escaping = false;
      continue;
    }

    if (ch == '\\') {
      escaping = true;
      continue;
    }

    if (ch == '"') {
      return std::pair{unescape_json_string(json.substr(value_start, i - value_start)), i + 1};
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> skip_json_value(std::string_view json, std::size_t index) {
  if (index >= json.size()) {
    return std::nullopt;
  }

  if (json[index] == '"') {
    const auto parsed = parse_json_string_at(json, index);
    if (!parsed) {
      return std::nullopt;
    }
    return parsed->second;
  }

  if (json[index] == '{' || json[index] == '[') {
    const char open = json[index];
    const char close = open == '{' ? '}' : ']';
    int depth = 1;
    bool escaping = false;
    bool in_string = false;
    for (std::size_t i = index + 1; i < json.size(); ++i) {
      const char ch = json[i];
      if (in_string) {
        if (escaping) {
          escaping = false;
        } else if (ch == '\\') {
          escaping = true;
        } else if (ch == '"') {
          in_string = false;
        }
        continue;
      }

      if (ch == '"') {
        in_string = true;
      } else if (ch == open) {
        ++depth;
      } else if (ch == close) {
        --depth;
        if (depth == 0) {
          return i + 1;
        }
      }
    }
    return std::nullopt;
  }

  std::size_t end = index;
  while (end < json.size() && json[end] != ',' && json[end] != '}') {
    ++end;
  }
  return end;
}

std::optional<std::string_view> find_json_value(std::string_view json, std::string_view key) {
  std::size_t index = skip_json_ws(json, 0);
  if (index >= json.size() || json[index] != '{') {
    return std::nullopt;
  }
  ++index;

  while (index < json.size()) {
    index = skip_json_ws(json, index);
    if (index < json.size() && json[index] == '}') {
      return std::nullopt;
    }

    const auto parsed_key = parse_json_string_at(json, index);
    if (!parsed_key) {
      return std::nullopt;
    }
    index = skip_json_ws(json, parsed_key->second);
    if (index >= json.size() || json[index] != ':') {
      return std::nullopt;
    }
    const auto value_start = skip_json_ws(json, index + 1);
    const auto value_end = skip_json_value(json, value_start);
    if (!value_end) {
      return std::nullopt;
    }

    if (parsed_key->first == key) {
      return json.substr(value_start, *value_end - value_start);
    }

    index = skip_json_ws(json, *value_end);
    if (index < json.size() && json[index] == ',') {
      ++index;
      continue;
    }
    if (index < json.size() && json[index] == '}') {
      return std::nullopt;
    }
  }

  return std::nullopt;
}

std::optional<std::string> find_json_string(std::string_view json, std::string_view key) {
  const auto value = find_json_value(json, key);
  if (!value) {
    return std::nullopt;
  }
  const auto parsed = parse_json_string_at(*value, 0);
  if (!parsed || parsed->second != value->size()) {
    return std::nullopt;
  }
  return parsed->first;
}

std::optional<bool> find_json_bool(std::string_view json, std::string_view key) {
  const auto value = find_json_value(json, key);
  if (!value) {
    return std::nullopt;
  }

  if (*value == "true") {
    return true;
  }
  if (*value == "false") {
    return false;
  }

  return std::nullopt;
}

std::optional<std::uint16_t> find_json_uint(std::string_view json, std::string_view key) {
  const auto value_text = find_json_value(json, key);
  if (!value_text) {
    return std::nullopt;
  }

  std::uint32_t value = 0;
  bool saw_digit = false;
  for (const char ch : *value_text) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }

    saw_digit = true;
    value = (value * 10) + static_cast<std::uint32_t>(ch - '0');
    if (value > 32767) {
      return std::nullopt;
    }
  }

  if (!saw_digit) {
    return std::nullopt;
  }

  return static_cast<std::uint16_t>(value);
}

std::string request_type_for_command(CommandKind kind) {
  switch (kind) {
    case CommandKind::DefaultSession:
      return "DefaultSession";
    case CommandKind::NewSession:
      return "NewSession";
    case CommandKind::ListSessions:
      return "ListSessions";
    case CommandKind::AttachSession:
      return "AttachSession";
    case CommandKind::RenameSession:
      return "RenameSession";
    case CommandKind::KillSession:
      return "KillSession";
    case CommandKind::NewWindow:
      return "NewWindow";
    case CommandKind::ListWindows:
      return "ListWindows";
    case CommandKind::RenameWindow:
      return "RenameWindow";
    case CommandKind::SelectWindow:
      return "SelectWindow";
    case CommandKind::NextWindow:
      return "NextWindow";
    case CommandKind::PreviousWindow:
      return "PreviousWindow";
    case CommandKind::KillWindow:
      return "KillWindow";
    case CommandKind::KillPane:
      return "KillPane";
    case CommandKind::SplitWindow:
      return "SplitWindow";
    case CommandKind::ResizePane:
      return "ResizePane";
    case CommandKind::SelectLayout:
      return "SelectLayout";
    case CommandKind::SetOption:
      return "SetOption";
    case CommandKind::BindKey:
      return "BindKey";
    case CommandKind::UnbindKey:
      return "UnbindKey";
    case CommandKind::ServerStatus:
      return "ServerStatus";
    case CommandKind::ServerStop:
      return "ServerStop";
    case CommandKind::DumpState:
      return "DumpState";
    case CommandKind::DumpLayout:
      return "DumpLayout";
    case CommandKind::DumpEvents:
      return "DumpEvents";
    case CommandKind::ResetTerminal:
    case CommandKind::Doctor:
    case CommandKind::DebugKeys:
      return {};
    case CommandKind::Daemon:
    case CommandKind::Help:
    case CommandKind::Version:
    case CommandKind::Unknown:
      return {};
  }

  return {};
}

std::string make_attach_frame(AttachFrameType type, std::string_view payload) {
  std::string frame;
  frame.reserve(kAttachFrameHeaderSize + payload.size());
  frame.push_back('W');
  frame.push_back('M');
  frame.push_back(static_cast<char>(type));

  const auto payload_size = static_cast<std::uint32_t>(payload.size());
  frame.push_back(static_cast<char>(payload_size & 0xFF));
  frame.push_back(static_cast<char>((payload_size >> 8) & 0xFF));
  frame.push_back(static_cast<char>((payload_size >> 16) & 0xFF));
  frame.push_back(static_cast<char>((payload_size >> 24) & 0xFF));
  frame.append(payload);
  return frame;
}

std::optional<std::uint32_t> max_payload_size_for_attach_frame(AttachFrameType type) {
  switch (type) {
    case AttachFrameType::Input:
      return kMaxAttachInputPayloadBytes;
    case AttachFrameType::Detach:
      return 0;
    case AttachFrameType::Command:
    case AttachFrameType::CommandMode:
      return kMaxAttachCommandPayloadBytes;
    case AttachFrameType::Resize:
      return kMaxAttachResizePayloadBytes;
    case AttachFrameType::Status:
      return kMaxAttachCommandPayloadBytes;
    case AttachFrameType::MouseFocus:
      return 4;
    case AttachFrameType::MouseEvent:
      return kMaxAttachMousePayloadBytes;
    case AttachFrameType::Scroll:
      return kMaxAttachScrollPayloadBytes;
    case AttachFrameType::CopyMode:
      return kMaxAttachCopyModePayloadBytes;
    case AttachFrameType::Paste:
      return kMaxAttachPastePayloadBytes;
  }

  return std::nullopt;
}

std::optional<std::uint32_t> max_payload_size_for_ipc_frame(IpcFrameKind kind) {
  switch (kind) {
    case IpcFrameKind::Control:
      return kMaxControlIpcPayloadBytes;
    case IpcFrameKind::AttachInput:
      return kMaxAttachFramePayloadSize;
    case IpcFrameKind::AttachOutput:
      return static_cast<std::uint32_t>(kMaxAttachRenderFrameBytes);
    case IpcFrameKind::Event:
      return kMaxAttachCommandPayloadBytes;
    case IpcFrameKind::Error:
      return kMaxIpcErrorPayloadBytes;
  }

  return std::nullopt;
}

std::optional<IpcFrameKind> ipc_frame_kind_from_byte(std::uint8_t value) {
  switch (value) {
    case static_cast<std::uint8_t>(IpcFrameKind::Control):
      return IpcFrameKind::Control;
    case static_cast<std::uint8_t>(IpcFrameKind::AttachInput):
      return IpcFrameKind::AttachInput;
    case static_cast<std::uint8_t>(IpcFrameKind::AttachOutput):
      return IpcFrameKind::AttachOutput;
    case static_cast<std::uint8_t>(IpcFrameKind::Event):
      return IpcFrameKind::Event;
    case static_cast<std::uint8_t>(IpcFrameKind::Error):
      return IpcFrameKind::Error;
    default:
      return std::nullopt;
  }
}

void append_u16_le(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void append_u32_le(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void append_u64_le(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint16_t read_u16_le(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
         static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32_le(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

std::uint64_t read_u64_le(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value <<= 8;
    value |= static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(index)]);
  }
  return value;
}

IpcFrameParseResult ipc_frame_parse_error(IpcFrameError error, std::string message) {
  IpcFrameParseResult result;
  result.error = error;
  result.message = std::move(message);
  return result;
}

}  // namespace

std::string_view ipc_frame_kind_name(IpcFrameKind kind) {
  switch (kind) {
    case IpcFrameKind::Control:
      return "Control";
    case IpcFrameKind::AttachInput:
      return "AttachInput";
    case IpcFrameKind::AttachOutput:
      return "AttachOutput";
    case IpcFrameKind::Event:
      return "Event";
    case IpcFrameKind::Error:
      return "Error";
  }

  return "Unknown";
}

std::string_view ipc_frame_error_name(IpcFrameError error) {
  switch (error) {
    case IpcFrameError::None:
      return "None";
    case IpcFrameError::PartialHeader:
      return "PartialHeader";
    case IpcFrameError::BadMagic:
      return "BadMagic";
    case IpcFrameError::UnsupportedVersion:
      return "UnsupportedVersion";
    case IpcFrameError::UnknownKind:
      return "UnknownKind";
    case IpcFrameError::OversizedPayload:
      return "OversizedPayload";
    case IpcFrameError::TruncatedPayload:
      return "TruncatedPayload";
  }

  return "Unknown";
}

std::string make_ipc_frame(
    IpcFrameKind kind,
    std::uint64_t request_id,
    std::string_view payload) {
  std::string frame;
  frame.reserve(kIpcFrameHeaderSize + payload.size());
  frame.append("WMUX", 4);
  append_u16_le(frame, kIpcProtocolVersion);
  frame.push_back(static_cast<char>(kind));
  append_u64_le(frame, request_id);
  append_u32_le(frame, static_cast<std::uint32_t>(payload.size()));
  frame.append(payload);
  return frame;
}

IpcFrameParseResult parse_ipc_frame_header(std::string_view header) {
  if (header.size() != kIpcFrameHeaderSize) {
    return ipc_frame_parse_error(IpcFrameError::PartialHeader, "wmux: partial IPC frame header");
  }

  if (header.substr(0, 4) != "WMUX") {
    return ipc_frame_parse_error(IpcFrameError::BadMagic, "wmux: bad IPC frame magic");
  }

  IpcFrameParseResult result;
  result.header.version = read_u16_le(header, 4);
  const auto kind = ipc_frame_kind_from_byte(static_cast<unsigned char>(header[6]));
  result.header.request_id = read_u64_le(header, 7);
  result.header.payload_size = read_u32_le(header, 15);

  if (result.header.version != kIpcProtocolVersion) {
    result.error = IpcFrameError::UnsupportedVersion;
    result.message = "wmux: unsupported IPC protocol version";
    return result;
  }

  if (!kind) {
    result.error = IpcFrameError::UnknownKind;
    result.message = "wmux: unknown IPC frame kind";
    return result;
  }
  result.header.kind = *kind;

  const auto max_payload_size = max_payload_size_for_ipc_frame(result.header.kind);
  if (!max_payload_size || result.header.payload_size > *max_payload_size ||
      result.header.payload_size > kMaxIpcFramePayloadBytes) {
    result.error = IpcFrameError::OversizedPayload;
    result.message = "wmux: IPC frame payload is too large";
    return result;
  }

  result.ok = true;
  return result;
}

IpcFrameParseResult parse_ipc_frame(std::string_view bytes) {
  if (bytes.size() < kIpcFrameHeaderSize) {
    return ipc_frame_parse_error(IpcFrameError::PartialHeader, "wmux: partial IPC frame header");
  }

  auto result = parse_ipc_frame_header(bytes.substr(0, kIpcFrameHeaderSize));
  if (!result.ok) {
    return result;
  }

  const auto expected_size = kIpcFrameHeaderSize + result.header.payload_size;
  if (bytes.size() < expected_size) {
    result.ok = false;
    result.error = IpcFrameError::TruncatedPayload;
    result.message = "wmux: truncated IPC frame payload";
    return result;
  }

  result.payload.assign(
      bytes.data() + kIpcFrameHeaderSize,
      bytes.data() + kIpcFrameHeaderSize + result.header.payload_size);
  return result;
}

std::string make_ping_request_json() {
  return "{\"type\":\"Ping\"}\n";
}

std::string make_command_request_json(const CommandLine& command) {
  const auto type = request_type_for_command(command.kind);
  std::ostringstream out;
  out << "{\"type\":\"" << type << '"';

  if (!command.session_name.empty()) {
    append_json_field(out, "session_name", command.session_name);
  }
  if (!command.target_name.empty()) {
    append_json_field(out, "target_name", command.target_name);
  }
  if (!command.new_name.empty()) {
    append_json_field(out, "new_name", command.new_name);
  }
  if (!command.window_name.empty()) {
    append_json_field(out, "window_name", command.window_name);
  }
  if (!command.split_direction.empty()) {
    append_json_field(out, "split_direction", command.split_direction);
  }
  if (!command.resize_direction.empty()) {
    append_json_field(out, "resize_direction", command.resize_direction);
  }
  if (!command.option_name.empty()) {
    append_json_field(out, "option_name", command.option_name);
  }
  if (!command.option_value.empty()) {
    append_json_field(out, "option_value", command.option_value);
  }
  if (!command.key_name.empty()) {
    append_json_field(out, "key_name", command.key_name);
  }
  if (!command.key_action.empty()) {
    append_json_field(out, "key_action", command.key_action);
  }
  if (command.force) {
    append_json_bool(out, "force", command.force);
  }

  out << "}\n";
  return out.str();
}

std::string make_attach_request_json(
    const CommandLine& command,
    std::uint16_t terminal_columns,
    std::uint16_t terminal_rows,
    const TerminalCapabilities& terminal_capabilities) {
  std::ostringstream out;
  out << "{\"type\":\"AttachStart\"";

  if (!command.session_name.empty()) {
    append_json_field(out, "session_name", command.session_name);
  }
  if (terminal_columns > 0 && terminal_rows > 0) {
    append_json_uint(out, "terminal_columns", terminal_columns);
    append_json_uint(out, "terminal_rows", terminal_rows);
  }
  append_json_field(out, "terminal_host", terminal_host_name(terminal_capabilities.host));
  append_json_bool(out, "terminal_truecolor", terminal_capabilities.supports_truecolor);
  append_json_bool(out, "terminal_256_color", terminal_capabilities.supports_256_color);
  append_json_bool(out, "terminal_sgr_mouse", terminal_capabilities.supports_sgr_mouse);
  append_json_bool(out, "terminal_mouse_drag", terminal_capabilities.supports_mouse_drag);
  append_json_bool(out, "terminal_mouse_wheel", terminal_capabilities.supports_mouse_wheel);
  append_json_bool(
      out, "terminal_bracketed_paste", terminal_capabilities.supports_bracketed_paste);
  append_json_bool(out, "terminal_focus_events", terminal_capabilities.supports_focus_events);
  append_json_bool(out, "terminal_cursor_style", terminal_capabilities.supports_cursor_style);
  append_json_bool(out, "terminal_alt_screen", terminal_capabilities.supports_alt_screen);
  append_json_bool(out, "terminal_extended_keys", terminal_capabilities.supports_extended_keys);
  append_json_bool(
      out, "terminal_osc52_clipboard", terminal_capabilities.supports_osc52_clipboard);
  append_json_bool(
      out,
      "terminal_synchronized_output",
      terminal_capabilities.supports_synchronized_output);

  out << "}\n";
  return out.str();
}

std::string_view attach_frame_type_name(AttachFrameType type) {
  switch (type) {
    case AttachFrameType::Input:
      return "Input";
    case AttachFrameType::Detach:
      return "Detach";
    case AttachFrameType::Command:
      return "Command";
    case AttachFrameType::Resize:
      return "Resize";
    case AttachFrameType::Status:
      return "Status";
    case AttachFrameType::CommandMode:
      return "CommandMode";
    case AttachFrameType::MouseFocus:
      return "MouseFocus";
    case AttachFrameType::MouseEvent:
      return "Mouse";
    case AttachFrameType::Scroll:
      return "Scroll";
    case AttachFrameType::CopyMode:
      return "CopyMode";
    case AttachFrameType::Paste:
      return "Paste";
  }

  return "Unknown";
}

std::optional<IpcRequest> parse_request_json(std::string_view json) {
  const auto type = find_json_string(json, "type");
  if (!type || type->empty()) {
    return std::nullopt;
  }

  IpcRequest request;
  request.type = *type;

  if (const auto session_name = find_json_string(json, "session_name")) {
    request.session_name = *session_name;
  }
  if (const auto target_name = find_json_string(json, "target_name")) {
    request.target_name = *target_name;
  }
  if (const auto new_name = find_json_string(json, "new_name")) {
    request.new_name = *new_name;
  }
  if (const auto window_name = find_json_string(json, "window_name")) {
    request.window_name = *window_name;
  }
  if (const auto split_direction = find_json_string(json, "split_direction")) {
    request.split_direction = *split_direction;
  }
  if (const auto resize_direction = find_json_string(json, "resize_direction")) {
    request.resize_direction = *resize_direction;
  }
  if (const auto option_name = find_json_string(json, "option_name")) {
    request.option_name = *option_name;
  }
  if (const auto option_value = find_json_string(json, "option_value")) {
    request.option_value = *option_value;
  }
  if (const auto key_name = find_json_string(json, "key_name")) {
    request.key_name = *key_name;
  }
  if (const auto key_action = find_json_string(json, "key_action")) {
    request.key_action = *key_action;
  }
  if (const auto force = find_json_bool(json, "force")) {
    request.force = *force;
  }
  if (const auto terminal_columns = find_json_uint(json, "terminal_columns")) {
    request.terminal_columns = *terminal_columns;
  }
  if (const auto terminal_rows = find_json_uint(json, "terminal_rows")) {
    request.terminal_rows = *terminal_rows;
  }
  if (const auto terminal_host = find_json_string(json, "terminal_host")) {
    request.terminal_capabilities_provided = true;
    request.terminal_capabilities.host =
        parse_terminal_host(*terminal_host).value_or(TerminalHost::Unknown);
  }
  const auto apply_terminal_bool = [&](std::string_view field, bool& target) {
    if (const auto value = find_json_bool(json, field)) {
      request.terminal_capabilities_provided = true;
      target = *value;
    }
  };
  apply_terminal_bool("terminal_truecolor", request.terminal_capabilities.supports_truecolor);
  apply_terminal_bool("terminal_256_color", request.terminal_capabilities.supports_256_color);
  apply_terminal_bool("terminal_sgr_mouse", request.terminal_capabilities.supports_sgr_mouse);
  apply_terminal_bool("terminal_mouse_drag", request.terminal_capabilities.supports_mouse_drag);
  apply_terminal_bool("terminal_mouse_wheel", request.terminal_capabilities.supports_mouse_wheel);
  apply_terminal_bool(
      "terminal_bracketed_paste",
      request.terminal_capabilities.supports_bracketed_paste);
  apply_terminal_bool(
      "terminal_focus_events",
      request.terminal_capabilities.supports_focus_events);
  apply_terminal_bool(
      "terminal_cursor_style",
      request.terminal_capabilities.supports_cursor_style);
  apply_terminal_bool("terminal_alt_screen", request.terminal_capabilities.supports_alt_screen);
  apply_terminal_bool(
      "terminal_extended_keys",
      request.terminal_capabilities.supports_extended_keys);
  apply_terminal_bool(
      "terminal_osc52_clipboard",
      request.terminal_capabilities.supports_osc52_clipboard);
  apply_terminal_bool(
      "terminal_synchronized_output",
      request.terminal_capabilities.supports_synchronized_output);

  return request;
}

std::string make_response_json(bool ok, std::string_view message) {
  std::ostringstream out;
  out << "{\"ok\":" << (ok ? "true" : "false");
  append_json_field(out, "message", message);
  out << "}\n";
  return out.str();
}

std::string make_response_json(bool ok, std::string_view message, bool mouse_enabled) {
  std::ostringstream out;
  out << "{\"ok\":" << (ok ? "true" : "false");
  append_json_field(out, "message", message);
  append_json_bool(out, "mouse_enabled", mouse_enabled);
  out << "}\n";
  return out.str();
}

std::string make_response_json(
    bool ok,
    std::string_view message,
    bool mouse_enabled,
    std::string_view prefix,
    bool status_bar_enabled,
    std::uint16_t escape_time_ms,
    std::string_view key_bindings) {
  std::ostringstream out;
  out << "{\"ok\":" << (ok ? "true" : "false");
  append_json_field(out, "message", message);
  append_json_bool(out, "mouse_enabled", mouse_enabled);
  append_json_field(out, "prefix", prefix);
  append_json_bool(out, "status_bar_enabled", status_bar_enabled);
  append_json_uint(out, "escape_time_ms", escape_time_ms);
  if (!key_bindings.empty()) {
    append_json_field(out, "key_bindings", key_bindings);
  }
  out << "}\n";
  return out.str();
}

std::optional<IpcResponse> parse_response_json(std::string_view json) {
  const auto ok = find_json_bool(json, "ok");
  const auto message = find_json_string(json, "message");
  if (!ok || !message) {
    return std::nullopt;
  }

  IpcResponse response;
  response.ok = *ok;
  response.message = *message;
  if (const auto mouse_enabled = find_json_bool(json, "mouse_enabled")) {
    response.mouse_enabled = *mouse_enabled;
  }
  if (const auto prefix = find_json_string(json, "prefix")) {
    response.prefix = *prefix;
  }
  if (const auto status_bar_enabled = find_json_bool(json, "status_bar_enabled")) {
    response.status_bar_enabled = *status_bar_enabled;
  }
  if (const auto escape_time_ms = find_json_uint(json, "escape_time_ms")) {
    response.escape_time_ms = *escape_time_ms;
  }
  if (const auto key_bindings = find_json_string(json, "key_bindings")) {
    response.key_bindings = *key_bindings;
  }
  return response;
}

std::string make_attach_input_frame(std::string_view bytes) {
  return make_attach_frame(AttachFrameType::Input, bytes);
}

std::string make_attach_detach_frame() {
  return make_attach_frame(AttachFrameType::Detach, {});
}

std::string make_attach_command_frame(std::string_view command) {
  return make_attach_frame(AttachFrameType::Command, command);
}

std::string make_attach_command_mode_frame(std::string_view command) {
  return make_attach_frame(AttachFrameType::CommandMode, command);
}

std::string make_attach_resize_frame(std::uint16_t columns, std::uint16_t rows) {
  std::string payload;
  payload.reserve(4);
  payload.push_back(static_cast<char>(columns & 0xFF));
  payload.push_back(static_cast<char>((columns >> 8) & 0xFF));
  payload.push_back(static_cast<char>(rows & 0xFF));
  payload.push_back(static_cast<char>((rows >> 8) & 0xFF));
  return make_attach_frame(AttachFrameType::Resize, payload);
}

std::string make_attach_status_frame(std::string_view status) {
  return make_attach_frame(AttachFrameType::Status, status);
}

std::string make_attach_mouse_focus_frame(std::uint16_t column, std::uint16_t row) {
  std::string payload;
  payload.reserve(4);
  payload.push_back(static_cast<char>(column & 0xFF));
  payload.push_back(static_cast<char>((column >> 8) & 0xFF));
  payload.push_back(static_cast<char>(row & 0xFF));
  payload.push_back(static_cast<char>((row >> 8) & 0xFF));
  return make_attach_frame(AttachFrameType::MouseFocus, payload);
}

std::string make_attach_mouse_event_frame(const AttachMouseEventPayload& event) {
  std::string payload;
  payload.reserve(8);
  payload.push_back(static_cast<char>(event.column & 0xFF));
  payload.push_back(static_cast<char>((event.column >> 8) & 0xFF));
  payload.push_back(static_cast<char>(event.row & 0xFF));
  payload.push_back(static_cast<char>((event.row >> 8) & 0xFF));
  payload.push_back(static_cast<char>(event.button_code & 0xFF));
  payload.push_back(static_cast<char>((event.button_code >> 8) & 0xFF));
  payload.push_back(static_cast<char>(event.button));
  payload.push_back(static_cast<char>(event.action));
  return make_attach_frame(AttachFrameType::MouseEvent, payload);
}

std::string make_attach_scroll_frame(AttachScrollAction action) {
  std::string payload;
  payload.push_back(static_cast<char>(action));
  return make_attach_frame(AttachFrameType::Scroll, payload);
}

std::string make_attach_copy_mode_frame(AttachCopyModeAction action) {
  std::string payload;
  payload.push_back(static_cast<char>(action));
  return make_attach_frame(AttachFrameType::CopyMode, payload);
}

std::optional<AttachFrameHeader> parse_attach_frame_header(std::string_view header) {
  if (header.size() != kAttachFrameHeaderSize || header[0] != 'W' || header[1] != 'M') {
    return std::nullopt;
  }

  AttachFrameHeader parsed;
  switch (static_cast<std::uint8_t>(header[2])) {
    case static_cast<std::uint8_t>(AttachFrameType::Input):
      parsed.type = AttachFrameType::Input;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::Detach):
      parsed.type = AttachFrameType::Detach;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::Command):
      parsed.type = AttachFrameType::Command;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::Resize):
      parsed.type = AttachFrameType::Resize;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::Status):
      parsed.type = AttachFrameType::Status;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::CommandMode):
      parsed.type = AttachFrameType::CommandMode;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::MouseFocus):
      parsed.type = AttachFrameType::MouseFocus;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::MouseEvent):
      parsed.type = AttachFrameType::MouseEvent;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::Scroll):
      parsed.type = AttachFrameType::Scroll;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::CopyMode):
      parsed.type = AttachFrameType::CopyMode;
      break;
    case static_cast<std::uint8_t>(AttachFrameType::Paste):
      parsed.type = AttachFrameType::Paste;
      break;
    default:
      return std::nullopt;
  }

  parsed.payload_size =
      static_cast<std::uint32_t>(static_cast<unsigned char>(header[3])) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[4])) << 8) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[5])) << 16) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[6])) << 24);
  const auto max_payload_size = max_payload_size_for_attach_frame(parsed.type);
  if (!max_payload_size || parsed.payload_size > *max_payload_size ||
      parsed.payload_size > kMaxAttachFramePayloadSize) {
    return std::nullopt;
  }

  return parsed;
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_attach_resize_payload(
    std::string_view payload) {
  if (payload.size() != 4) {
    return std::nullopt;
  }

  const auto columns =
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[1]) << 8);
  const auto rows =
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[2])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[3]) << 8);
  if (columns == 0 || rows == 0 || columns > kMaxAttachTerminalColumns ||
      rows > kMaxAttachTerminalRows) {
    return std::nullopt;
  }

  return std::pair{columns, rows};
}

std::optional<AttachMouseFocusPayload> parse_attach_mouse_focus_payload(
    std::string_view payload) {
  if (payload.size() != 4) {
    return std::nullopt;
  }

  const auto column =
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[1]) << 8);
  const auto row =
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[2])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[3]) << 8);
  if (column == 0 || row == 0 || column > 32767 || row > 32767) {
    return std::nullopt;
  }

  return AttachMouseFocusPayload{
      static_cast<std::uint16_t>(column),
      static_cast<std::uint16_t>(row)};
}

std::optional<AttachMouseEventPayload> parse_attach_mouse_event_payload(
    std::string_view payload) {
  if (payload.size() != 8) {
    return std::nullopt;
  }

  const auto column = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[1]) << 8));
  const auto row = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[2])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[3]) << 8));
  const auto button_code = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[4])) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(payload[5]) << 8));
  const auto button = static_cast<unsigned char>(payload[6]);
  const auto action = static_cast<unsigned char>(payload[7]);

  if (column == 0 || row == 0 || column > 32767 || row > 32767 ||
      button > static_cast<unsigned char>(AttachMouseButton::Other) ||
      action > static_cast<unsigned char>(AttachMouseAction::Wheel)) {
    return std::nullopt;
  }

  return AttachMouseEventPayload{
      column,
      row,
      button_code,
      static_cast<AttachMouseButton>(button),
      static_cast<AttachMouseAction>(action)};
}

std::optional<AttachScrollAction> parse_attach_scroll_payload(std::string_view payload) {
  if (payload.size() != 1) {
    return std::nullopt;
  }

  const auto action = static_cast<unsigned char>(payload[0]);
  if (action > static_cast<unsigned char>(AttachScrollAction::Bottom)) {
    return std::nullopt;
  }

  return static_cast<AttachScrollAction>(action);
}

std::optional<AttachCopyModeAction> parse_attach_copy_mode_payload(std::string_view payload) {
  if (payload.size() != 1) {
    return std::nullopt;
  }

  const auto action = static_cast<unsigned char>(payload[0]);
  switch (action) {
    case static_cast<unsigned char>(AttachCopyModeAction::Enter):
    case static_cast<unsigned char>(AttachCopyModeAction::Exit):
    case static_cast<unsigned char>(AttachCopyModeAction::CursorUp):
    case static_cast<unsigned char>(AttachCopyModeAction::CursorDown):
    case static_cast<unsigned char>(AttachCopyModeAction::CursorLeft):
    case static_cast<unsigned char>(AttachCopyModeAction::CursorRight):
    case static_cast<unsigned char>(AttachCopyModeAction::PageUp):
    case static_cast<unsigned char>(AttachCopyModeAction::PageDown):
    case static_cast<unsigned char>(AttachCopyModeAction::StartSelection):
    case static_cast<unsigned char>(AttachCopyModeAction::CopySelection):
    case static_cast<unsigned char>(AttachCopyModeAction::Home):
    case static_cast<unsigned char>(AttachCopyModeAction::End):
      return static_cast<AttachCopyModeAction>(action);
    default:
      return std::nullopt;
  }
}

std::string make_attach_paste_frame() {
  return make_attach_frame(AttachFrameType::Paste, {});
}

}  // namespace wmux
