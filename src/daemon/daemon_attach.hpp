#pragma once

#include "daemon_state.hpp"
#include "wmux/ipc_protocol.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wmux::daemon_internal {

enum class AttachDispatch {
  NotAttach,
  Completed,
  HandedOff,
};

std::wstring widen(std::string_view value);
bool read_request(HANDLE pipe, std::string& request);
bool write_all(HANDLE pipe, std::string_view bytes);
void close_pipe(HANDLE pipe);
void close_attach_pipe(HANDLE pipe);
HANDLE create_server_pipe(const std::wstring& endpoint, DWORD open_mode_flags);
bool connect_named_pipe(HANDLE pipe);
AttachDispatch dispatch_attach_connection(
    HANDLE pipe,
    const IpcRequest& request,
    DaemonEventLoop& events);
void run_windows_attach_listener(DaemonEventLoop& events, std::atomic_bool& should_stop);
void wake_attach_listener();
bool wait_for_no_attach_clients(DaemonState& state, std::chrono::milliseconds timeout);
void reap_finished_attach_workers(DaemonState& state);
void join_all_attach_workers(DaemonState& state);

}  // namespace wmux::daemon_internal
#endif
