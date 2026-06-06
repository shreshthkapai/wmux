#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace wmux {

enum class LogRole {
  Client,
  Daemon,
};

enum class LogLevel {
  Debug,
  Info,
  Warn,
  Error,
};

struct LogField {
  std::string_view key;
  std::string value;
};

void initialize_logging(LogRole role);
void shutdown_logging();
std::filesystem::path log_directory();
std::filesystem::path log_file_path(LogRole role);
void log_event(
    LogLevel level,
    std::string_view component,
    std::string_view event,
    std::initializer_list<LogField> fields = {});

}  // namespace wmux
