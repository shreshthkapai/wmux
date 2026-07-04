#include "wmux/logging.hpp"

#include "wmux/resource_limits.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
std::size_t g_max_file_bytes = kMaxLogFileBytes;
std::atomic<int> g_min_log_level{1};

int level_rank(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return 0;
    case LogLevel::Info:
      return 1;
    case LogLevel::Warn:
      return 2;
    case LogLevel::Error:
      return 3;
  }

  return 1;
}

LogLevel level_from_rank(int rank) {
  switch (rank) {
    case 0:
      return LogLevel::Debug;
    case 1:
      return LogLevel::Info;
    case 2:
      return LogLevel::Warn;
    case 3:
      return LogLevel::Error;
    default:
      return LogLevel::Info;
  }
}

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
#else
std::string environment_variable(std::string_view name) {
  const auto* value = std::getenv(std::string{name}.c_str());
  return value == nullptr ? std::string{} : std::string{value};
}
#endif

bool parse_log_level(std::string_view value, LogLevel& out) {
  std::string normalized{value};
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  if (normalized == "debug" || normalized == "trace") {
    out = LogLevel::Debug;
    return true;
  }
  if (normalized == "info") {
    out = LogLevel::Info;
    return true;
  }
  if (normalized == "warn" || normalized == "warning") {
    out = LogLevel::Warn;
    return true;
  }
  if (normalized == "error") {
    out = LogLevel::Error;
    return true;
  }
  return false;
}

void configure_log_level_from_environment() {
  const auto configured = environment_variable("WMUX_LOG_LEVEL");
  if (configured.empty()) {
    return;
  }

  LogLevel level = LogLevel::Info;
  if (parse_log_level(configured, level)) {
    g_min_log_level.store(level_rank(level), std::memory_order_relaxed);
  }
}

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

std::uintmax_t file_size_or_zero(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : size;
}

void rotate_log_file_locked(LogRole role, std::size_t pending_bytes) {
  if (g_max_file_bytes == 0) {
    return;
  }

  const auto active_path = log_file_path(role);
  const auto current_size = file_size_or_zero(active_path);
  if (current_size + pending_bytes <= g_max_file_bytes) {
    return;
  }

  if (g_log_file.is_open()) {
    g_log_file.flush();
    g_log_file.close();
  }

  std::error_code ec;
  const auto rotated_path = rotated_log_file_path(role);
  std::filesystem::remove(rotated_path, ec);
  ec.clear();
  if (std::filesystem::exists(active_path, ec)) {
    ec.clear();
    std::filesystem::rename(active_path, rotated_path, ec);
  }

  open_log_file_locked(role);
}

}  // namespace

void initialize_logging(LogRole role) {
  std::lock_guard lock(g_log_mutex);
  g_role = role;
  configure_log_level_from_environment();
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

void configure_logging(std::size_t max_file_bytes) {
  std::lock_guard lock(g_log_mutex);
  g_max_file_bytes = max_file_bytes;
  if (g_initialized) {
    rotate_log_file_locked(g_role, 0);
  }
}

void set_log_level(LogLevel level) {
  g_min_log_level.store(level_rank(level), std::memory_order_relaxed);
}

LogLevel current_log_level() {
  return level_from_rank(g_min_log_level.load(std::memory_order_relaxed));
}

bool should_log(LogLevel level) {
  return level_rank(level) >= g_min_log_level.load(std::memory_order_relaxed);
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

std::filesystem::path rotated_log_file_path(LogRole role) {
  return log_file_path(role).string() + ".1";
}

void log_event(
    LogLevel level,
    std::string_view component,
    std::string_view event,
    std::initializer_list<LogField> fields) {
  if (!should_log(level)) {
    return;
  }

  std::lock_guard lock(g_log_mutex);
  if (!g_initialized) {
    open_log_file_locked(g_role);
    g_initialized = true;
  }

  if (!g_log_file.is_open()) {
    return;
  }

  std::ostringstream line;
  line << "ts=\"" << timestamp() << "\""
       << " level=\"" << level_name(level) << "\""
       << " role=\"" << role_name(g_role) << "\""
       << " component=\"" << escaped(component) << "\""
       << " event=\"" << escaped(event) << "\"";

  for (const auto& field : fields) {
    line << ' ' << field.key << "=\"" << escaped(field.value) << "\"";
  }

  line << '\n';
  const auto text = line.str();
  rotate_log_file_locked(g_role, text.size());
  if (!g_log_file.is_open()) {
    return;
  }

  g_log_file << text;
  if (level == LogLevel::Warn || level == LogLevel::Error) {
    g_log_file.flush();
  }
}

}  // namespace wmux
