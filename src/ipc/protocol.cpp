#include "wmux/ipc_protocol.hpp"

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

}  // namespace wmux
