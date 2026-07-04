#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool is_source_file(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  return extension == ".cpp" || extension == ".hpp" || extension == ".h";
}

std::string generic_slash_path(const std::filesystem::path& path) {
  auto value = path.generic_string();
  for (auto& ch : value) {
    if (ch == '\\') {
      ch = '/';
    }
  }
  return value;
}

bool is_platform_owned_path(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = generic_slash_path(std::filesystem::relative(path, root));
  return relative.starts_with("src/platform/") ||
         relative.starts_with("include/wmux/platform/");
}

std::filesystem::path find_repo_root() {
  std::vector<std::filesystem::path> candidates;

  std::filesystem::path source_file{__FILE__};
  if (source_file.is_relative()) {
    source_file = std::filesystem::absolute(source_file);
  }
  candidates.push_back(source_file.parent_path().parent_path());

  auto current = std::filesystem::current_path();
  for (int i = 0; i < 8 && !current.empty(); ++i) {
    candidates.push_back(current);
    current = current.parent_path();
  }

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate / "CMakeLists.txt") &&
        std::filesystem::exists(candidate / "src") &&
        std::filesystem::exists(candidate / "include")) {
      return candidate;
    }
  }

  return {};
}

bool contains_forbidden_token(std::string_view text, std::string_view token) {
  const std::regex expression{"\\b" + std::string{token} + "\\b"};
  return std::regex_search(text.begin(), text.end(), expression);
}

void platform_boundary_keeps_win32_out_of_core_sources() {
  const auto root = find_repo_root();
  assert(!root.empty());

  const std::vector<std::string> forbidden_includes{
      "#include <windows.h>",
      "#include <Windows.h>",
      "#include <sddl.h>",
      "#include <tlhelp32.h>",
      "#include <psapi.h>",
  };

  const std::vector<std::string> forbidden_tokens{
      "HANDLE",
      "HPCON",
      "DWORD",
      "BOOL",
      "CreateProcess",
      "CreatePseudoConsole",
      "ResizePseudoConsole",
      "ClosePseudoConsole",
      "CreateNamedPipe",
      "ConnectNamedPipe",
      "WaitNamedPipe",
      "OpenClipboard",
      "SetClipboardData",
      "GetClipboardData",
      "SetConsoleCtrlHandler",
      "GetStdHandle",
      "GetConsoleMode",
      "SetConsoleMode",
      "ReadFile",
      "WriteFile",
      "CloseHandle",
      "WaitForSingleObject",
      "WaitForMultipleObjects",
      "CreateJobObject",
      "AssignProcessToJobObject",
      "TerminateJobObject",
      "GenerateConsoleCtrlEvent",
  };

  const std::vector<std::string> forbidden_platform_calls{
      "platform_environment_variable(",
      "platform_current_working_directory(",
      "platform_resolve_shell_command(",
      "platform_has_interactive_console(",
      "platform_has_console_output(",
      "platform_os_version(",
      "platform_pty_available(",
      "platform_probe_console_mode(",
      "platform_clipboard_backend_name(",
      "platform_current_process_resources(",
      "PtyProcess::start(",
      "write_clipboard_text(",
      "terminal_reset_sequence(",
      "terminal_attach_enter_sequence(",
  };

  std::vector<std::string> violations;
  for (const auto& tree : {root / "src", root / "include"}) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tree)) {
      if (!entry.is_regular_file() || !is_source_file(entry.path()) ||
          is_platform_owned_path(root, entry.path())) {
        continue;
      }

      std::ifstream input{entry.path(), std::ios::binary};
      const std::string text{
          std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
      for (const auto& include : forbidden_includes) {
        if (text.find(include) != std::string::npos) {
          violations.push_back(
              generic_slash_path(std::filesystem::relative(entry.path(), root)) +
              ": forbidden platform include " + include);
        }
      }
      for (const auto& token : forbidden_tokens) {
        if (contains_forbidden_token(text, token)) {
          violations.push_back(
              generic_slash_path(std::filesystem::relative(entry.path(), root)) +
              ": forbidden Win32 token " + token);
        }
      }
      for (const auto& call : forbidden_platform_calls) {
        if (text.find(call) != std::string::npos) {
          violations.push_back(
              generic_slash_path(std::filesystem::relative(entry.path(), root)) +
              ": call platform service through platform_services(), not " + call);
        }
      }
    }
  }

  if (!violations.empty()) {
    for (const auto& violation : violations) {
      std::cerr << violation << "\n";
    }
    assert(false);
  }
}

void removed_platform_shim_headers_stay_removed() {
  const auto root = find_repo_root();
  assert(!root.empty());

  for (const auto& relative : {
           "include/wmux/pty_process.hpp",
           "include/wmux/ipc_transport.hpp",
           "include/wmux/terminal_control.hpp",
           "include/wmux/windows_clipboard.hpp",
       }) {
    if (std::filesystem::exists(root / relative)) {
      std::cerr << relative << ": removed platform compatibility shim was reintroduced\n";
      assert(false);
    }
  }
}

void cmake_target_split_stays_explicit() {
  const auto root = find_repo_root();
  assert(!root.empty());

  std::ifstream input{root / "CMakeLists.txt", std::ios::binary};
  const std::string text{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};

  for (const auto& required : {
           "add_library(wmux_core STATIC",
           "add_library(wmux_platform_windows STATIC",
           "add_library(wmux_daemon STATIC",
           "add_library(wmux_app_support STATIC",
           "add_library(wmux_windows_runtime STATIC",
       }) {
    if (text.find(required) == std::string::npos) {
      std::cerr << "CMakeLists.txt: missing required boundary target " << required << "\n";
      assert(false);
    }
  }

  for (const auto& forbidden : {
           "${WMUX_CORE_SOURCES}",
           "${WMUX_DAEMON_SOURCES}",
           "${WMUX_PLATFORM_WINDOWS_SOURCES}",
           "${WMUX_WINDOWS_RUNTIME_SOURCES}",
           "${WMUX_APP_SUPPORT_SOURCES}",
       }) {
    const auto executable_pos = text.find("add_executable(wmux");
    const auto tests_pos = text.find("add_executable(wmux_tests");
    if (executable_pos != std::string::npos) {
      const auto executable_end = text.find(")", executable_pos);
      const auto body = text.substr(executable_pos, executable_end - executable_pos);
      if (body.find(forbidden) != std::string::npos) {
        std::cerr << "CMakeLists.txt: wmux executable must link library targets, not "
                  << forbidden << "\n";
        assert(false);
      }
    }
    if (tests_pos != std::string::npos) {
      const auto tests_end = text.find(")", tests_pos);
      const auto body = text.substr(tests_pos, tests_end - tests_pos);
      if (body.find(forbidden) != std::string::npos) {
        std::cerr << "CMakeLists.txt: wmux_tests must link library targets, not "
                  << forbidden << "\n";
        assert(false);
      }
    }
  }
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  return {
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
}

std::string cmake_list_body(std::string_view cmake, std::string_view list_name) {
  const std::string start = "set(" + std::string{list_name};
  const auto start_pos = cmake.find(start);
  if (start_pos == std::string_view::npos) {
    return {};
  }
  const auto body_start = cmake.find('\n', start_pos);
  if (body_start == std::string_view::npos) {
    return {};
  }
  const auto body_end = cmake.find("\n)", body_start);
  if (body_end == std::string_view::npos) {
    return {};
  }
  return std::string{cmake.substr(body_start, body_end - body_start)};
}

void terminal_engine_v2_stays_platform_neutral_core_code() {
  const auto root = find_repo_root();
  assert(!root.empty());

  const std::vector<std::string> engine_files{
      "include/wmux/terminal_engine.hpp",
      "src/core/terminal_engine_legacy.cpp",
      "src/core/terminal_engine_v2.cpp",
      "src/core/terminal_engine_v2_internal.hpp",
      "src/core/vt_parser_v2.cpp",
      "src/core/screen_writer_v2.cpp",
      "src/core/grid_core_v2.cpp",
  };

  const auto cmake = read_text_file(root / "CMakeLists.txt");
  const auto core_sources = cmake_list_body(cmake, "WMUX_CORE_SOURCES");
  const auto platform_sources = cmake_list_body(cmake, "WMUX_PLATFORM_WINDOWS_SOURCES");
  const auto runtime_sources = cmake_list_body(cmake, "WMUX_WINDOWS_RUNTIME_SOURCES");

  for (const auto& relative : engine_files) {
    const auto path = root / relative;
    if (!std::filesystem::exists(path)) {
      std::cerr << relative << ": terminal engine boundary file is missing\n";
      assert(false);
    }

    if (relative.ends_with(".cpp") && core_sources.find(relative) == std::string::npos) {
      std::cerr << relative << ": terminal engine implementation must belong to WMUX_CORE_SOURCES\n";
      assert(false);
    }
    if (platform_sources.find(relative) != std::string::npos ||
        runtime_sources.find(relative) != std::string::npos) {
      std::cerr << relative << ": terminal engine implementation must not belong to a Windows target\n";
      assert(false);
    }

    const auto text = read_text_file(path);
    for (const auto& forbidden : {
             "#include <windows.h>",
             "#include <Windows.h>",
             "wmux/platform/",
             "HANDLE",
             "HPCON",
             "ConPTY",
             "ReadFile",
             "WriteFile",
             "CreatePseudoConsole",
             "ResizePseudoConsole",
             "ClosePseudoConsole",
         }) {
      if (text.find(forbidden) != std::string::npos) {
        std::cerr << relative << ": V2 terminal engine core leaked platform detail "
                  << forbidden << "\n";
        assert(false);
      }
    }
  }
}

}  // namespace

void run_platform_boundary_tests() {
  platform_boundary_keeps_win32_out_of_core_sources();
  removed_platform_shim_headers_stay_removed();
  cmake_target_split_stays_explicit();
  terminal_engine_v2_stays_platform_neutral_core_code();
}
