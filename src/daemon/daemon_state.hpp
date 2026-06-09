#pragma once

#include "wmux/config.hpp"
#include "wmux/pty_process.hpp"
#include "wmux/session_manager.hpp"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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

struct DaemonConfigState {
  Config values;
  std::filesystem::path path;
  bool file_exists{false};
  std::vector<ConfigParseError> errors;
};

struct DaemonState {
  std::mutex mutex;
  std::condition_variable attach_clients_changed;
  std::condition_variable attach_workers_changed;
  SessionManager sessions;
  DaemonConfigState config;
  bool mouse_enabled{false};
  std::string paste_buffer;

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

  struct AttachWorkerRuntime {
    ClientId client_id{0};
    std::shared_ptr<std::atomic_bool> done;
    std::thread thread;
  };

  std::unordered_map<SessionId, SessionRuntime> runtimes;
  std::unordered_map<ClientId, AttachClientRuntime> attach_clients;
  std::vector<AttachWorkerRuntime> attach_workers;
  ClientId next_client_id{1};

  struct RenderMetrics {
    std::atomic<std::uint64_t> frames_written{0};
    std::atomic<std::uint64_t> full_frames_written{0};
    std::atomic<std::uint64_t> partial_frames_written{0};
    std::atomic<std::uint64_t> skipped_frames{0};
    std::atomic<std::uint64_t> dirty_panes_rendered{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> write_failures{0};
  };

  RenderMetrics render_metrics;
};

class DaemonEventLoop {
 public:
  explicit DaemonEventLoop(DaemonState& state);
  DaemonEventLoop(const DaemonEventLoop&) = delete;
  DaemonEventLoop& operator=(const DaemonEventLoop&) = delete;
  ~DaemonEventLoop();

  void start();
  void stop();
  void notify_attach_workers_changed();

  template <typename Fn>
  auto call(Fn&& fn) -> std::invoke_result_t<Fn, DaemonState&> {
    using Result = std::invoke_result_t<Fn, DaemonState&>;

    if (on_event_thread()) {
      if constexpr (std::is_void_v<Result>) {
        std::forward<Fn>(fn)(state_);
        return;
      } else {
        return std::forward<Fn>(fn)(state_);
      }
    }

    auto task = std::make_shared<std::packaged_task<Result()>>(
        [this, fn = std::forward<Fn>(fn)]() mutable -> Result {
          if constexpr (std::is_void_v<Result>) {
            fn(state_);
            return;
          } else {
            return fn(state_);
          }
        });
    auto future = task->get_future();
    enqueue(Event{[task](DaemonState&) mutable { (*task)(); }});

    if constexpr (std::is_void_v<Result>) {
      future.get();
      return;
    } else {
      return future.get();
    }
  }

  template <typename Fn>
  void post(Fn&& fn) {
    enqueue(Event{[fn = std::forward<Fn>(fn)](DaemonState& state) mutable { fn(state); }});
  }

 private:
  using Event = std::packaged_task<void(DaemonState&)>;

  bool on_event_thread() const;
  void enqueue(Event event);
  void run();

  DaemonState& state_;
  std::mutex queue_mutex_;
  std::condition_variable queue_changed_;
  std::deque<Event> queue_;
  std::thread worker_;
  std::thread::id worker_id_;
  bool stopping_{false};
};

struct DaemonStats {
  std::size_t session_count{0};
  std::size_t attach_client_count{0};
  std::size_t runtime_session_count{0};
  std::size_t runtime_window_count{0};
  std::size_t runtime_pane_count{0};
  std::size_t live_shell_count{0};
  std::size_t attach_worker_count{0};
  std::uint64_t render_frames_written{0};
  std::uint64_t render_full_frames_written{0};
  std::uint64_t render_partial_frames_written{0};
  std::uint64_t render_skipped_frames{0};
  std::uint64_t render_dirty_panes{0};
  std::uint64_t render_bytes_written{0};
  std::uint64_t render_write_failures{0};
  bool mouse_enabled{false};
  DaemonConfigState config;
};

struct DaemonAttachSettings {
  bool mouse_enabled{false};
  std::string prefix{"C-b"};
  bool status_bar_enabled{true};
};

std::string quoted(std::string_view value);
std::string session_error_message(SessionError error, std::string_view name);
std::string window_error_message(WindowError error, std::string_view name);
std::string pane_error_message(PaneError error);
void apply_daemon_config(
    DaemonState& state,
    std::filesystem::path path,
    ConfigParseResult result,
    bool file_exists);
void load_daemon_config(DaemonState& state);
DaemonStats daemon_stats(DaemonState& state);
DaemonAttachSettings daemon_attach_settings(DaemonState& state);
void set_daemon_mouse_enabled(DaemonState& state, bool enabled);
std::vector<std::shared_ptr<PtyProcess>> take_all_shells(DaemonState& state);

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id);
void disconnect_all_attach_clients(DaemonState& state);

}  // namespace wmux::daemon_internal
