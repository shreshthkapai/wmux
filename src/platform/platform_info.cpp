#include "wmux/platform/platform_info.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace wmux {
namespace {

std::string trim_ascii(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
          value[first] == '\n')) {
    ++first;
  }

  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' ||
          value[last - 1] == '\n')) {
    --last;
  }

  return std::string{value.substr(first, last - first)};
}

std::string first_command_token(std::string_view command_line) {
  const auto trimmed = trim_ascii(command_line);
  if (trimmed.empty()) {
    return {};
  }

  if (trimmed.front() == '"') {
    const auto end = trimmed.find('"', 1);
    if (end != std::string::npos) {
      return trimmed.substr(1, end - 1);
    }
  }

  const auto end = trimmed.find_first_of(" \t\r\n");
  return end == std::string::npos ? trimmed : trimmed.substr(0, end);
}

std::string quoted_command_path(std::string_view path) {
  std::string quoted{"\""};
  quoted += path;
  quoted += "\"";
  return quoted;
}

#ifdef _WIN32
std::wstring widen(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int required = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (required <= 0) {
    return {};
  }

  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
  return wide;
}

std::string narrow(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  const int required = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }

  std::string result(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      required,
      nullptr,
      nullptr);
  return result;
}

std::optional<std::string> search_executable(std::string_view name) {
  const auto wide_name = widen(name);
  if (wide_name.empty()) {
    return std::nullopt;
  }

  DWORD required = SearchPathW(
      nullptr,
      wide_name.c_str(),
      L".exe",
      0,
      nullptr,
      nullptr);
  if (required == 0) {
    return std::nullopt;
  }

  std::wstring buffer(static_cast<std::size_t>(required) + 1, L'\0');
  DWORD written = SearchPathW(
      nullptr,
      wide_name.c_str(),
      L".exe",
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (written == 0 || written >= buffer.size()) {
    return std::nullopt;
  }

  buffer.resize(written);
  return narrow(buffer);
}
#endif

PlatformShellResolution configured_shell_resolution(
    std::string_view command_line,
    std::string_view source,
    std::string working_directory) {
  const auto trimmed = trim_ascii(command_line);
  return PlatformShellResolution{
      trimmed,
      first_command_token(trimmed),
      std::string{source},
      std::move(working_directory)};
}

PlatformShellResolution fallback_shell_resolution(std::string working_directory) {
  return PlatformShellResolution{
      "powershell.exe -NoLogo -NoProfile",
      "powershell.exe",
      "fallback",
      std::move(working_directory)};
}

}  // namespace

std::string platform_environment_variable(std::string_view name) {
  const std::string key{name};
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, key.c_str()) != 0 || value == nullptr) {
    return {};
  }

  std::string result{value};
  std::free(value);
  return result;
#else
  if (const char* value = std::getenv(key.c_str()); value != nullptr) {
    return value;
  }
  return {};
#endif
}

std::string platform_current_working_directory() {
  std::error_code error;
  const auto cwd = std::filesystem::current_path(error);
  return error ? std::string{} : cwd.string();
}

PlatformShellResolution platform_resolve_shell_command(std::string_view configured_shell) {
  auto cwd = platform_current_working_directory();
  if (auto configured = trim_ascii(configured_shell); !configured.empty()) {
    return configured_shell_resolution(configured, "config", std::move(cwd));
  }

  if (auto environment = platform_environment_variable("WMUX_DEFAULT_SHELL");
      !trim_ascii(environment).empty()) {
    return configured_shell_resolution(environment, "environment", std::move(cwd));
  }

#ifdef _WIN32
  if (const auto pwsh = search_executable("pwsh.exe")) {
    return PlatformShellResolution{
        quoted_command_path(*pwsh) + " -NoLogo -NoProfile",
        *pwsh,
        "pwsh",
        std::move(cwd)};
  }

  if (const auto powershell = search_executable("powershell.exe")) {
    return PlatformShellResolution{
        quoted_command_path(*powershell) + " -NoLogo -NoProfile",
        *powershell,
        "windows-powershell",
        std::move(cwd)};
  }

  if (const auto cmd = search_executable("cmd.exe")) {
    return PlatformShellResolution{
        quoted_command_path(*cmd),
        *cmd,
        "cmd",
        std::move(cwd)};
  }
#endif

  return fallback_shell_resolution(std::move(cwd));
}

bool platform_has_console_output() {
#ifdef _WIN32
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (output == nullptr || output == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD mode = 0;
  return GetConsoleMode(output, &mode) != 0;
#else
  return false;
#endif
}

bool platform_has_interactive_console() {
#ifdef _WIN32
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (input == nullptr || input == INVALID_HANDLE_VALUE || output == nullptr ||
      output == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD mode = 0;
  return GetConsoleMode(input, &mode) != 0 && GetConsoleMode(output, &mode) != 0;
#else
  return false;
#endif
}

std::string platform_os_version() {
#ifdef _WIN32
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return "unknown";
  }

  const auto rtl_get_version =
      reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (rtl_get_version == nullptr) {
    return "unknown";
  }

  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  if (rtl_get_version(&version) != 0) {
    return "unknown";
  }

  return std::to_string(version.dwMajorVersion) + "." +
         std::to_string(version.dwMinorVersion) + "." +
         std::to_string(version.dwBuildNumber);
#else
  return "unsupported";
#endif
}

bool platform_pty_available() {
#ifdef _WIN32
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  return kernel32 != nullptr && GetProcAddress(kernel32, "CreatePseudoConsole") != nullptr;
#else
  return false;
#endif
}

PlatformConsoleModeProbe platform_probe_console_mode(PlatformConsoleStream stream) {
  PlatformConsoleModeProbe probe;
#ifdef _WIN32
  const HANDLE handle =
      GetStdHandle(stream == PlatformConsoleStream::Input ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return probe;
  }

  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode) == 0) {
    return probe;
  }

  probe.available = true;
  probe.mode = mode;
  if (stream == PlatformConsoleStream::Input) {
    probe.vt_enabled = (mode & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0;
  } else {
    probe.vt_enabled = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
  }
#else
  (void)stream;
#endif
  return probe;
}

std::string_view platform_clipboard_backend_name() {
#ifdef _WIN32
  return "windows-clipboard";
#else
  return "unsupported";
#endif
}

PlatformProcessResourceSnapshot platform_current_process_resources() {
  PlatformProcessResourceSnapshot snapshot;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters)) != 0) {
    snapshot.memory_available = true;
    snapshot.working_set_bytes = static_cast<unsigned long long>(counters.WorkingSetSize);
    snapshot.private_bytes = static_cast<unsigned long long>(counters.PrivateUsage);
  }

  DWORD handle_count = 0;
  if (GetProcessHandleCount(GetCurrentProcess(), &handle_count) != 0) {
    snapshot.handle_count_available = true;
    snapshot.handle_count = static_cast<unsigned long>(handle_count);
  }
#endif
  return snapshot;
}

}  // namespace wmux
