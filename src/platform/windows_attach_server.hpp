#pragma once

#include "wmux/ipc_protocol.hpp"
#include "wmux/platform/pipe_handle.hpp"

#include "daemon_state.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
namespace wmux::daemon_internal {

enum class AttachDispatch {
  NotAttach,
  Completed,
  HandedOff,
};

std::wstring widen(std::string_view value);
bool write_all(PlatformPipeHandle pipe, std::string_view bytes);
void close_pipe(PlatformPipeHandle pipe);
void close_attach_pipe(PlatformPipeHandle pipe);
PlatformPipeHandle create_server_pipe(
    const std::wstring& endpoint,
    std::uint32_t open_mode_flags);
bool connect_named_pipe(PlatformPipeHandle pipe);
std::optional<DaemonEventResult> handle_attach_daemon_event(
    DaemonState& state,
    const DaemonEvent& event);
AttachDispatch dispatch_attach_connection(
    PlatformPipeHandle pipe,
    const IpcRequest& request,
    RequestId request_id,
    DaemonEventLoop& events);
void run_windows_attach_listener(DaemonEventLoop& events, std::atomic_bool& should_stop);
void wake_attach_listener();
bool wait_for_no_attach_clients(DaemonState& state, std::chrono::milliseconds timeout);
void reap_finished_attach_workers(DaemonState& state);
void join_all_attach_workers(DaemonState& state);

}  // namespace wmux::daemon_internal
#endif
