#pragma once

#include "wmux/pty_process.hpp"
#include "wmux/session_manager.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wmux::daemon_internal {

using ClientId = std::uint64_t;

enum class AttachEndReason {
  Detached,
  ClientDisconnected,
  ShellClosed,
  OutputClosed,
  ProtocolError,
};

struct DaemonState {
  std::mutex mutex;
  std::condition_variable attach_clients_changed;
  SessionManager sessions;
  bool mouse_enabled{false};

  struct PaneRuntime {
    std::shared_ptr<PtyProcess> shell;
  };

  struct WindowRuntime {
    std::unordered_map<PaneId, PaneRuntime> panes;
  };

  struct SessionRuntime {
    std::unordered_map<WindowId, WindowRuntime> windows;
  };

  struct AttachClientRuntime {
    SessionId session_id{0};
    std::string session_name;
#ifdef _WIN32
    HANDLE pipe{nullptr};
#endif
  };

  std::unordered_map<SessionId, SessionRuntime> runtimes;
  std::unordered_map<ClientId, AttachClientRuntime> attach_clients;
  ClientId next_client_id{1};
};

struct DaemonStats {
  std::size_t session_count{0};
  std::size_t attach_client_count{0};
  bool mouse_enabled{false};
};

std::string quoted(std::string_view value);
std::string session_error_message(SessionError error, std::string_view name);
std::string window_error_message(WindowError error, std::string_view name);
std::string pane_error_message(PaneError error);
DaemonStats daemon_stats(DaemonState& state);
bool daemon_mouse_enabled(DaemonState& state);
void set_daemon_mouse_enabled(DaemonState& state, bool enabled);
std::vector<std::shared_ptr<PtyProcess>> take_all_shells(DaemonState& state);

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id);
void disconnect_all_attach_clients(DaemonState& state);

}  // namespace wmux::daemon_internal
