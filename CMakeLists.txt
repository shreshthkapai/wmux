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

set(WMUX_COMMON_SOURCES
  src/commands/commands.cpp
  src/core/command_mode.cpp
  src/core/config.cpp
  src/core/copy_selection.cpp
  src/core/logging.cpp
  src/core/mouse_input.cpp
  src/core/paste_buffer.cpp
  src/core/session_manager.cpp
  src/core/terminal_control.cpp
  src/core/terminal_grid.cpp
  src/core/version.cpp
  src/ipc/protocol.cpp
)

set(WMUX_RUNTIME_SOURCES
  src/client/attach_client.cpp
  src/daemon/daemon_attach.cpp
  src/daemon/daemon_commands.cpp
  src/daemon/daemon.cpp
  src/daemon/daemon_render.cpp
  src/daemon/daemon_shell.cpp
  src/daemon/daemon_state.cpp
  src/ipc/transport.cpp
  src/platform/windows_clipboard.cpp
  src/pty/pty_process.cpp
)

add_executable(wmux
  src/main.cpp
  ${WMUX_COMMON_SOURCES}
  ${WMUX_RUNTIME_SOURCES}
)

target_include_directories(wmux PRIVATE include)
target_link_libraries(wmux PRIVATE "${WMUX_CLI11_TARGET}" "${WMUX_SPDLOG_TARGET}")
target_compile_definitions(wmux PRIVATE
  WMUX_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
  target_compile_definitions(wmux PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(wmux PRIVATE user32)
endif()

if(MSVC)
  target_compile_options(wmux PRIVATE /W4 /permissive-)
else()
  target_compile_options(wmux PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WMUX_BUILD_TESTS)
  enable_testing()

  add_executable(wmux_tests
    tests/command_mode_test.cpp
    tests/command_parser_test.cpp
    tests/config_test.cpp
    tests/copy_selection_test.cpp
    tests/daemon_render_test.cpp
    tests/ipc_protocol_test.cpp
    tests/mouse_input_test.cpp
    tests/paste_buffer_test.cpp
    tests/session_manager_test.cpp
    tests/terminal_grid_test.cpp
    tests/windows_clipboard_test.cpp
    ${WMUX_COMMON_SOURCES}
    src/daemon/daemon_render.cpp
    src/platform/windows_clipboard.cpp
  )
  target_include_directories(wmux_tests PRIVATE include src/daemon)
  target_link_libraries(wmux_tests PRIVATE "${WMUX_CLI11_TARGET}" "${WMUX_SPDLOG_TARGET}")
  target_compile_definitions(wmux_tests PRIVATE
    WMUX_VERSION="${PROJECT_VERSION}"
  )

  if(WIN32)
    target_compile_definitions(wmux_tests PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(wmux_tests PRIVATE user32)
  endif()

  if(MSVC)
    target_compile_options(wmux_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(wmux_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  add_test(NAME wmux.command_parser COMMAND wmux_tests)
endif()
