cmake_minimum_required(VERSION 3.24)

project(
  wmux
  VERSION 0.1.0
  DESCRIPTION "Native Windows terminal multiplexer"
  LANGUAGES CXX
)

option(WMUX_FETCH_DEPS "Fetch CLI11 and spdlog with CMake FetchContent when not installed" OFF)
option(WMUX_BUILD_TESTS "Build wmux tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

find_package(CLI11 CONFIG QUIET)
if(CLI11_FOUND)
  set(WMUX_CLI11_TARGET CLI11::CLI11)
elseif(WMUX_FETCH_DEPS)
  FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
  )
  FetchContent_MakeAvailable(CLI11)
  set(WMUX_CLI11_TARGET CLI11::CLI11)
else()
  add_library(wmux_cli11_compat INTERFACE)
  target_include_directories(wmux_cli11_compat INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/compat")
  set(WMUX_CLI11_TARGET wmux_cli11_compat)
  message(WARNING "CLI11 was not found; using a narrow compatibility header. Configure with -DWMUX_FETCH_DEPS=ON or install CLI11 for the real dependency.")
endif()

find_package(spdlog CONFIG QUIET)
if(spdlog_FOUND)
  set(WMUX_SPDLOG_TARGET spdlog::spdlog)
elseif(WMUX_FETCH_DEPS)
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
  )
  FetchContent_MakeAvailable(spdlog)
  set(WMUX_SPDLOG_TARGET spdlog::spdlog)
else()
  add_library(wmux_spdlog_compat INTERFACE)
  target_include_directories(wmux_spdlog_compat INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/compat")
  set(WMUX_SPDLOG_TARGET wmux_spdlog_compat)
  message(WARNING "spdlog was not found; using a narrow compatibility header. Configure with -DWMUX_FETCH_DEPS=ON or install spdlog for the real dependency.")
endif()

function(wmux_apply_common_target_settings target_name)
  target_compile_definitions("${target_name}" PRIVATE
    WMUX_VERSION="${PROJECT_VERSION}"
  )

  if(WIN32)
    target_compile_definitions("${target_name}" PRIVATE _WIN32_WINNT=0x0A00)
  endif()

  if(MSVC)
    target_compile_options("${target_name}" PRIVATE /W4 /permissive-)
  else()
    target_compile_options("${target_name}" PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

set(WMUX_CORE_SOURCES
  src/commands/commands.cpp
  src/core/attach_input_mode.cpp
  src/core/attach_keymap.cpp
  src/core/command_engine.cpp
  src/core/command_mode.cpp
  src/core/config.cpp
  src/core/copy_selection.cpp
  src/core/logging.cpp
  src/core/mouse_input.cpp
  src/core/paste_buffer.cpp
  src/core/session_manager.cpp
  src/core/status_line.cpp
  src/core/terminal_capabilities.cpp
  src/core/terminal_input.cpp
  src/core/terminal_grid.cpp
  src/core/terminal_vt.cpp
  src/core/unicode_width.cpp
  src/core/version.cpp
  src/ipc/protocol.cpp
)

set(WMUX_DAEMON_SOURCES
  src/daemon/daemon_command_engine.cpp
  src/daemon/daemon_commands.cpp
  src/daemon/daemon_render.cpp
  src/daemon/daemon_shell.cpp
  src/daemon/daemon_state.cpp
)

set(WMUX_PLATFORM_WINDOWS_SOURCES
  src/platform/ipc_transport.cpp
  src/platform/platform_info.cpp
  src/platform/platform_services.cpp
  src/platform/terminal_control.cpp
  src/platform/windows_clipboard.cpp
  src/platform/windows_pty_process.cpp
)

set(WMUX_WINDOWS_RUNTIME_SOURCES
  src/platform/daemon_server.cpp
  src/platform/windows_attach_client.cpp
  src/platform/windows_attach_server.cpp
)

set(WMUX_APP_SUPPORT_SOURCES
  src/core/doctor.cpp
)

add_library(wmux_core STATIC ${WMUX_CORE_SOURCES})
target_include_directories(wmux_core PUBLIC include)
target_link_libraries(wmux_core PUBLIC "${WMUX_CLI11_TARGET}" "${WMUX_SPDLOG_TARGET}")
wmux_apply_common_target_settings(wmux_core)

add_library(wmux_platform_windows STATIC ${WMUX_PLATFORM_WINDOWS_SOURCES})
target_include_directories(wmux_platform_windows PUBLIC include)
target_link_libraries(wmux_platform_windows PUBLIC wmux_core)
wmux_apply_common_target_settings(wmux_platform_windows)

if(WIN32)
  target_link_libraries(wmux_platform_windows PUBLIC user32 advapi32 psapi)
endif()

add_library(wmux_daemon STATIC ${WMUX_DAEMON_SOURCES})
target_include_directories(wmux_daemon PUBLIC include src/daemon)
target_link_libraries(wmux_daemon PUBLIC wmux_core PRIVATE wmux_platform_windows)
wmux_apply_common_target_settings(wmux_daemon)

add_library(wmux_app_support STATIC ${WMUX_APP_SUPPORT_SOURCES})
target_include_directories(wmux_app_support PUBLIC include)
target_link_libraries(wmux_app_support PUBLIC wmux_core PRIVATE wmux_platform_windows)
wmux_apply_common_target_settings(wmux_app_support)

add_library(wmux_windows_runtime STATIC ${WMUX_WINDOWS_RUNTIME_SOURCES})
target_include_directories(wmux_windows_runtime PUBLIC include src/daemon)
target_link_libraries(
  wmux_windows_runtime
  PUBLIC
    wmux_app_support
    wmux_core
    wmux_daemon
    wmux_platform_windows
)
wmux_apply_common_target_settings(wmux_windows_runtime)

add_executable(wmux
  src/main.cpp
)

target_link_libraries(wmux PRIVATE wmux_windows_runtime)
wmux_apply_common_target_settings(wmux)

if(WMUX_BUILD_TESTS)
  enable_testing()

  add_executable(wmux_tests
    tests/attach_keymap_test.cpp
    tests/attach_input_mode_test.cpp
    tests/command_mode_test.cpp
    tests/command_engine_test.cpp
    tests/command_parser_test.cpp
    tests/config_test.cpp
    tests/copy_selection_test.cpp
    tests/daemon_event_test.cpp
    tests/daemon_render_test.cpp
    tests/ipc_protocol_test.cpp
    tests/mouse_input_test.cpp
    tests/paste_buffer_test.cpp
    tests/platform_boundary_test.cpp
    tests/platform_info_test.cpp
    tests/session_manager_test.cpp
    tests/status_line_test.cpp
    tests/terminal_capabilities_test.cpp
    tests/terminal_control_test.cpp
    tests/terminal_input_test.cpp
    tests/terminal_grid_test.cpp
    tests/terminal_vt_test.cpp
    tests/windows_clipboard_test.cpp
  )
  target_link_libraries(
    wmux_tests
    PRIVATE
      wmux_app_support
      wmux_core
      wmux_daemon
      wmux_platform_windows
  )
  wmux_apply_common_target_settings(wmux_tests)

  add_test(NAME wmux.command_parser COMMAND wmux_tests)
endif()
