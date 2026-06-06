#include "wmux/logging.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace wmux {
namespace {

std::mutex g_log_mutex;
std::ofstream g_log_file;
LogRole g_role = LogRole::Client;
bool g_initialized = false;

std::string role_name(LogRole role) {
  switch (role) {
    case LogRole::Client:
      return "client";
    case LogRole::Daemon:
      return "daemon";
  }

  return "unknown";
}

std::string level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
  }

  return "unknown";
}

std::filesystem::path fallback_log_root() {
  std::error_code ec;
  auto path = std::filesystem::temp_directory_path(ec);
  if (ec || path.empty()) {
    path = ".";
  }
  return path / "wmux" / "logs";
}

#ifdef _WIN32
std::string environment_variable(std::string_view name) {
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, std::string{name}.c_str()) != 0 || value == nullptr) {
    return {};
  }

  std::string result{value};
  std::free(value);
  return result;
}
#endif

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);

  std::tm local_time{};
#ifdef _WIN32
  localtime_s(&local_time, &time);
#else
  localtime_r(&time, &local_time);
#endif

  std::ostringstream out;
  out << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S") << '.'
      << std::setw(3) << std::setfill('0') << millis;
  return out.str();
}

std::string escaped(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

void open_log_file_locked(LogRole role) {
  std::error_code ec;
  const auto directory = log_directory();
  std::filesystem::create_directories(directory, ec);
  g_log_file.open(log_file_path(role), std::ios::app);
}

}  // namespace

void initialize_logging(LogRole role) {
  std::lock_guard lock(g_log_mutex);
  g_role = role;
  if (!g_initialized || !g_log_file.is_open()) {
    open_log_file_locked(role);
  }
  g_initialized = true;
}

void shutdown_logging() {
  std::lock_guard lock(g_log_mutex);
  if (g_log_file.is_open()) {
    g_log_file.flush();
    g_log_file.close();
  }
  g_initialized = false;
}

std::filesystem::path log_directory() {
#ifdef _WIN32
  const auto local_app_data = environment_variable("LOCALAPPDATA");
  if (!local_app_data.empty()) {
    return std::filesystem::path{local_app_data} / "wmux" / "logs";
  }
#endif

  return fallback_log_root();
}

std::filesystem::path log_file_path(LogRole role) {
  return log_directory() / ("wmux-" + role_name(role) + ".log");
}

void log_event(
    LogLevel level,
    std::string_view component,
    std::string_view event,
    std::initializer_list<LogField> fields) {
  std::lock_guard lock(g_log_mutex);
  if (!g_initialized) {
    open_log_file_locked(g_role);
    g_initialized = true;
  }

  if (!g_log_file.is_open()) {
    return;
  }

  g_log_file << "ts=\"" << timestamp() << "\""
             << " level=\"" << level_name(level) << "\""
             << " role=\"" << role_name(g_role) << "\""
             << " component=\"" << escaped(component) << "\""
             << " event=\"" << escaped(event) << "\"";

  for (const auto& field : fields) {
    g_log_file << ' ' << field.key << "=\"" << escaped(field.value) << "\"";
  }

  g_log_file << '\n';
  g_log_file.flush();
}

}  // namespace wmux
