#include "wmux/ipc_protocol.hpp"

#include <cstdint>
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

std::optional<std::string> find_json_string(std::string_view json, std::string_view key) {
  const std::string pattern = "\"" + std::string{key} + "\":\"";
  const auto start = json.find(pattern);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }

  const auto value_start = start + pattern.size();
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
      return unescape_json_string(json.substr(value_start, i - value_start));
    }
  }

  return std::nullopt;
}

std::optional<bool> find_json_bool(std::string_view json, std::string_view key) {
  const std::string pattern = "\"" + std::string{key} + "\":";
  const auto start = json.find(pattern);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }

  const auto value_start = start + pattern.size();
  if (json.substr(value_start, 4) == "true") {
    return true;
  }
  if (json.substr(value_start, 5) == "false") {
    return false;
  }

  return std::nullopt;
}

std::optional<std::uint16_t> find_json_uint(std::string_view json, std::string_view key) {
  const std::string pattern = "\"" + std::string{key} + "\":";
  const auto start = json.find(pattern);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }

  const auto value_start = start + pattern.size();
  std::uint32_t value = 0;
  bool saw_digit = false;
  for (std::size_t i = value_start; i < json.size(); ++i) {
    const char ch = json[i];
    if (ch < '0' || ch > '9') {
      break;
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
    case CommandKind::ServerStatus:
      return "ServerStatus";
    case CommandKind::ServerStop:
      return "ServerStop";
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

}  // namespace

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
  if (command.force) {
    append_json_bool(out, "force", command.force);
  }

  out << "}\n";
  return out.str();
}

std::string make_attach_request_json(
    const CommandLine& command,
    std::uint16_t terminal_columns,
    std::uint16_t terminal_rows) {
  std::ostringstream out;
  out << "{\"type\":\"AttachSession\"";

  if (!command.session_name.empty()) {
    append_json_field(out, "session_name", command.session_name);
  }
  if (terminal_columns > 0 && terminal_rows > 0) {
    append_json_uint(out, "terminal_columns", terminal_columns);
    append_json_uint(out, "terminal_rows", terminal_rows);
  }

  out << "}\n";
  return out.str();
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
  if (const auto force = find_json_bool(json, "force")) {
    request.force = *force;
  }
  if (const auto terminal_columns = find_json_uint(json, "terminal_columns")) {
    request.terminal_columns = *terminal_columns;
  }
  if (const auto terminal_rows = find_json_uint(json, "terminal_rows")) {
    request.terminal_rows = *terminal_rows;
  }

  return request;
}

std::string make_response_json(bool ok, std::string_view message) {
  std::ostringstream out;
  out << "{\"ok\":" << (ok ? "true" : "false");
  append_json_field(out, "message", message);
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
  return response;
}

std::string make_attach_input_frame(std::string_view bytes) {
  return make_attach_frame(AttachFrameType::Input, bytes);
}

std::string make_attach_detach_frame() {
  return make_attach_frame(AttachFrameType::Detach, {});
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
    default:
      return std::nullopt;
  }

  parsed.payload_size =
      static_cast<std::uint32_t>(static_cast<unsigned char>(header[3])) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[4])) << 8) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[5])) << 16) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[6])) << 24);
  if (parsed.payload_size > kMaxAttachFramePayloadSize) {
    return std::nullopt;
  }

  return parsed;
}

}  // namespace wmux
