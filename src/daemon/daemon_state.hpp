#pragma once

#include "wmux/config.hpp"
#include "wmux/ipc_protocol.hpp"
#include "wmux/mouse_input.hpp"
#include "wmux/paste_buffer.hpp"
#include "wmux/platform/pipe_handle.hpp"
#include "wmux/platform/pty_process.hpp"
#include "wmux/session_manager.hpp"
#include "wmux/status_line.hpp"
#include "wmux/terminal_capabilities.hpp"

#include <functional>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace wmux::daemon_internal {

enum class AttachEndReason {
  Detached,
  ClientDisconnected,
  ShellClosed,
  OutputClosed,
  ProtocolError,
};

enum class DaemonEventKind {
  AttachStart,
  ClientConnected,
  ClientDisconnected,
  ClientInput,
  DecodedKey,
  MouseEvent,
  MouseFocus,
  AttachCommand,
  CommandModeCommand,
  Paste,
  PasteBufferSet,
  IpcCommand,
  PaneOutput,
  PaneExited,
  ClientResize,
  Timer,
  Shutdown,
};

enum class DaemonTimerKind {
  ReapAttachWorkers,
  RenderTick,
};

struct DaemonKeyEvent {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

struct DaemonTimerEvent {
  DaemonTimerKind kind{DaemonTimerKind::RenderTick};
  std::uint64_t sequence{0};
};

struct DaemonAttachSettings {
  bool mouse_enabled{false};
  std::string prefix{"C-b"};
  bool status_bar_enabled{true};
  std::uint16_t escape_time_ms{50};
  UiTheme ui;
  ResourceLimits limits;
  std::string key_bindings;
};

enum class DiagnosticEventCategory {
  Command,
  Key,
  Process,
  ResizeLayout,
  Error,
};

struct DiagnosticMetadata {
  std::string key;
  std::string value;
};

struct DiagnosticEvent {
  std::uint64_t sequence{0};
  std::chrono::system_clock::time_point timestamp;
  DiagnosticEventCategory category{DiagnosticEventCategory::Command};
  std::string level{"info"};
  std::string event_type;
  RequestId request_id{0};
  ClientId client_id{0};
  SessionId session_id{0};
  WindowId window_id{0};
  PaneId pane_id{0};
  std::string message;
  std::vector<DiagnosticMetadata> metadata;
};

constexpr std::size_t kDiagnosticEventRingCapacity = 1000;

struct DaemonDiagnosticRings {
  std::uint64_t next_sequence{1};
  std::deque<DiagnosticEvent> commands;
  std::deque<DiagnosticEvent> keys;
  std::deque<DiagnosticEvent> processes;
  std::deque<DiagnosticEvent> resize_layout;
  std::deque<DiagnosticEvent> errors;
};

struct DaemonEvent {
  DaemonEventKind kind{DaemonEventKind::Timer};
  std::optional<ClientId> client_id;
  SessionId session_id{0};
  RequestId request_id{0};
  IpcRequest command;
  std::vector<std::uint8_t> bytes;
  std::string text;
  std::string session_name;
  DaemonKeyEvent key;
  MouseEvent mouse;
  AttachMouseFocusPayload focus;
  AttachMouseEventPayload attach_mouse;
  PaneId pane_id{0};
  std::int64_t exit_code{0};
  std::uint16_t columns{0};
  std::uint16_t rows{0};
  bool status_bar_enabled{true};
  bool reserve_status_row{false};
  DaemonTimerEvent timer;
  AttachEndReason attach_end_reason{AttachEndReason::ClientDisconnected};
  PasteBufferSource paste_source{PasteBufferSource::Unknown};
  PlatformPipeHandle pipe{kNullPlatformPipeHandle};

  static DaemonEvent attach_start(PlatformPipeHandle pipe, IpcRequest command);
  static DaemonEvent client_connected(ClientId client_id);
  static DaemonEvent client_disconnected(ClientId client_id, AttachEndReason reason);
  static DaemonEvent client_input(
      ClientId client_id,
      SessionId session_id,
      std::vector<std::uint8_t> bytes);
  static DaemonEvent decoded_key(ClientId client_id, DaemonKeyEvent key);
  static DaemonEvent mouse_event(ClientId client_id, MouseEvent mouse);
  static DaemonEvent attach_mouse_event(
      ClientId client_id,
      SessionId session_id,
      AttachMouseEventPayload mouse,
      std::uint16_t columns,
      std::uint16_t rows,
      bool reserve_status_row);
  static DaemonEvent mouse_focus(
      ClientId client_id,
      SessionId session_id,
      AttachMouseFocusPayload focus,
      std::uint16_t columns,
      std::uint16_t rows,
      bool reserve_status_row);
  static DaemonEvent attach_command(
      ClientId client_id,
      SessionId session_id,
      std::string command,
      std::uint16_t columns,
      std::uint16_t rows,
      bool status_bar_enabled);
  static DaemonEvent command_mode_command(
      ClientId client_id,
      SessionId session_id,
      std::string session_name,
      std::string command,
      std::uint16_t columns,
      std::uint16_t rows,
      bool status_bar_enabled);
  static DaemonEvent ipc_command(
      std::optional<ClientId> client_id,
      RequestId request_id,
      IpcRequest command);
  static DaemonEvent pane_output(PaneId pane_id, std::vector<std::uint8_t> bytes);
  static DaemonEvent pane_exited(PaneId pane_id, std::int64_t exit_code);
  static DaemonEvent client_resize(ClientId client_id, std::uint16_t columns, std::uint16_t rows);
  static DaemonEvent paste(ClientId client_id, SessionId session_id);
  static DaemonEvent set_paste_buffer(
      std::string text,
      PasteBufferSource source = PasteBufferSource::CopyMode);
  static DaemonEvent timer_event(DaemonTimerEvent timer);
  static DaemonEvent shutdown();
};

struct DaemonEventResult {
  bool ok{true};
  bool handled{false};
  bool has_response{false};
  std::string response;
  bool request_shutdown{false};
  bool changed{false};
  std::string status;
  std::string error;
  std::optional<ClientId> client_id;
  SessionId session_id{0};
  std::string session_name;
  DaemonAttachSettings attach_settings;
  std::shared_ptr<PtyProcess> shell;
  PaneId pane_id{0};
  std::string text;
};

std::string_view daemon_event_kind_name(DaemonEventKind kind);
bool daemon_state_mutation_allowed();
void assert_daemon_state_mutation_allowed(std::string_view source);

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
  std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};
  SessionManager sessions;
  DaemonConfigState config;
  bool mouse_enabled{false};
  PasteBuffer paste_buffer;
  BufferId next_paste_buffer_id{1};
  DaemonDiagnosticRings diagnostics;

  struct PaneRuntime {
    std::shared_ptr<PtyProcess> shell;
    short pty_columns{0};
    short pty_rows{0};
  };

  struct WindowRuntime {
    std::unordered_map<PaneId, PaneRuntime> panes;
  };

  struct SessionRuntime {
    std::unordered_map<WindowId, WindowRuntime> windows;
  };

  enum class ClientMode {
    Normal,
    Command,
    Copy,
  };

  struct TerminalSize {
    std::uint16_t columns{0};
    std::uint16_t rows{0};
  };

  struct RenderState {
    std::uint64_t last_frame_sequence{0};
    bool dirty{true};
  };

  struct Client {
    ClientId id{0};
    std::optional<SessionId> attached_session;
    std::optional<WindowId> active_window;
    std::optional<PaneId> active_pane;
    TerminalCapabilities terminal_caps;
    ClientMode mode{ClientMode::Normal};
    TerminalSize size;
    RenderState render_state;
    StatusState status;
    bool mouse_drag_active{false};
    PaneSplitResizeTarget mouse_drag_target;
  };

  struct AttachClientRuntime {
    Client client;
    std::string session_name;
    PlatformPipeHandle pipe{kNullPlatformPipeHandle};
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
  std::atomic<std::uint64_t> event_queue_depth{0};
  std::atomic<std::uint64_t> peak_event_queue_depth{0};

  struct RenderMetrics {
    std::atomic<std::uint64_t> frames_written{0};
    std::atomic<std::uint64_t> full_frames_written{0};
    std::atomic<std::uint64_t> partial_frames_written{0};
    std::atomic<std::uint64_t> skipped_frames{0};
    std::atomic<std::uint64_t> coalesced_output_events{0};
    std::atomic<std::uint64_t> dirty_panes_rendered{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> pending_pane_output_bytes{0};
    std::atomic<std::uint64_t> peak_pending_pane_output_bytes{0};
    std::atomic<std::uint64_t> pending_client_output_bytes{0};
    std::atomic<std::uint64_t> peak_pending_client_output_bytes{0};
    std::atomic<std::uint64_t> render_frame_duration_us{0};
    std::atomic<std::uint64_t> max_render_frame_duration_us{0};
    std::atomic<std::uint64_t> slow_clients{0};
    std::atomic<std::uint64_t> write_failures{0};
    std::atomic<std::uint64_t> client_resize_events{0};
    std::atomic<std::uint64_t> client_resize_noops{0};
    std::atomic<std::uint64_t> pty_resize_requests{0};
    std::atomic<std::uint64_t> pty_resize_applied{0};
    std::atomic<std::uint64_t> pty_resize_skipped{0};
    std::atomic<std::uint64_t> pty_resize_failures{0};
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
  void set_handler(std::function<DaemonEventResult(DaemonState&, const DaemonEvent&)> handler);
  DaemonEventResult call_event(DaemonEvent event);
  void post_event(DaemonEvent event);

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
  std::function<DaemonEventResult(DaemonState&, const DaemonEvent&)> handler_;
  std::mutex queue_mutex_;
  std::condition_variable queue_changed_;
  std::deque<Event> queue_;
  std::thread worker_;
  std::thread::id worker_id_;
  bool stopping_{false};
};

DaemonEventResult handle_daemon_event(DaemonState& state, const DaemonEvent& event);

struct DaemonStats {
  std::size_t session_count{0};
  std::size_t attach_client_count{0};
  std::size_t runtime_session_count{0};
  std::size_t runtime_window_count{0};
  std::size_t runtime_pane_count{0};
  std::size_t live_shell_count{0};
  std::size_t terminating_shell_count{0};
  std::size_t job_object_shell_count{0};
  std::size_t degraded_cleanup_shell_count{0};
  std::size_t attach_worker_count{0};
  std::uint64_t uptime_seconds{0};
  std::uint64_t event_queue_depth{0};
  std::uint64_t peak_event_queue_depth{0};
  std::uint64_t render_frames_written{0};
  std::uint64_t render_full_frames_written{0};
  std::uint64_t render_partial_frames_written{0};
  std::uint64_t render_skipped_frames{0};
  std::uint64_t render_coalesced_output_events{0};
  std::uint64_t render_dirty_panes{0};
  std::uint64_t render_bytes_written{0};
  std::uint64_t render_pending_pane_output_bytes{0};
  std::uint64_t render_peak_pending_pane_output_bytes{0};
  std::uint64_t render_pending_client_output_bytes{0};
  std::uint64_t render_peak_pending_client_output_bytes{0};
  std::uint64_t render_frame_duration_us{0};
  std::uint64_t render_max_frame_duration_us{0};
  std::uint64_t render_slow_clients{0};
  std::uint64_t render_write_failures{0};
  std::uint64_t client_resize_events{0};
  std::uint64_t client_resize_noops{0};
  std::uint64_t pty_resize_requests{0};
  std::uint64_t pty_resize_applied{0};
  std::uint64_t pty_resize_skipped{0};
  std::uint64_t pty_resize_failures{0};
  BufferId paste_buffer_id{0};
  std::size_t paste_buffer_bytes{0};
  std::size_t paste_buffer_original_bytes{0};
  bool paste_buffer_truncated{false};
  PasteBufferSource paste_buffer_source{PasteBufferSource::Unknown};
  bool mouse_enabled{false};
  DaemonConfigState config;
  std::size_t recent_error_count{0};
};

std::string_view diagnostic_event_category_name(DiagnosticEventCategory category);
std::vector<DiagnosticEvent> diagnostic_events_snapshot(
    const DaemonDiagnosticRings& rings,
    DiagnosticEventCategory category);
void record_diagnostic_event_locked(DaemonState& state, DiagnosticEvent event);
void record_diagnostic_event(DaemonState& state, DiagnosticEvent event);
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
void install_pane_runtime_shell_locked(
    DaemonState& state,
    SessionId session_id,
    WindowId window_id,
    PaneId pane_id,
    std::shared_ptr<PtyProcess> shell);
std::vector<std::shared_ptr<PtyProcess>> take_all_shells(DaemonState& state);
void sync_attach_client_focus_locked(DaemonState& state, SessionId session_id);

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id);
void disconnect_all_attach_clients(DaemonState& state);

}  // namespace wmux::daemon_internal
