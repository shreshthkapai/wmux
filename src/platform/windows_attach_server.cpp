#include "windows_attach_server.hpp"

#include "daemon_command_engine.hpp"
#include "daemon_render.hpp"
#include "daemon_shell.hpp"
#include "wmux/attach_keymap.hpp"
#include "wmux/command_mode.hpp"
#include "wmux/commands.hpp"
#include "wmux/platform/ipc_transport.hpp"
#include "wmux/logging.hpp"
#include "wmux/paste_buffer.hpp"
#include "wmux/platform/pty_process.hpp"
#include "wmux/resource_limits.hpp"
#include "wmux/platform/services.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace wmux::daemon_internal {

#ifdef _WIN32
namespace {

constexpr auto kRequestReadTimeout = std::chrono::seconds{5};
constexpr auto kRequestReadPoll = std::chrono::milliseconds{10};
constexpr auto kOutputRedrawInterval = std::chrono::milliseconds{16};
constexpr auto kV2IdleOutputPoll = std::chrono::milliseconds{2};
constexpr auto kV2SlowOutputRedrawInterval = std::chrono::milliseconds{4};
constexpr auto kV2VerySlowOutputRedrawInterval = std::chrono::milliseconds{8};
constexpr auto kV2BurstQuietWindow = std::chrono::milliseconds{24};
constexpr auto kV2BurstMaxPresentationInterval = std::chrono::milliseconds{250};
constexpr auto kHighOutputRedrawInterval = std::chrono::milliseconds{100};
constexpr auto kVeryHighOutputRedrawInterval = std::chrono::milliseconds{250};
constexpr std::size_t kHighOutputPendingBytes = 4 * 1024;
constexpr std::size_t kVeryHighOutputPendingBytes = 16 * 1024;
constexpr std::size_t kBacklogRenderPendingBytes = 16 * 1024;
constexpr std::size_t kBacklogRenderCoalescedEvents = 32;
constexpr std::size_t kV2BurstFastForwardPendingBytes = 32 * 1024;
constexpr std::size_t kV2BurstFastForwardCoalescedEvents = 4;
constexpr auto kSlowClientWriteTimeout = std::chrono::seconds{5};
// Match tmux's practical behavior: splitting is bounded by visible geometry,
// not by a high arbitrary pane-size cap. With borders enabled, a 3x3 pane is
// the smallest rectangle that still has one drawable interior cell.
constexpr int kMinimumInteractivePaneBodyColumns = 1;
constexpr int kMinimumInteractivePaneBodyRows = 1;
constexpr int kPaneBorderColumns = 2;
constexpr int kPaneBorderRows = 2;

enum class SceneInvalidationPolicy {
  InitialAttach,
  FullScene,
  SceneDelta,
  LayoutOnly,
  OutputDelta,
  LatestViewport,
};

enum class AttachRenderMode {
  Auto,
  FullLatestOnly,
  DirtyRowDiff,
  BacklogFullFrame,
};

struct AttachTarget {
  SessionId session_id{0};
  std::string session_name;
};

struct ActiveShell {
  WindowId window_id{0};
  PaneId pane_id{0};
  std::shared_ptr<PtyProcess> shell;
};

enum class AttachWriteKind {
  Output,
  Event,
  Error,
};

struct QueuedAttachWrite {
  std::string frame;
  AttachWriteKind kind{AttachWriteKind::Output};
  bool partial_frame{false};
  std::size_t dirty_pane_count{0};
  bool establishes_baseline{false};
  std::optional<RenderDiffState> baseline_after_write;
  std::optional<std::unordered_map<PaneId, std::uint64_t>> sequences_after_write;
};

class LocalHandle {
 public:
  LocalHandle() = default;
  explicit LocalHandle(HANDLE handle) : handle_(handle) {}
  LocalHandle(const LocalHandle&) = delete;
  LocalHandle& operator=(const LocalHandle&) = delete;

  LocalHandle(LocalHandle&& other) noexcept : handle_(other.release()) {}

  LocalHandle& operator=(LocalHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~LocalHandle() {
    reset();
  }

  HANDLE get() const {
    return handle_;
  }

  bool valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  HANDLE release() {
    const auto handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  void reset(HANDLE handle = nullptr) {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_{nullptr};
};

enum class PipeIoResult {
  Ok,
  Closed,
  Timeout,
  Failed,
};

void update_atomic_peak(std::atomic<std::uint64_t>& target, std::uint64_t value) {
  auto observed = target.load(std::memory_order_relaxed);
  while (observed < value &&
         !target.compare_exchange_weak(
             observed, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

std::uint64_t elapsed_us(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, micros));
}

std::string environment_variable_value(const char* name) {
  const auto required = GetEnvironmentVariableA(name, nullptr, 0);
  if (required == 0) {
    return {};
  }

  std::string value(required, '\0');
  const auto written = GetEnvironmentVariableA(name, value.data(), required);
  if (written == 0 || written >= required) {
    return {};
  }

  value.resize(written);
  return value;
}

AttachRenderMode attach_render_mode_from_environment() {
  const auto mode = environment_variable_value("WMUX_RENDER_MODE");
  if (mode.empty()) {
    return AttachRenderMode::Auto;
  }
  if (mode == "full" || mode == "full-latest-only" || mode == "full_latest_only") {
    return AttachRenderMode::FullLatestOnly;
  }
  if (mode == "dirty" || mode == "dirty-row" || mode == "dirty-row-diff" ||
      mode == "dirty_row_diff") {
    return AttachRenderMode::DirtyRowDiff;
  }
  if (mode == "backlog" || mode == "backlog-full" || mode == "backlog_full_frame") {
    return AttachRenderMode::BacklogFullFrame;
  }
  return AttachRenderMode::Auto;
}

std::string_view attach_render_mode_name(AttachRenderMode mode) {
  switch (mode) {
    case AttachRenderMode::Auto:
      return "auto";
    case AttachRenderMode::FullLatestOnly:
      return "full";
    case AttachRenderMode::DirtyRowDiff:
      return "dirty-row-diff";
    case AttachRenderMode::BacklogFullFrame:
      return "backlog-full";
  }
  return "auto";
}

std::string_view scene_invalidation_policy_name(SceneInvalidationPolicy policy) {
  switch (policy) {
    case SceneInvalidationPolicy::InitialAttach:
      return "initial-attach";
    case SceneInvalidationPolicy::FullScene:
      return "full-scene";
    case SceneInvalidationPolicy::SceneDelta:
      return "scene-delta";
    case SceneInvalidationPolicy::LayoutOnly:
      return "layout-only";
    case SceneInvalidationPolicy::OutputDelta:
      return "output-delta";
    case SceneInvalidationPolicy::LatestViewport:
      return "latest-viewport";
  }
  return "unknown";
}

bool render_mode_uses_diff(AttachRenderMode mode) {
  // Dirty-row diffing is currently a diagnostic mode only. The full/latest
  // path is the safe default until cache invalidation has stronger coverage.
  return mode == AttachRenderMode::DirtyRowDiff;
}

bool render_mode_allows_dirty_row_snapshots(AttachRenderMode mode) {
  return mode == AttachRenderMode::DirtyRowDiff;
}

std::chrono::milliseconds redraw_interval_for_pending_bytes(std::size_t pending_bytes) {
  if (pending_bytes >= kVeryHighOutputPendingBytes) {
    return kVeryHighOutputRedrawInterval;
  }
  if (pending_bytes >= kHighOutputPendingBytes) {
    return kHighOutputRedrawInterval;
  }
  return kOutputRedrawInterval;
}

std::chrono::milliseconds output_redraw_interval_for_mode(
    AttachRenderMode mode,
    std::size_t pending_bytes,
    std::uint64_t last_render_micros) {
  const auto base = redraw_interval_for_pending_bytes(pending_bytes);
  if (last_render_micros >= 50'000) {
    return std::max(base, std::chrono::milliseconds{250});
  }
  if (last_render_micros >= 16'000) {
    return std::max(base, std::chrono::milliseconds{100});
  }
  if (mode != AttachRenderMode::DirtyRowDiff) {
    return std::max(base, std::chrono::milliseconds{33});
  }
  return base;
}

std::chrono::milliseconds v2_smooth_output_redraw_interval(std::uint64_t last_render_micros) {
  if (last_render_micros >= 25'000) {
    return kV2VerySlowOutputRedrawInterval;
  }
  if (last_render_micros >= 12'000) {
    return kV2SlowOutputRedrawInterval;
  }
  return std::chrono::milliseconds{0};
}

std::optional<AttachTarget> target_for_attach(
    DaemonState& state,
    const IpcRequest& request,
    std::string& error) {
  if (request.session_name.empty()) {
    error = session_error_message(SessionError::EmptyName, {});
    return {};
  }

  std::lock_guard lock(state.mutex);
  const auto session_id = state.sessions.session_id_for_name(request.session_name);
  if (!session_id) {
    error = session_error_message(SessionError::NotFound, request.session_name);
    return {};
  }

  return AttachTarget{*session_id, request.session_name};
}

std::optional<ActiveShell> active_shell_for_session(
    DaemonState& state,
    SessionId session_id,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto active_window = state.sessions.active_window_id(session_id);
  if (!active_window) {
    error = "wmux: session has no active window\n";
    return {};
  }
  const auto active_pane = state.sessions.active_pane_id(session_id);
  if (!active_pane) {
    error = "wmux: active window has no active pane\n";
    return {};
  }

  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    error = "wmux: session has no runtime\n";
    return {};
  }

  const auto window = runtime->second.windows.find(*active_window);
  if (window == runtime->second.windows.end()) {
    error = "wmux: active window has no runtime\n";
    return {};
  }

  const auto pane = window->second.panes.find(*active_pane);
  if (pane == window->second.panes.end() || !pane->second.shell) {
    error = "wmux: active pane has no shell process\n";
    return {};
  }

  return ActiveShell{*active_window, *active_pane, pane->second.shell};
}

bool resource_limits_equal(const ResourceLimits& lhs, const ResourceLimits& rhs) {
  return lhs.max_sessions == rhs.max_sessions &&
         lhs.max_windows_per_session == rhs.max_windows_per_session &&
         lhs.max_panes_per_window == rhs.max_panes_per_window &&
         lhs.max_pane_raw_output_bytes == rhs.max_pane_raw_output_bytes &&
         lhs.max_pane_scrollback_lines == rhs.max_pane_scrollback_lines &&
         lhs.max_paste_buffer_bytes == rhs.max_paste_buffer_bytes &&
         lhs.max_ipc_frame_payload_bytes == rhs.max_ipc_frame_payload_bytes &&
         lhs.max_client_output_queue_bytes == rhs.max_client_output_queue_bytes &&
         lhs.max_client_output_queue_frames == rhs.max_client_output_queue_frames &&
         lhs.max_attach_render_frame_bytes == rhs.max_attach_render_frame_bytes &&
         lhs.max_log_file_bytes == rhs.max_log_file_bytes;
}

bool attach_settings_equal(
    const DaemonAttachSettings& lhs,
    const DaemonAttachSettings& rhs) {
  return lhs.mouse_enabled == rhs.mouse_enabled &&
         lhs.synchronized_output_supported == rhs.synchronized_output_supported &&
         lhs.prefix == rhs.prefix &&
         lhs.status_bar_enabled == rhs.status_bar_enabled &&
         lhs.escape_time_ms == rhs.escape_time_ms &&
         lhs.ui.inherit_terminal_theme == rhs.ui.inherit_terminal_theme &&
         lhs.ui.tmux_style == rhs.ui.tmux_style &&
         lhs.ui.smooth_borders == rhs.ui.smooth_borders &&
         lhs.ui.accent_spec == rhs.ui.accent_spec &&
         lhs.key_bindings == rhs.key_bindings &&
         resource_limits_equal(lhs.limits, rhs.limits);
}

DaemonAttachSettings attach_settings_for_client(DaemonState& state, ClientId client_id) {
  assert_daemon_state_mutation_allowed("attach_settings_for_client");
  std::lock_guard lock(state.mutex);

  DaemonAttachSettings settings{
      state.mouse_enabled,
      false,
      state.config.values.prefix,
      state.config.values.status_bar_enabled,
      state.config.values.escape_time_ms,
      state.config.values.ui,
      state.config.values.limits,
      serialize_attach_key_binding_overrides(state.config.values.keys.bindings)};

  const auto client = state.attach_clients.find(client_id);
  if (client != state.attach_clients.end() &&
      !client->second.client.terminal_caps.supports_sgr_mouse) {
    settings.mouse_enabled = false;
  }
  if (client != state.attach_clients.end()) {
    settings.synchronized_output_supported =
        client->second.client.terminal_caps.supports_synchronized_output;
  }

  return settings;
}

std::optional<ActiveWindowFrame> active_window_frame(
    DaemonState& state,
    SessionId session_id,
    short columns,
    short rows,
    bool status_bar_enabled,
    bool reserve_status_row,
    std::string& error) {
  ActiveWindowFrame frame;
  frame.columns = columns > 0 ? columns : 120;
  frame.rows = rows > 0 ? rows : 30;
  frame.status_bar_enabled = status_bar_enabled;

  std::lock_guard lock(state.mutex);
  for (const auto& session : state.sessions.list_sessions()) {
    if (session.id == session_id) {
      frame.session_name = session.name;
      break;
    }
  }
  const auto window = state.sessions.active_window_summary(session_id);
  if (!window) {
    error = "wmux: session has no active window\n";
    return {};
  }

  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    error = "wmux: session has no runtime\n";
    return {};
  }

  const auto window_runtime = runtime->second.windows.find(window->id);
  if (window_runtime == runtime->second.windows.end()) {
    error = "wmux: active window has no runtime\n";
    return {};
  }

  const int pane_rows =
      std::max(1, frame.rows - (reserve_status_row && frame.rows > 1 ? 1 : 0));
  frame.pane_rows = pane_rows;
  const auto rects = compute_pane_layout_rects(window->pane_tree, frame.columns, pane_rows);

  frame.window_id = window->id;
  frame.layout_generation = window_runtime->second.layout_generation;
  frame.active_pane_id = window->active_pane_id;
  frame.window_name = window->name;
  frame.window_index = window->index;
  frame.panes.reserve(rects.size());
  for (const auto& rect : rects) {
    const auto pane_runtime = window_runtime->second.panes.find(rect.pane_id);
    if (pane_runtime == window_runtime->second.panes.end() || !pane_runtime->second.shell) {
      continue;
    }

    frame.panes.push_back(RenderPane{
        rect,
        rect.pane_id == window->active_pane_id,
        pane_runtime->second.shell});
  }

  if (frame.panes.empty()) {
    error = "wmux: active window has no pane runtimes\n";
    return {};
  }

  return frame;
}

short bounded_attach_columns(std::uint16_t value) {
  if (value == 0) {
    return 0;
  }
  return static_cast<short>(std::min<std::uint16_t>(value, kMaxAttachTerminalColumns));
}

short bounded_attach_rows(std::uint16_t value) {
  if (value == 0) {
    return 0;
  }
  return static_cast<short>(std::min<std::uint16_t>(value, kMaxAttachTerminalRows));
}

void run_attach_connection(
    HANDLE pipe,
    DaemonEventLoop& events,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    RequestId attach_request_id,
    short columns,
    short rows,
    DaemonAttachSettings settings);

ClientId register_attach_client(
    DaemonState& state,
    SessionId session_id,
    std::string session_name,
    HANDLE pipe,
    std::uint16_t columns,
    std::uint16_t rows,
    const TerminalCapabilities& terminal_capabilities,
    bool terminal_capabilities_provided) {
  assert_daemon_state_mutation_allowed("register_attach_client");
  std::lock_guard lock(state.mutex);
  const ClientId client_id = state.next_client_id++;
  const auto registered_session_name = session_name;
  DaemonState::AttachClientRuntime client;
  client.client.id = client_id;
  client.client.attached_session = session_id;
  client.client.active_window = state.sessions.active_window_id(session_id);
  client.client.active_pane = state.sessions.active_pane_id(session_id);
  client.client.size.columns = columns;
  client.client.size.rows = rows;
  const auto detected_capabilities =
      terminal_capabilities_provided ? terminal_capabilities : detect_terminal_capabilities();
  client.client.terminal_caps = apply_terminal_capability_overrides(
      detected_capabilities,
      state.config.values.terminal_overrides);
  const auto terminal_host = terminal_host_name(client.client.terminal_caps.host);
  client.session_name = std::move(session_name);
  client.pipe = pipe;
  state.attach_clients.emplace(client_id, std::move(client));
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "register",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", registered_session_name},
       {"terminal_host", terminal_host}});
  return client_id;
}

std::string attach_end_reason_name(AttachEndReason reason) {
  switch (reason) {
    case AttachEndReason::Detached:
      return "detached";
    case AttachEndReason::ClientDisconnected:
      return "client_disconnected";
    case AttachEndReason::ShellClosed:
      return "shell_closed";
    case AttachEndReason::OutputClosed:
      return "output_closed";
    case AttachEndReason::ProtocolError:
      return "protocol_error";
  }

  return "unknown";
}

bool is_attach_start_request(std::string_view type) {
  return type == "AttachStart" || type == "AttachSession";
}

void unregister_attach_client(
    DaemonState& state,
    ClientId client_id,
    AttachEndReason reason) {
  assert_daemon_state_mutation_allowed("unregister_attach_client");
  SessionId session_id{0};
  std::string session_name;
  {
    std::lock_guard lock(state.mutex);
    if (const auto client = state.attach_clients.find(client_id);
        client != state.attach_clients.end()) {
      session_id = client->second.client.attached_session.value_or(0);
      session_name = client->second.session_name;
    }
    state.attach_clients.erase(client_id);
  }
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "unregister",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", session_name},
       {"reason", attach_end_reason_name(reason)}});
  log_event(
      LogLevel::Info,
      "daemon.attach",
      "lifecycle",
      {{"event", reason == AttachEndReason::Detached ? "Detach" : "AttachEnd"},
       {"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", session_name},
       {"reason", attach_end_reason_name(reason)}});
  state.attach_clients_changed.notify_all();
}

void start_attach_worker(
    DaemonEventLoop& events,
    HANDLE pipe,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    RequestId attach_request_id,
    short columns,
    short rows,
    DaemonAttachSettings settings) {
  auto done = std::make_shared<std::atomic_bool>(false);
  std::thread worker{[pipe,
                      &events,
                      client_id,
                      session_id,
                      session_name = std::move(session_name),
                      attach_request_id,
                      columns,
                      rows,
                      settings,
                      done] {
    run_attach_connection(
        pipe,
        events,
        client_id,
        session_id,
        session_name,
        attach_request_id,
        columns,
        rows,
        settings);
    done->store(true, std::memory_order_release);
    events.notify_attach_workers_changed();
  }};

  events.call([&](DaemonState& state) {
    state.attach_workers.push_back(DaemonState::AttachWorkerRuntime{
        client_id,
        done,
        std::move(worker)});
  });

  log_event(
      LogLevel::Info,
      "daemon.attach",
      "worker_start",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)}});
}

void join_workers(
    std::vector<std::pair<ClientId, std::thread>>& workers,
    std::string_view event) {
  for (auto& [client_id, worker] : workers) {
    if (worker.joinable()) {
      worker.join();
      log_event(
          LogLevel::Info,
          "daemon.attach",
          event,
          {{"client_id", std::to_string(client_id)}});
    }
  }
}

std::vector<std::pair<ClientId, std::thread>> take_finished_attach_workers(
    DaemonState& state) {
  assert_daemon_state_mutation_allowed("take_finished_attach_workers");
  std::vector<std::pair<ClientId, std::thread>> workers;
  std::lock_guard lock(state.mutex);

  auto it = state.attach_workers.begin();
  while (it != state.attach_workers.end()) {
    if (it->done && it->done->load(std::memory_order_acquire)) {
      workers.emplace_back(it->client_id, std::move(it->thread));
      it = state.attach_workers.erase(it);
    } else {
      ++it;
    }
  }

  return workers;
}

std::vector<HANDLE> attach_client_pipes_for_session(DaemonState& state, SessionId session_id) {
  std::vector<HANDLE> pipes;
  std::lock_guard lock(state.mutex);
  for (const auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.client.attached_session == session_id && client.pipe != nullptr) {
      pipes.push_back(client.pipe);
    }
  }
  return pipes;
}

std::vector<HANDLE> all_attach_client_pipes(DaemonState& state) {
  std::vector<HANDLE> pipes;
  std::lock_guard lock(state.mutex);
  pipes.reserve(state.attach_clients.size());
  for (const auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.pipe != nullptr) {
      pipes.push_back(client.pipe);
    }
  }
  return pipes;
}

void disconnect_attach_pipes(const std::vector<HANDLE>& pipes) {
  for (const auto pipe : pipes) {
    CancelIoEx(pipe, nullptr);
    DisconnectNamedPipe(pipe);
  }
}

bool is_closed_pipe_error(DWORD error) {
  return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED ||
         error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
}

PipeIoResult wait_for_overlapped_result(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    HANDLE event,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    const std::atomic_bool* stop_requested,
    DWORD& bytes_transferred,
    DWORD& error_code) {
  while (true) {
    if (stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed)) {
      CancelIoEx(pipe, &overlapped);
      WaitForSingleObject(event, 1000);
      error_code = ERROR_OPERATION_ABORTED;
      return PipeIoResult::Closed;
    }

    DWORD wait_ms = 25;
    if (deadline) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= *deadline) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(event, 1000);
        error_code = ERROR_TIMEOUT;
        return PipeIoResult::Timeout;
      }

      const auto remaining_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count();
      wait_ms = static_cast<DWORD>(
          std::clamp<std::int64_t>(remaining_ms, 1, static_cast<std::int64_t>(25)));
    }

    const DWORD wait_result = WaitForSingleObject(event, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
      continue;
    }
    if (wait_result != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &overlapped);
      WaitForSingleObject(event, 1000);
      error_code = GetLastError();
      return PipeIoResult::Failed;
    }

    if (GetOverlappedResult(pipe, &overlapped, &bytes_transferred, FALSE)) {
      return PipeIoResult::Ok;
    }

    error_code = GetLastError();
    return is_closed_pipe_error(error_code) ? PipeIoResult::Closed : PipeIoResult::Failed;
  }
}

PipeIoResult read_overlapped_chunk(
    HANDLE pipe,
    char* buffer,
    DWORD byte_count,
    const std::atomic_bool* stop_requested,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    DWORD& bytes_read,
    DWORD& error_code) {
  LocalHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.valid()) {
    error_code = GetLastError();
    return PipeIoResult::Failed;
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  bytes_read = 0;
  error_code = ERROR_SUCCESS;

  if (ReadFile(pipe, buffer, byte_count, nullptr, &overlapped)) {
    if (GetOverlappedResult(pipe, &overlapped, &bytes_read, FALSE)) {
      return bytes_read == 0 ? PipeIoResult::Closed : PipeIoResult::Ok;
    }

    error_code = GetLastError();
    return is_closed_pipe_error(error_code) ? PipeIoResult::Closed : PipeIoResult::Failed;
  }

  error_code = GetLastError();
  if (error_code != ERROR_IO_PENDING) {
    return is_closed_pipe_error(error_code) ? PipeIoResult::Closed : PipeIoResult::Failed;
  }

  const auto result = wait_for_overlapped_result(
      pipe, overlapped, event.get(), deadline, stop_requested, bytes_read, error_code);
  return result == PipeIoResult::Ok && bytes_read == 0 ? PipeIoResult::Closed : result;
}

PipeIoResult write_overlapped_chunk(
    HANDLE pipe,
    const char* buffer,
    DWORD byte_count,
    std::chrono::steady_clock::time_point deadline,
    DWORD& bytes_written,
    DWORD& error_code) {
  LocalHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.valid()) {
    error_code = GetLastError();
    return PipeIoResult::Failed;
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  bytes_written = 0;
  error_code = ERROR_SUCCESS;

  if (WriteFile(pipe, buffer, byte_count, nullptr, &overlapped)) {
    if (GetOverlappedResult(pipe, &overlapped, &bytes_written, FALSE)) {
      return bytes_written == 0 ? PipeIoResult::Closed : PipeIoResult::Ok;
    }

    error_code = GetLastError();
    return is_closed_pipe_error(error_code) ? PipeIoResult::Closed : PipeIoResult::Failed;
  }

  error_code = GetLastError();
  if (error_code != ERROR_IO_PENDING) {
    return is_closed_pipe_error(error_code) ? PipeIoResult::Closed : PipeIoResult::Failed;
  }

  const auto result = wait_for_overlapped_result(
      pipe, overlapped, event.get(), deadline, nullptr, bytes_written, error_code);
  return result == PipeIoResult::Ok && bytes_written == 0 ? PipeIoResult::Closed : result;
}

bool write_all_overlapped(HANDLE pipe, std::string_view bytes) {
  const auto deadline = std::chrono::steady_clock::now() + kSlowClientWriteTimeout;
  while (!bytes.empty()) {
    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
    DWORD bytes_written = 0;
    DWORD error_code = ERROR_SUCCESS;
    const auto write_result = write_overlapped_chunk(
        pipe, bytes.data(), bytes_to_write, deadline, bytes_written, error_code);
    if (write_result != PipeIoResult::Ok) {
      log_event(
          write_result == PipeIoResult::Timeout ? LogLevel::Warn : LogLevel::Debug,
          "daemon.attach",
          write_result == PipeIoResult::Timeout ? "write_timeout" : "write_failed",
          {{"remaining_bytes", std::to_string(bytes.size())},
           {"win32_error", std::to_string(error_code)}});
      return false;
    }

    bytes.remove_prefix(bytes_written);
  }

  return true;
}

enum class PipeReadResult {
  Ok,
  Closed,
  Failed,
};

PipeReadResult wait_for_available_bytes(
    HANDLE pipe,
    DWORD minimum_byte_count,
    const std::atomic_bool& stop_requested) {
  while (!stop_requested) {
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_available, nullptr)) {
      const DWORD error = GetLastError();
      return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED
                 ? PipeReadResult::Closed
                 : PipeReadResult::Failed;
    }

    if (bytes_available >= minimum_byte_count) {
      return PipeReadResult::Ok;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  return PipeReadResult::Closed;
}

PipeReadResult wait_for_available_bytes_until(
    HANDLE pipe,
    DWORD minimum_byte_count,
    std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_available, nullptr)) {
      const DWORD error = GetLastError();
      return error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED
                 ? PipeReadResult::Closed
                 : PipeReadResult::Failed;
    }

    if (bytes_available >= minimum_byte_count) {
      return PipeReadResult::Ok;
    }

    std::this_thread::sleep_for(kRequestReadPoll);
  }

  return PipeReadResult::Failed;
}

PipeReadResult read_exact(
    HANDLE pipe,
    char* buffer,
    std::size_t byte_count,
    const std::atomic_bool* stop_requested = nullptr,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
  std::size_t total_read = 0;
  while (total_read < byte_count) {
    const auto bytes_remaining =
        static_cast<DWORD>(std::min<std::size_t>(byte_count - total_read, 64 * 1024));
    DWORD bytes_read = 0;
    DWORD error_code = ERROR_SUCCESS;
    const auto read_result = read_overlapped_chunk(
        pipe,
        buffer + total_read,
        bytes_remaining,
        stop_requested,
        deadline,
        bytes_read,
        error_code);
    if (read_result == PipeIoResult::Closed) {
      return PipeReadResult::Closed;
    }
    if (read_result != PipeIoResult::Ok) {
      return PipeReadResult::Failed;
    }
    if (bytes_read == 0) {
      return PipeReadResult::Closed;
    }
    total_read += bytes_read;
  }

  return PipeReadResult::Ok;
}

bool read_attach_start_frame(HANDLE pipe, IpcFrameParseResult& frame) {
  std::array<char, kIpcFrameHeaderSize> raw_header{};
  const auto deadline = std::chrono::steady_clock::now() + kRequestReadTimeout;
  const auto header_ready =
      wait_for_available_bytes_until(pipe, static_cast<DWORD>(raw_header.size()), deadline);
  if (header_ready != PipeReadResult::Ok) {
    return false;
  }

  const auto header_read =
      read_exact(pipe, raw_header.data(), raw_header.size(), nullptr, deadline);
  if (header_read != PipeReadResult::Ok) {
    return false;
  }

  frame = parse_ipc_frame_header(std::string_view{raw_header.data(), raw_header.size()});
  if (!frame.ok) {
    return true;
  }

  frame.payload.clear();
  if (frame.header.payload_size == 0) {
    return true;
  }

  const auto payload_ready =
      wait_for_available_bytes_until(pipe, frame.header.payload_size, deadline);
  if (payload_ready != PipeReadResult::Ok) {
    frame.ok = false;
    frame.error = IpcFrameError::TruncatedPayload;
    frame.message = "wmux: truncated IPC frame payload";
    return true;
  }

  frame.payload.resize(frame.header.payload_size);
  const auto payload_read =
      read_exact(pipe, frame.payload.data(), frame.payload.size(), nullptr, deadline);
  if (payload_read != PipeReadResult::Ok) {
    frame.ok = false;
    frame.error = IpcFrameError::TruncatedPayload;
    frame.message = "wmux: truncated IPC frame payload";
    return true;
  }

  return true;
}

bool write_ipc_response_frame(
    HANDLE pipe,
    IpcFrameKind kind,
    RequestId request_id,
    std::string_view payload) {
  return write_all_overlapped(pipe, make_ipc_frame(kind, request_id, payload));
}

struct AttachFrame {
  AttachFrameType type{AttachFrameType::Input};
  RequestId request_id{0};
  std::string payload;
};

struct MouseDragState {
  bool active{false};
  PaneSplitResizeTarget target;
};

struct PendingPaneResize {
  WindowId window_id{0};
  PaneId pane_id{0};
  std::shared_ptr<PtyProcess> shell;
  short columns{0};
  short rows{0};
};

struct PaneResizePlan {
  std::vector<PendingPaneResize> pending;
  std::size_t skipped{0};
};

short bounded_pty_dimension(int value) {
  // ConPTY can technically accept 1x1, but shells and console hosts behave poorly
  // during rapid nested layout resizes at that size. Keep the visible layout exact,
  // but never drive the backing PTY below a minimal usable grid.
  return static_cast<short>(std::clamp(value, 2, 32767));
}

PaneResizePlan collect_pending_pane_resizes(
    DaemonState& state,
    SessionId session_id,
    const ActiveWindowFrame& frame) {
  PaneResizePlan plan;
  plan.pending.reserve(frame.panes.size());

  std::lock_guard lock(state.mutex);
  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    return plan;
  }

  const auto window_runtime = runtime->second.windows.find(frame.window_id);
  if (window_runtime == runtime->second.windows.end()) {
    return plan;
  }

  for (const auto& pane : frame.panes) {
    const auto pane_runtime = window_runtime->second.panes.find(pane.rect.pane_id);
    if (pane_runtime == window_runtime->second.panes.end() || !pane_runtime->second.shell) {
      continue;
    }

    const short columns = bounded_pty_dimension(body_width(pane.rect, frame.columns));
    const short rows = bounded_pty_dimension(body_height(pane.rect, frame.pane_rows));
    if (pane_runtime->second.pty_columns == columns && pane_runtime->second.pty_rows == rows) {
      ++plan.skipped;
      continue;
    }

    pane_runtime->second.pty_columns = columns;
    pane_runtime->second.pty_rows = rows;
    plan.pending.push_back(PendingPaneResize{
        frame.window_id,
        pane.rect.pane_id,
        pane_runtime->second.shell,
        columns,
        rows});
  }

  if (plan.skipped > 0) {
    state.render_metrics.pty_resize_skipped.fetch_add(plan.skipped, std::memory_order_relaxed);
  }
  if (!plan.pending.empty()) {
    state.render_metrics.pty_resize_requests.fetch_add(
        plan.pending.size(),
        std::memory_order_relaxed);
  }

  return plan;
}

void record_pane_resize_results(
    DaemonState& state,
    SessionId session_id,
    std::size_t applied_count,
    const std::vector<PendingPaneResize>& failed) {
  if (applied_count > 0) {
    state.render_metrics.pty_resize_applied.fetch_add(applied_count, std::memory_order_relaxed);
  }
  if (failed.empty()) {
    return;
  }

  state.render_metrics.pty_resize_failures.fetch_add(failed.size(), std::memory_order_relaxed);
  std::lock_guard lock(state.mutex);
  const auto runtime = state.runtimes.find(session_id);
  if (runtime == state.runtimes.end()) {
    return;
  }

  for (const auto& resize : failed) {
    const auto window = runtime->second.windows.find(resize.window_id);
    if (window == runtime->second.windows.end()) {
      continue;
    }

    const auto pane = window->second.panes.find(resize.pane_id);
    if (pane == window->second.panes.end()) {
      continue;
    }

    if (pane->second.pty_columns == resize.columns && pane->second.pty_rows == resize.rows) {
      pane->second.pty_columns = 0;
      pane->second.pty_rows = 0;
    }
  }
}

std::string next_window_name(const std::vector<WindowSummary>& windows) {
  for (std::uint64_t candidate = 0; candidate < 100000; ++candidate) {
    const auto name = std::to_string(candidate);
    const auto exists = std::any_of(windows.begin(), windows.end(), [&](const auto& window) {
      return window.name == name;
    });
    if (!exists) {
      return name;
    }
  }

  return "window-" + std::to_string(windows.size());
}

std::string resource_limit_message(std::string_view object, std::size_t limit) {
  return "wmux: " + std::string{object} + " limit reached (" + std::to_string(limit) + ")\n";
}

bool create_interactive_window_named(
    DaemonState& state,
    SessionId session_id,
    std::string_view name,
    std::string& error) {
  {
    std::lock_guard lock(state.mutex);
    const auto windows = state.sessions.list_windows(session_id);
    if (windows.size() >= state.config.values.limits.max_windows_per_session) {
      error = resource_limit_message(
          "window per session",
          state.config.values.limits.max_windows_per_session);
      return false;
    }
  }

  auto shell = start_configured_shell(state);
  if (!shell.process) {
    record_diagnostic_event(
        state,
        DiagnosticEvent{
            0,
            {},
            DiagnosticEventCategory::Error,
            "error",
            "shell_spawn_failed",
            0,
            0,
            session_id,
            0,
            0,
            "interactive window shell spawn failed",
            {{"error", shell.error},
             {"shell_source", shell.options.source},
             {"shell_executable", shell.options.executable},
             {"cwd", shell.options.working_directory}}});
    error = shell.error;
    return false;
  }

  std::shared_ptr<PtyProcess> shell_process = std::move(shell.process);
  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.create_window(session_id, std::string{name});
    if (result.ok) {
      install_pane_runtime_shell_locked(
          state,
          result.session_id,
          result.window_id,
          result.pane_id,
          std::move(shell_process));
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }

  if (!result.ok) {
    error = window_error_message(result.error, name);
    if (shell_process) {
      (void)shell_process->terminate();
    }
    return false;
  }

  log_event(
      LogLevel::Info,
      "daemon.window",
      "interactive_create",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"window_name", std::string{name}}});
  return true;
}

bool create_interactive_window(DaemonState& state, SessionId session_id, std::string& error) {
  std::string name;
  {
    std::lock_guard lock(state.mutex);
    name = next_window_name(state.sessions.list_windows(session_id));
  }

  return create_interactive_window_named(state, session_id, name, error);
}

bool select_interactive_window(
    DaemonState& state,
    SessionId session_id,
    std::string_view direction,
    std::string& error) {
  std::lock_guard lock(state.mutex);

  WindowOperationResult result;
  if (direction == "next-window") {
    result = state.sessions.select_next_window(session_id);
  } else if (direction == "previous-window") {
    result = state.sessions.select_previous_window(session_id);
  } else {
    error = "wmux: unknown attach command\n";
    return false;
  }

  if (!result.ok) {
    error = window_error_message(result.error, {});
    return false;
  }
  sync_attach_client_focus_locked(state, result.session_id);

  log_event(
      LogLevel::Info,
      "daemon.window",
      "interactive_select",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"command", std::string{direction}}});
  return true;
}

std::optional<bool> select_interactive_window_id(
    DaemonState& state,
    SessionId session_id,
    WindowId window_id,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto active = state.sessions.active_window_id(session_id);
  if (active && *active == window_id) {
    return false;
  }

  const auto result = state.sessions.select_window(session_id, window_id);
  if (!result.ok) {
    error = window_error_message(result.error, {});
    return std::nullopt;
  }
  sync_attach_client_focus_locked(state, result.session_id);

  log_event(
      LogLevel::Info,
      "daemon.window",
      "interactive_select",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"command", "select-window"}});
  return true;
}

int minimum_interactive_pane_columns() {
  return kMinimumInteractivePaneBodyColumns + kPaneBorderColumns;
}

int minimum_interactive_pane_rows() {
  return kMinimumInteractivePaneBodyRows + kPaneBorderRows;
}

bool validate_interactive_split_size(
    DaemonState& state,
    SessionId session_id,
    SplitDirection direction,
    short columns,
    short rows,
    bool status_bar_enabled,
    std::string& error) {
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - (status_bar_enabled && frame_rows > 1 ? 1 : 0));

  std::lock_guard lock(state.mutex);
  const auto window = state.sessions.active_window_summary(session_id);
  if (!window) {
    error = "wmux: session has no active window\n";
    return false;
  }

  const auto rects = compute_pane_layout_rects(window->pane_tree, frame_columns, pane_rows);
  const auto active = std::find_if(rects.begin(), rects.end(), [&](const auto& rect) {
    return rect.pane_id == window->active_pane_id;
  });
  if (active == rects.end()) {
    error = "wmux: active pane has no layout rectangle\n";
    return false;
  }

  const int min_columns = minimum_interactive_pane_columns();
  const int min_rows = minimum_interactive_pane_rows();
  const bool too_narrow = direction == SplitDirection::Horizontal
                              ? active->width < min_columns * 2
                              : active->width < min_columns;
  const bool too_short = direction == SplitDirection::Vertical
                             ? active->height < min_rows * 2
                             : active->height < min_rows;
  if (!too_narrow && !too_short) {
    return true;
  }

  error = direction == SplitDirection::Horizontal
              ? "wmux: active pane is too narrow to split\n"
              : "wmux: active pane is too short to split\n";
  return false;
}

bool split_interactive_pane(
    DaemonState& state,
    SessionId session_id,
    SplitDirection direction,
    short columns,
    short rows,
    bool status_bar_enabled,
    std::string& error) {
  if (!validate_interactive_split_size(
          state, session_id, direction, columns, rows, status_bar_enabled, error)) {
    return false;
  }

  {
    std::lock_guard lock(state.mutex);
    const auto window = state.sessions.active_window_summary(session_id);
    if (window && window->panes.size() >= state.config.values.limits.max_panes_per_window) {
      error = resource_limit_message(
          "pane per window",
          state.config.values.limits.max_panes_per_window);
      return false;
    }
  }

  auto shell = start_configured_shell(state);
  if (!shell.process) {
    record_diagnostic_event(
        state,
        DiagnosticEvent{
            0,
            {},
            DiagnosticEventCategory::Error,
            "error",
            "shell_spawn_failed",
            0,
            0,
            session_id,
            0,
            0,
            "interactive pane shell spawn failed",
            {{"error", shell.error},
             {"shell_source", shell.options.source},
             {"shell_executable", shell.options.executable},
             {"cwd", shell.options.working_directory}}});
    error = shell.error;
    return false;
  }

  std::shared_ptr<PtyProcess> shell_process = std::move(shell.process);
  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.split_active_pane(session_id, direction);
    if (result.ok) {
      install_pane_runtime_shell_locked(
          state,
          result.session_id,
          result.window_id,
          result.pane_id,
          std::move(shell_process));
      mark_window_layout_changed_locked(state, result.session_id, result.window_id);
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }

  if (!result.ok) {
    error = pane_error_message(result.error);
    if (shell_process) {
      (void)shell_process->terminate();
    }
    return false;
  }

  log_event(
      LogLevel::Info,
      "daemon.pane",
      "interactive_split",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.pane_id)},
       {"direction", direction == SplitDirection::Horizontal ? "horizontal" : "vertical"}});
  return true;
}

bool select_interactive_pane(
    DaemonState& state,
    SessionId session_id,
    PaneDirection direction,
    short columns,
    short rows,
    bool reserve_status_row,
    std::string& error) {
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - (reserve_status_row && frame_rows > 1 ? 1 : 0));

  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.select_pane(session_id, direction, frame_columns, pane_rows);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return false;
  }
  sync_attach_client_focus_locked(state, result.session_id);

  return true;
}

std::optional<bool> select_interactive_pane_id(
    DaemonState& state,
    SessionId session_id,
    PaneId pane_id,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto active_pane = state.sessions.active_pane_id(session_id);
  if (active_pane && *active_pane == pane_id) {
    return false;
  }

  const auto result = state.sessions.select_pane(session_id, pane_id);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return std::nullopt;
  }
  sync_attach_client_focus_locked(state, result.session_id);

  return true;
}

std::optional<bool> select_interactive_pane_at(
    DaemonState& state,
    SessionId session_id,
    std::uint16_t column,
    std::uint16_t row,
    short columns,
    short rows,
    bool reserve_status_row,
    std::string& error) {
  if (column == 0 || row == 0) {
    return false;
  }

  const int zero_based_column = static_cast<int>(column) - 1;
  const int zero_based_row = static_cast<int>(row) - 1;
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - (reserve_status_row && frame_rows > 1 ? 1 : 0));
  if (zero_based_column < 0 || zero_based_column >= frame_columns ||
      zero_based_row < 0 || zero_based_row >= pane_rows) {
    return false;
  }

  std::lock_guard lock(state.mutex);
  const auto window = state.sessions.active_window_summary(session_id);
  if (!window) {
    error = "wmux: session has no active window\n";
    return std::nullopt;
  }

  const auto rects = compute_pane_layout_rects(window->pane_tree, frame_columns, pane_rows);
  const auto hit = std::find_if(rects.begin(), rects.end(), [&](const auto& rect) {
    return zero_based_column >= rect.left &&
           zero_based_column < rect.left + rect.width &&
           zero_based_row >= rect.top &&
           zero_based_row < rect.top + rect.height;
  });
  if (hit == rects.end()) {
    return false;
  }

  if (hit->pane_id == window->active_pane_id) {
    return false;
  }

  const auto result = state.sessions.select_pane(session_id, hit->pane_id);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return std::nullopt;
  }
  sync_attach_client_focus_locked(state, result.session_id);

  return true;
}

std::optional<bool> resize_interactive_split(
    DaemonState& state,
    SessionId session_id,
    const PaneSplitResizeTarget& target,
    int column,
    int row,
    std::string& error) {
  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.resize_active_window_split(session_id, target, column, row);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return std::nullopt;
  }
  if (result.changed) {
    mark_window_layout_changed_locked(state, result.session_id, result.window_id);
  }

  return true;
}

PaneDirection pane_direction_from_resize_direction(ResizeDirection direction) {
  switch (direction) {
    case ResizeDirection::Left:
      return PaneDirection::Left;
    case ResizeDirection::Right:
      return PaneDirection::Right;
    case ResizeDirection::Up:
      return PaneDirection::Up;
    case ResizeDirection::Down:
      return PaneDirection::Down;
  }
  return PaneDirection::Right;
}

std::optional<bool> resize_interactive_active_pane(
    DaemonState& state,
    SessionId session_id,
    ResizeDirection direction,
    std::uint16_t amount,
    short columns,
    short rows,
    bool reserve_status_row,
    std::string& error) {
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - (reserve_status_row && frame_rows > 1 ? 1 : 0));

  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.resize_active_pane(
      session_id,
      pane_direction_from_resize_direction(direction),
      amount == 0 ? 1 : amount,
      frame_columns,
      pane_rows);
  if (!result.ok) {
    error = pane_error_message(result.error);
    return std::nullopt;
  }
  if (result.changed) {
    mark_window_layout_changed_locked(state, result.session_id, result.window_id);
  }
  sync_attach_client_focus_locked(state, result.session_id);
  return result.changed;
}

CommandResult execute_runtime_attach_command(
    DaemonState& state,
    ClientId client_id,
    const RuntimeCommand& command,
    short columns,
    short rows,
    bool status_bar_enabled,
    std::string* attached_session_name = nullptr);

RuntimeCommand select_pane_at_mouse_command(
    ClientId client_id,
    std::uint16_t column,
    std::uint16_t row) {
  RuntimeCommand command;
  command.kind = RuntimeCommandKind::SelectPane;
  command.target = Target::mouse_position(client_id, column, row);
  return command;
}

std::optional<bool> handle_interactive_mouse_event(
    DaemonState& state,
    ClientId client_id,
    const AttachMouseEventPayload& mouse,
    short columns,
    short rows,
    bool reserve_status_row,
    MouseDragState& drag,
    std::string& error) {
  const int frame_columns = columns > 0 ? columns : 120;
  const int frame_rows = rows > 0 ? rows : 30;
  const int pane_rows = std::max(1, frame_rows - (reserve_status_row && frame_rows > 1 ? 1 : 0));
  const int column = static_cast<int>(mouse.column) - 1;
  const int row = static_cast<int>(mouse.row) - 1;

  if (mouse.action == AttachMouseAction::Release) {
    drag.active = false;
    return false;
  }

  if (column < 0 || column >= frame_columns || row < 0 || row >= pane_rows) {
    return false;
  }

  if (mouse.action == AttachMouseAction::Drag) {
    drag.active = false;
    return false;
  }

  if (mouse.action != AttachMouseAction::Press || mouse.button != AttachMouseButton::Left) {
    return false;
  }

  drag.active = false;
  const auto command = select_pane_at_mouse_command(client_id, mouse.column, mouse.row);
  const auto result = execute_runtime_attach_command(
      state,
      client_id,
      command,
      columns,
      rows,
      reserve_status_row,
      nullptr);
  if (result.status == CommandStatus::Success) {
    return true;
  }
  if (result.status == CommandStatus::NoOp) {
    return false;
  }
  error = result.message.value_or("wmux: mouse focus failed");
  return std::nullopt;
}

std::string status_from_error(std::string_view error) {
  std::string status{error};
  while (!status.empty() && (status.back() == '\n' || status.back() == '\r')) {
    status.pop_back();
  }
  return status;
}

bool daemon_mouse_setting_enabled(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return state.mouse_enabled;
}

bool kill_interactive_pane(DaemonState& state, SessionId session_id, std::string& status) {
  std::vector<std::shared_ptr<PtyProcess>> removed_shells;
  PaneOperationResult result;
  WindowOperationResult window_result;
  bool killed_window = false;
  bool killed_session = false;
  std::string killed_session_name;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.kill_active_pane(session_id);
    if (!result.ok) {
      if (result.error != PaneError::LastPane) {
        status = status_from_error(pane_error_message(result.error));
        return false;
      }

      const auto windows = state.sessions.list_windows(session_id);
      if (windows.size() <= 1) {
        for (const auto& session : state.sessions.list_sessions()) {
          if (session.id == session_id) {
            killed_session_name = session.name;
            break;
          }
        }
        if (killed_session_name.empty()) {
          status = "wmux: attached session no longer exists";
          return false;
        }

        const auto runtime = state.runtimes.find(session_id);
        if (runtime != state.runtimes.end()) {
          for (auto& [window_id, window] : runtime->second.windows) {
            (void)window_id;
            for (auto& [pane_id, pane] : window.panes) {
              (void)pane_id;
              if (pane.shell) {
                removed_shells.push_back(std::move(pane.shell));
              }
            }
          }
          state.runtimes.erase(runtime);
        }

        const auto killed = state.sessions.kill_session(killed_session_name);
        if (!killed.ok) {
          status = status_from_error(session_error_message(killed.error, killed_session_name));
          return false;
        }
        sync_attach_client_focus_locked(state, session_id);
        killed_session = true;
      } else {
        window_result = state.sessions.kill_active_window(session_id);
        if (!window_result.ok) {
          status = status_from_error(window_error_message(window_result.error, {}));
          return false;
        }

        const auto runtime = state.runtimes.find(window_result.session_id);
        if (runtime != state.runtimes.end()) {
          const auto window = runtime->second.windows.find(window_result.removed_window_id);
          if (window != runtime->second.windows.end()) {
            removed_shells.reserve(window->second.panes.size());
            for (auto& [pane_id, pane] : window->second.panes) {
              (void)pane_id;
              if (pane.shell) {
                removed_shells.push_back(std::move(pane.shell));
              }
            }
            runtime->second.windows.erase(window);
          }
        }
        sync_attach_client_focus_locked(state, window_result.session_id);
        killed_window = true;
      }
    } else {
      const auto runtime = state.runtimes.find(result.session_id);
      if (runtime != state.runtimes.end()) {
        const auto window = runtime->second.windows.find(result.window_id);
        if (window != runtime->second.windows.end()) {
          const auto pane = window->second.panes.find(result.removed_pane_id);
          if (pane != window->second.panes.end()) {
            removed_shells.push_back(std::move(pane->second.shell));
            window->second.panes.erase(pane);
            mark_window_layout_changed_locked(state, result.session_id, result.window_id);
          }
        }
      }
      sync_attach_client_focus_locked(state, result.session_id);
    }
  }

  for (auto& shell : removed_shells) {
    if (shell) {
      shell->terminate();
    }
  }

  if (killed_session) {
    log_event(
        LogLevel::Info,
        "daemon.session",
        "interactive_kill_via_last_pane",
        {{"session_id", std::to_string(session_id)},
         {"session_name", killed_session_name},
         {"shells", std::to_string(removed_shells.size())}});
    status = "wmux: killed session " + killed_session_name;
    return true;
  }

  if (killed_window) {
    log_event(
        LogLevel::Info,
        "daemon.window",
        "interactive_kill_via_last_pane",
        {{"session_id", std::to_string(window_result.session_id)},
         {"window_id", std::to_string(window_result.removed_window_id)},
         {"shells", std::to_string(removed_shells.size())}});
    status = "wmux: killed window " + std::to_string(window_result.removed_window_id);
    return true;
  }

  log_event(
      LogLevel::Info,
      "daemon.pane",
      "interactive_kill",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"pane_id", std::to_string(result.removed_pane_id)}});
  status = "wmux: killed pane " + std::to_string(result.removed_pane_id);
  return true;
}

bool kill_interactive_window(DaemonState& state, SessionId session_id, std::string& status) {
  std::vector<std::shared_ptr<PtyProcess>> removed_shells;
  WindowOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.kill_active_window(session_id);
    if (!result.ok) {
      status = status_from_error(window_error_message(result.error, {}));
      return false;
    }

    const auto runtime = state.runtimes.find(result.session_id);
    if (runtime != state.runtimes.end()) {
      const auto window = runtime->second.windows.find(result.removed_window_id);
      if (window != runtime->second.windows.end()) {
        removed_shells.reserve(window->second.panes.size());
        for (auto& [pane_id, pane] : window->second.panes) {
          (void)pane_id;
          if (pane.shell) {
            removed_shells.push_back(std::move(pane.shell));
          }
        }
        runtime->second.windows.erase(window);
      }
    }
    sync_attach_client_focus_locked(state, result.session_id);
  }

  for (auto& shell : removed_shells) {
    if (shell) {
      shell->terminate();
    }
  }

  log_event(
      LogLevel::Info,
      "daemon.window",
      "interactive_kill",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.removed_window_id)},
       {"shells", std::to_string(removed_shells.size())}});
  status = "wmux: killed window " + std::to_string(result.removed_window_id);
  return true;
}

bool equalize_interactive_panes(DaemonState& state, SessionId session_id, std::string& status) {
  PaneOperationResult result;
  {
    std::lock_guard lock(state.mutex);
    result = state.sessions.equalize_active_window_panes(session_id);
    if (!result.ok) {
      status = status_from_error(pane_error_message(result.error));
      return false;
    }
    if (result.changed) {
      mark_window_layout_changed_locked(state, result.session_id, result.window_id);
    }
  }

  log_event(
      LogLevel::Info,
      "daemon.pane",
      "interactive_equalize",
      {{"session_id", std::to_string(result.session_id)},
       {"window_id", std::to_string(result.window_id)},
       {"active_pane_id", std::to_string(result.pane_id)},
       {"changed", result.changed ? "true" : "false"}});
  status = result.changed ? "wmux: panes spread evenly" : "wmux: panes already evenly spread";
  return true;
}

bool rename_attached_session(
    DaemonState& state,
    SessionId session_id,
    std::string_view new_name,
    std::string* attached_session_name,
    std::string& status) {
  std::lock_guard lock(state.mutex);
  const auto sessions = state.sessions.list_sessions();
  const auto current = std::find_if(sessions.begin(), sessions.end(), [&](const auto& session) {
    return session.id == session_id;
  });
  if (current == sessions.end()) {
    status = "wmux: attached session no longer exists";
    return false;
  }

  const auto old_name = current->name;
  const auto result = state.sessions.rename_session(old_name, std::string{new_name});
  if (!result.ok) {
    const auto name = result.error == SessionError::DuplicateName ? new_name : old_name;
    status = status_from_error(session_error_message(result.error, name));
    return false;
  }

  for (auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.client.attached_session == session_id) {
      client.session_name = std::string{new_name};
    }
  }
  if (attached_session_name) {
    *attached_session_name = std::string{new_name};
  }
  status = "wmux: renamed session to '" + std::string{new_name} + "'";
  return true;
}

bool rename_attached_window(
    DaemonState& state,
    SessionId session_id,
    std::string_view name,
    std::string& status) {
  std::lock_guard lock(state.mutex);
  const auto result = state.sessions.rename_active_window(session_id, std::string{name});
  if (!result.ok) {
    status = status_from_error(window_error_message(result.error, name));
    return false;
  }

  status = "wmux: renamed active window to '" + std::string{name} + "'";
  return true;
}

bool execute_command_mode_command(
    DaemonState& state,
    ClientId client_id,
    SessionId session_id,
    std::string& session_name,
    short columns,
    short rows,
    bool status_bar_enabled,
    std::string_view command_text,
    std::string& status) {
  (void)session_id;

  std::string parse_error;
  const auto runtime_command = runtime_command_from_command_mode_text(command_text, parse_error);
  if (!runtime_command) {
    status = parse_error;
    return false;
  }

  const auto result = execute_runtime_attach_command(
      state,
      client_id,
      *runtime_command,
      columns,
      rows,
      status_bar_enabled,
      &session_name);
  status = result.message.value_or("");
  return result.status == CommandStatus::Success || result.status == CommandStatus::NoOp;
}

CommandResult attach_command_error(std::string message) {
  CommandResult result;
  result.status = CommandStatus::UserError;
  result.message = status_from_error(message);
  return result;
}

CommandResult attach_command_success(
    std::optional<std::string> message,
    RedrawRequest redraw = RedrawRequest::ActiveWindow) {
  CommandResult result;
  result.status = CommandStatus::Success;
  result.message = std::move(message);
  result.redraw = redraw;
  return result;
}

CommandResult attach_command_noop(std::optional<std::string> message = std::nullopt) {
  CommandResult result;
  result.status = CommandStatus::NoOp;
  result.message = std::move(message);
  result.redraw = RedrawRequest::None;
  return result;
}

CommandResult execute_runtime_attach_command(
    DaemonState& state,
    ClientId client_id,
    const RuntimeCommand& command,
    short columns,
    short rows,
    bool status_bar_enabled,
    std::string* attached_session_name) {
  const auto started_at = std::chrono::steady_clock::now();

  const auto finish =
      [&](CommandResult result, const std::optional<ResolvedTarget>& target) -> CommandResult {
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    log_event(
        result.status == CommandStatus::Success || result.status == CommandStatus::NoOp
            ? LogLevel::Info
            : LogLevel::Warn,
        "daemon.command",
        "execute",
        {{"client_id", std::to_string(client_id)},
         {"session_id", target ? std::to_string(target->session_id) : ""},
         {"window_id", target ? std::to_string(target->window_id) : ""},
         {"pane_id",
          target && target->pane_id ? std::to_string(*target->pane_id) : ""},
         {"command", std::string{runtime_command_name(command.kind)}},
         {"target", std::string{target_kind_name(command.target.kind)}},
         {"result", std::string{command_status_name(result.status)}},
         {"message", result.message.value_or("")},
         {"elapsed_us", std::to_string(elapsed_us)}});
    return result;
  };

  const auto resolved = resolve_target(state, command.target, client_id);
  if (!resolved.ok) {
    return finish(attach_command_error(resolved.error), std::nullopt);
  }

  const SessionId session_id = resolved.target.session_id;
  switch (command.kind) {
    case RuntimeCommandKind::NewWindow: {
      std::string error;
      const bool created = command.name && !command.name->empty()
                               ? create_interactive_window_named(
                                     state,
                                     session_id,
                                     *command.name,
                                     error)
                               : create_interactive_window(state, session_id, error);
      if (!created) {
        return finish(attach_command_error(error), resolved.target);
      }
      const auto message = command.name && !command.name->empty()
                               ? "wmux: created window '" + *command.name + "'"
                               : std::string{"wmux: created window"};
      return finish(attach_command_success(message), resolved.target);
    }

    case RuntimeCommandKind::RenameSession: {
      if (!command.name || command.name->empty()) {
        return finish(attach_command_error("wmux: rename-session requires <new>"), resolved.target);
      }

      std::string status;
      if (!rename_attached_session(
              state,
              session_id,
              *command.name,
              attached_session_name,
              status)) {
        return finish(attach_command_error(status), resolved.target);
      }
      return finish(attach_command_success(status), resolved.target);
    }

    case RuntimeCommandKind::RenameWindow: {
      if (!command.name || command.name->empty()) {
        return finish(attach_command_error("wmux: rename-window requires <new>"), resolved.target);
      }

      std::string status;
      if (!rename_attached_window(state, session_id, *command.name, status)) {
        return finish(attach_command_error(status), resolved.target);
      }
      return finish(attach_command_success(status), resolved.target);
    }

    case RuntimeCommandKind::NextWindow:
    case RuntimeCommandKind::PreviousWindow: {
      const std::string_view direction =
          command.kind == RuntimeCommandKind::NextWindow ? "next-window" : "previous-window";
      std::string error;
      if (!select_interactive_window(state, session_id, direction, error)) {
        return finish(attach_command_error(error), resolved.target);
      }
      const auto message = command.kind == RuntimeCommandKind::NextWindow
                               ? "wmux: selected next window"
                               : "wmux: selected previous window";
      return finish(attach_command_success(message), resolved.target);
    }

    case RuntimeCommandKind::SelectWindow: {
      std::string error;
      const auto changed =
          select_interactive_window_id(state, session_id, resolved.target.window_id, error);
      if (!changed) {
        if (!error.empty()) {
          return finish(attach_command_error(error), resolved.target);
        }
        return finish(attach_command_noop(), resolved.target);
      }
      return finish(
          attach_command_success(
              "wmux: selected window " + std::to_string(resolved.target.window_id)),
          resolved.target);
    }

    case RuntimeCommandKind::SplitPane: {
      std::string error;
      if (!split_interactive_pane(
              state,
              session_id,
              command.axis,
              columns,
              rows,
              status_bar_enabled,
              error)) {
        return finish(attach_command_error(error), resolved.target);
      }
      const auto message = command.axis == SplitDirection::Horizontal
                               ? "wmux: split active pane horizontally"
                               : "wmux: split active pane vertically";
      return finish(attach_command_success(message), resolved.target);
    }

    case RuntimeCommandKind::SelectPane: {
      if (command.target.kind == TargetKind::MousePosition ||
          command.target.kind == TargetKind::Pane) {
        if (!resolved.target.pane_id) {
          return finish(
              attach_command_error("wmux: target does not resolve to a pane"),
              resolved.target);
        }

        std::string error;
        const auto changed =
            select_interactive_pane_id(state, session_id, *resolved.target.pane_id, error);
        if (!changed) {
          if (!error.empty()) {
            return finish(attach_command_error(error), resolved.target);
          }
          return finish(attach_command_noop(), resolved.target);
        }
        return finish(attach_command_success(std::nullopt), resolved.target);
      }

      std::string error;
      if (!select_interactive_pane(
              state,
              session_id,
              command.pane_direction,
              columns,
              rows,
              status_bar_enabled,
              error)) {
        return finish(attach_command_error(error), resolved.target);
      }
      return finish(attach_command_success(std::nullopt), resolved.target);
    }

    case RuntimeCommandKind::ResizePane: {
      std::string error;
      const auto changed = command.split_resize_target
                               ? resize_interactive_split(
                                     state,
                                     session_id,
                                     *command.split_resize_target,
                                     command.mouse_column,
                                     command.mouse_row,
                                     error)
                               : resize_interactive_active_pane(
                                     state,
                                     session_id,
                                     command.resize_direction,
                                     command.amount,
                                     columns,
                                     rows,
                                     status_bar_enabled,
                                     error);
      if (!changed) {
        if (!error.empty()) {
          return finish(attach_command_error(error), resolved.target);
        }
        return finish(attach_command_noop(), resolved.target);
      }
      return finish(attach_command_success("wmux: pane resized"), resolved.target);
    }

    case RuntimeCommandKind::KillPane: {
      std::string status;
      if (!kill_interactive_pane(state, session_id, status)) {
        return finish(attach_command_error(status), resolved.target);
      }
      return finish(attach_command_success(status), resolved.target);
    }

    case RuntimeCommandKind::KillWindow: {
      std::string status;
      if (!kill_interactive_window(state, session_id, status)) {
        return finish(attach_command_error(status), resolved.target);
      }
      return finish(attach_command_success(status), resolved.target);
    }

    case RuntimeCommandKind::SpreadPanesEvenly: {
      std::string status;
      if (!equalize_interactive_panes(state, session_id, status)) {
        return finish(attach_command_error(status), resolved.target);
      }
      return finish(attach_command_success(status), resolved.target);
    }

    case RuntimeCommandKind::SetOption: {
      if (command.option_scope != OptionScope::Global) {
        return finish(attach_command_error("wmux: unsupported option scope"), resolved.target);
      }

      Config next_config;
      {
        std::lock_guard lock(state.mutex);
        next_config = state.config.values;
      }
      if (auto error = apply_global_config_option(next_config, command.key, command.value)) {
        return finish(attach_command_error("wmux: " + error->message), resolved.target);
      }
      const auto log_max_bytes = next_config.limits.max_log_file_bytes;
      {
        std::lock_guard lock(state.mutex);
        state.config.values = std::move(next_config);
        state.mouse_enabled = state.config.values.mouse_enabled;
      }
      configure_logging(log_max_bytes);

      const auto message = "wmux: set " + command.key + " " + command.value;
      return finish(attach_command_success(message, RedrawRequest::AllClients), resolved.target);
    }

    case RuntimeCommandKind::BindKey: {
      if (command.option_scope != OptionScope::Global) {
        return finish(attach_command_error("wmux: unsupported option scope"), resolved.target);
      }

      Config next_config;
      {
        std::lock_guard lock(state.mutex);
        next_config = state.config.values;
      }
      if (auto error = apply_key_binding_config(next_config, command.key, command.value)) {
        return finish(attach_command_error("wmux: " + error->message), resolved.target);
      }
      {
        std::lock_guard lock(state.mutex);
        state.config.values = std::move(next_config);
      }

      const auto message = "wmux: bound " + command.key + " to " + command.value;
      return finish(attach_command_success(message, RedrawRequest::AllClients), resolved.target);
    }

    case RuntimeCommandKind::UnbindKey: {
      if (command.option_scope != OptionScope::Global) {
        return finish(attach_command_error("wmux: unsupported option scope"), resolved.target);
      }

      Config next_config;
      {
        std::lock_guard lock(state.mutex);
        next_config = state.config.values;
      }
      if (auto error = apply_key_unbinding_config(next_config, command.key)) {
        return finish(attach_command_error("wmux: " + error->message), resolved.target);
      }
      {
        std::lock_guard lock(state.mutex);
        state.config.values = std::move(next_config);
      }

      const auto message = "wmux: unbound " + command.key;
      return finish(attach_command_success(message, RedrawRequest::AllClients), resolved.target);
    }

    default:
      return finish(
          attach_command_error("wmux: command is not supported in attach mode"),
          resolved.target);
  }
}

bool execute_attach_command(
    DaemonState& state,
    ClientId client_id,
    SessionId session_id,
    std::string_view command,
    short columns,
    short rows,
    bool status_bar_enabled,
    std::string& status,
    std::string& error) {
  (void)session_id;

  std::string parse_error;
  const auto runtime_command = runtime_command_from_attach_command(command, parse_error);
  if (!runtime_command) {
    error = parse_error + "\n";
    return false;
  }

  const auto result = execute_runtime_attach_command(
      state,
      client_id,
      *runtime_command,
      columns,
      rows,
      status_bar_enabled);

  status = result.message.value_or("");
  const bool ok = result.status == CommandStatus::Success || result.status == CommandStatus::NoOp;
  if (!ok) {
    error = status.empty() ? "wmux: attach command failed\n" : status + "\n";
  }
  return ok;
}

bool read_attach_frame(
    HANDLE pipe,
    AttachFrame& frame,
    AttachEndReason& end_reason,
    const std::atomic_bool& stop_requested) {
  std::array<char, kIpcFrameHeaderSize> ipc_header{};
  const auto header_ready =
      wait_for_available_bytes(pipe, static_cast<DWORD>(ipc_header.size()), stop_requested);
  if (header_ready != PipeReadResult::Ok) {
    end_reason = header_ready == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                        : AttachEndReason::ProtocolError;
    return false;
  }

  const auto header_read =
      read_exact(pipe, ipc_header.data(), ipc_header.size(), &stop_requested);
  if (header_read != PipeReadResult::Ok) {
    end_reason = header_read == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                       : AttachEndReason::ProtocolError;
    return false;
  }

  const auto ipc = parse_ipc_frame_header(std::string_view{ipc_header.data(), ipc_header.size()});
  if (!ipc.ok) {
    end_reason = AttachEndReason::ProtocolError;
    log_event(
        LogLevel::Warn,
        "daemon.attach",
        "invalid_ipc_frame_header",
        {{"error", std::string{ipc_frame_error_name(ipc.error)}},
         {"message", ipc.message}});
    return false;
  }

  if (ipc.header.kind != IpcFrameKind::AttachInput) {
    end_reason = AttachEndReason::ProtocolError;
    log_event(
        LogLevel::Warn,
        "daemon.attach",
        "unexpected_ipc_frame_kind",
        {{"request_id", std::to_string(ipc.header.request_id)},
         {"kind", std::string{ipc_frame_kind_name(ipc.header.kind)}}});
    return false;
  }

  std::string attach_payload;
  attach_payload.resize(ipc.header.payload_size);
  if (ipc.header.payload_size > 0) {
    const auto payload_ready =
        wait_for_available_bytes(pipe, ipc.header.payload_size, stop_requested);
    if (payload_ready != PipeReadResult::Ok) {
      end_reason = payload_ready == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                           : AttachEndReason::ProtocolError;
      return false;
    }

    const auto payload_read =
        read_exact(pipe, attach_payload.data(), attach_payload.size(), &stop_requested);
    if (payload_read != PipeReadResult::Ok) {
      end_reason = payload_read == PipeReadResult::Closed ? AttachEndReason::ClientDisconnected
                                                          : AttachEndReason::ProtocolError;
      return false;
    }
  }

  if (attach_payload.size() < kAttachFrameHeaderSize) {
    end_reason = AttachEndReason::ProtocolError;
    log_event(
        LogLevel::Warn,
        "daemon.attach",
        "short_attach_payload",
        {{"request_id", std::to_string(ipc.header.request_id)},
         {"payload_bytes", std::to_string(attach_payload.size())}});
    return false;
  }

  const auto parsed = parse_attach_frame_header(
      std::string_view{attach_payload.data(), kAttachFrameHeaderSize});
  if (!parsed) {
    end_reason = AttachEndReason::ProtocolError;
    log_event(
        LogLevel::Warn,
        "daemon.attach",
        "invalid_attach_frame_header",
        {{"request_id", std::to_string(ipc.header.request_id)}});
    return false;
  }

  const auto expected_size = kAttachFrameHeaderSize + parsed->payload_size;
  if (attach_payload.size() != expected_size) {
    end_reason = AttachEndReason::ProtocolError;
    log_event(
        LogLevel::Warn,
        "daemon.attach",
        "truncated_attach_payload",
        {{"request_id", std::to_string(ipc.header.request_id)},
         {"expected", std::to_string(expected_size)},
         {"actual", std::to_string(attach_payload.size())}});
    return false;
  }

  frame.type = parsed->type;
  frame.request_id = ipc.header.request_id;
  if (should_log(LogLevel::Debug)) {
    log_event(
        LogLevel::Debug,
        "daemon.attach",
        "frame",
        {{"request_id", std::to_string(frame.request_id)},
         {"type", std::string{attach_frame_type_name(frame.type)}},
         {"payload_bytes", std::to_string(parsed->payload_size)}});
  }
  frame.payload.clear();
  if (parsed->payload_size == 0) {
    return true;
  }

  frame.payload.assign(
      attach_payload.data() + kAttachFrameHeaderSize,
      attach_payload.data() + kAttachFrameHeaderSize + parsed->payload_size);
  return true;
}

void run_attach_connection(
    HANDLE pipe,
    DaemonEventLoop& events,
    ClientId client_id,
    SessionId session_id,
    std::string session_name,
    RequestId attach_request_id,
    short columns,
    short rows,
    DaemonAttachSettings initial_settings) {
  const auto render_mode = attach_render_mode_from_environment();
  std::mutex settings_mutex;
  DaemonAttachSettings settings = std::move(initial_settings);
  const auto settings_snapshot = [&]() {
    std::lock_guard settings_lock(settings_mutex);
    return settings;
  };
  const auto replace_settings = [&](DaemonAttachSettings next_settings) {
    std::lock_guard settings_lock(settings_mutex);
    if (attach_settings_equal(settings, next_settings)) {
      return false;
    }
    settings = std::move(next_settings);
    return true;
  };

  log_event(
      LogLevel::Info,
      "daemon.attach",
      "start",
      {{"client_id", std::to_string(client_id)},
       {"session_id", std::to_string(session_id)},
       {"session_name", session_name},
       {"columns", std::to_string(columns)},
       {"rows", std::to_string(rows)},
       {"mouse", settings.mouse_enabled ? "on" : "off"},
       {"prefix", settings.prefix},
       {"status", settings.status_bar_enabled ? "on" : "off"},
       {"escape_time_ms", std::to_string(settings.escape_time_ms)},
       {"render_mode", std::string{attach_render_mode_name(render_mode)}},
       {"key_bindings_bytes", std::to_string(settings.key_bindings.size())}});
  if (!write_all_overlapped(
          pipe,
          make_ipc_frame(
              IpcFrameKind::Control,
              attach_request_id,
              make_response_json(
                  true,
                  "",
                  settings.mouse_enabled,
                  settings.prefix,
                  settings.status_bar_enabled,
                  settings.escape_time_ms,
                  settings.key_bindings)))) {
    close_attach_pipe(pipe);
    (void)events.call_event(
        DaemonEvent::client_disconnected(client_id, AttachEndReason::OutputClosed));
    return;
  }

  std::atomic_bool stop_requested{false};
  std::atomic_bool output_closed{false};
  std::atomic<short> current_columns{columns > 0 ? columns : static_cast<short>(120)};
  std::atomic<short> current_rows{rows > 0 ? rows : static_cast<short>(30)};
  std::mutex render_mutex;
  std::mutex write_queue_mutex;
  std::condition_variable write_queue_changed;
  std::deque<QueuedAttachWrite> write_queue;
  std::size_t pending_write_bytes = 0;
  bool write_queue_stopping = false;
  std::mutex stream_mutex;
  std::unordered_map<PaneId, std::shared_ptr<PtyProcess>> current_shells;
  std::unordered_map<PaneId, std::uint64_t> next_sequences;
  PaneViewportStates viewport_states;
  CopyModeState copy_mode;
  std::mutex scene_baseline_mutex;
  RenderDiffState client_scene_baseline;
  std::atomic<std::uint64_t> last_render_duration_us{0};
  std::atomic_bool client_render_baseline_unknown{false};
  StatusState status_state;
  StatusLineMode status_mode{StatusLineMode::Normal};
  std::uint64_t next_output_frame_id = 1;
  std::atomic_bool status_dirty{false};
  struct BackgroundAttachWorker {
    std::shared_ptr<std::atomic_bool> done;
    std::thread thread;
  };
  std::mutex background_workers_mutex;
  std::vector<BackgroundAttachWorker> background_workers;

  const auto set_temporary_status = [&](std::string_view status) {
    {
      std::lock_guard stream_lock(stream_mutex);
      status_set_temporary(status_state, status, std::chrono::steady_clock::now());
      status_mode = StatusLineMode::Normal;
    }
    status_dirty.store(true, std::memory_order_release);
  };

  const auto set_background_status = [&](std::string status) {
    set_temporary_status(status);
  };

  const auto set_prefix_status = [&] {
    {
      std::lock_guard stream_lock(stream_mutex);
      status_set_temporary(
          status_state,
          "prefix",
          std::chrono::steady_clock::now(),
          kPrefixStatusMessageTtl);
      status_mode = StatusLineMode::Prefix;
    }
    status_dirty.store(true, std::memory_order_release);
  };

  const auto set_command_prompt_status = [&](std::string_view status) {
    {
      std::lock_guard stream_lock(stream_mutex);
      if (status.empty()) {
        status_clear_temporary(status_state);
        status_mode = StatusLineMode::Normal;
      } else {
        status_set_persistent(status_state, status, std::chrono::steady_clock::now());
        status_mode = StatusLineMode::CommandPrompt;
      }
    }
    status_dirty.store(true, std::memory_order_release);
  };

  const auto set_status_frame = [&](std::string_view status) {
    if (status.empty()) {
      set_command_prompt_status(status);
      return;
    }
    if (status == "prefix") {
      set_prefix_status();
      return;
    }
    if (!status.empty() && status.front() == ':') {
      set_command_prompt_status(status);
      return;
    }
    set_temporary_status(status);
  };

  const auto reap_background_workers = [&] {
    std::vector<std::thread> workers;
    {
      std::lock_guard workers_lock(background_workers_mutex);
      auto worker = background_workers.begin();
      while (worker != background_workers.end()) {
        if (worker->done && worker->done->load(std::memory_order_acquire)) {
          workers.push_back(std::move(worker->thread));
          worker = background_workers.erase(worker);
        } else {
          ++worker;
        }
      }
    }
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  };

  const auto start_background_worker = [&](auto&& work) {
    reap_background_workers();
    auto done = std::make_shared<std::atomic_bool>(false);
    std::thread worker{
        [done, work = std::forward<decltype(work)>(work)]() mutable {
          work();
          done->store(true, std::memory_order_release);
        }};
    std::lock_guard workers_lock(background_workers_mutex);
    background_workers.push_back(BackgroundAttachWorker{done, std::move(worker)});
  };

  const auto join_background_workers = [&] {
    std::vector<std::thread> workers;
    {
      std::lock_guard workers_lock(background_workers_mutex);
      workers.reserve(background_workers.size());
      for (auto& worker : background_workers) {
        workers.push_back(std::move(worker.thread));
      }
      background_workers.clear();
    }
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  };

  const auto enqueue_attach_write = [&](
                                        AttachWriteKind kind,
                                        std::string frame,
                                        bool partial_frame,
                                        std::size_t dirty_pane_count,
                                        bool replace_pending_output,
                                        bool establishes_baseline,
                                        std::optional<RenderDiffState> baseline_after_write,
                                        std::optional<std::unordered_map<PaneId, std::uint64_t>>
                                            sequences_after_write) {
    const auto current_settings = settings_snapshot();
    const auto max_pending_bytes = current_settings.limits.max_client_output_queue_bytes;
    const auto max_pending_frames = current_settings.limits.max_client_output_queue_frames;
    if (frame.size() > max_pending_bytes) {
      return false;
    }

    std::unique_lock queue_lock(write_queue_mutex);
    if (write_queue_stopping || output_closed.load(std::memory_order_relaxed)) {
      return false;
    }

    const auto would_exceed_bytes =
        pending_write_bytes + frame.size() > max_pending_bytes;
    const auto would_exceed_frames = write_queue.size() >= max_pending_frames;
    std::size_t coalesced_write_bytes = 0;
    std::uint64_t coalesced_write_frames = 0;
    bool dropped_output_that_may_have_advanced_cache = false;
    if ((replace_pending_output || would_exceed_bytes || would_exceed_frames) &&
        kind == AttachWriteKind::Output) {
      for (auto queued = write_queue.begin(); queued != write_queue.end();) {
        if (queued->kind == AttachWriteKind::Output) {
          if (queued->establishes_baseline && !establishes_baseline) {
            ++queued;
            continue;
          }
          dropped_output_that_may_have_advanced_cache = true;
          coalesced_write_bytes += queued->frame.size();
          ++coalesced_write_frames;
          pending_write_bytes -= queued->frame.size();
          queued = write_queue.erase(queued);
        } else {
          ++queued;
        }
      }
    }

    const auto still_exceeds_bytes =
        pending_write_bytes + frame.size() > max_pending_bytes;
    const auto still_exceeds_frames = write_queue.size() >= max_pending_frames;
    if (still_exceeds_bytes || still_exceeds_frames) {
      output_closed.store(true, std::memory_order_relaxed);
      write_queue_stopping = true;
      queue_lock.unlock();
      write_queue_changed.notify_all();

      events.call([&](DaemonState& state) {
        state.render_metrics.slow_clients.fetch_add(1, std::memory_order_relaxed);
        state.render_metrics.write_failures.fetch_add(1, std::memory_order_relaxed);
      });
      log_event(
          LogLevel::Warn,
          "daemon.attach",
          "client_output_backpressure",
          {{"client_id", std::to_string(client_id)},
           {"session_id", std::to_string(session_id)},
           {"queued_bytes", std::to_string(pending_write_bytes)},
           {"queued_frames", std::to_string(write_queue.size())},
           {"frame_bytes", std::to_string(frame.size())},
           {"limit_bytes", std::to_string(max_pending_bytes)},
           {"limit_frames", std::to_string(max_pending_frames)}});
      return false;
    }

    write_queue.push_back(QueuedAttachWrite{
        std::move(frame),
        kind,
        partial_frame,
        dirty_pane_count,
        establishes_baseline,
        std::move(baseline_after_write),
        std::move(sequences_after_write)});
    const auto frame_bytes = write_queue.back().frame.size();
    pending_write_bytes += frame_bytes;
    queue_lock.unlock();

    events.call([&](DaemonState& state) {
      if (coalesced_write_bytes > 0) {
        state.render_metrics.pending_client_output_bytes.fetch_sub(
            coalesced_write_bytes, std::memory_order_relaxed);
      }
      if (coalesced_write_frames > 0) {
        state.render_metrics.skipped_frames.fetch_add(
            coalesced_write_frames, std::memory_order_relaxed);
        state.render_metrics.coalesced_output_events.fetch_add(
          coalesced_write_frames, std::memory_order_relaxed);
      }
      if (dropped_output_that_may_have_advanced_cache && !establishes_baseline) {
        client_render_baseline_unknown.store(true, std::memory_order_release);
      }
      const auto current = state.render_metrics.pending_client_output_bytes.fetch_add(
                               frame_bytes, std::memory_order_relaxed) +
                           frame_bytes;
      update_atomic_peak(state.render_metrics.peak_pending_client_output_bytes, current);
    });
    if (coalesced_write_frames > 0 && should_log(LogLevel::Debug)) {
      log_event(
          LogLevel::Debug,
          "daemon.attach",
          "coalesce_client_output_frames",
          {{"client_id", std::to_string(client_id)},
           {"session_id", std::to_string(session_id)},
           {"frames", std::to_string(coalesced_write_frames)},
           {"bytes", std::to_string(coalesced_write_bytes)}});
    }
    write_queue_changed.notify_one();
    return true;
  };

  const auto queue_attach_output = [&](
                                     std::string_view payload,
                                     bool partial_frame,
                                     std::size_t dirty_pane_count,
                                     bool replace_pending_output,
                                     bool establishes_baseline,
                                     std::optional<RenderDiffState> baseline_after_write,
                                     std::optional<std::unordered_map<PaneId, std::uint64_t>>
                                         sequences_after_write) {
    auto output_frame = make_ipc_frame(IpcFrameKind::AttachOutput, next_output_frame_id++, payload);
    return enqueue_attach_write(
        AttachWriteKind::Output,
        std::move(output_frame),
        partial_frame,
        dirty_pane_count,
        replace_pending_output,
        establishes_baseline,
        std::move(baseline_after_write),
        std::move(sequences_after_write));
  };

  const auto commit_output_sequences = [&](
                                           const std::unordered_map<PaneId, std::uint64_t>&
                                               sequences_after_write) {
    if (sequences_after_write.empty()) {
      return;
    }
    std::lock_guard stream_lock(stream_mutex);
    for (const auto& [pane_id, sequence] : sequences_after_write) {
      next_sequences[pane_id] = sequence;
    }
  };

  const auto queue_attach_settings_event = [&](const DaemonAttachSettings& event_settings) {
    auto settings_frame = make_ipc_frame(
        IpcFrameKind::Event,
        next_output_frame_id++,
        make_response_json(
            true,
            "settings",
            event_settings.mouse_enabled,
            event_settings.prefix,
            event_settings.status_bar_enabled,
            event_settings.escape_time_ms,
            event_settings.key_bindings));
    return enqueue_attach_write(
        AttachWriteKind::Event,
        std::move(settings_frame),
        false,
        0,
        false,
        false,
        std::nullopt,
        std::nullopt);
  };

  const auto write_attach_error = [&](std::string_view message) {
    auto error_frame = make_ipc_frame(
        IpcFrameKind::Error,
        next_output_frame_id++,
        make_response_json(false, message));
    return enqueue_attach_write(
        AttachWriteKind::Error,
        std::move(error_frame),
        false,
        0,
        false,
        false,
        std::nullopt,
        std::nullopt);
  };

  std::thread writer_thread{[&] {
    while (true) {
      QueuedAttachWrite queued;
      {
        std::unique_lock queue_lock(write_queue_mutex);
        write_queue_changed.wait(queue_lock, [&] {
          return write_queue_stopping || !write_queue.empty();
        });
        if (write_queue.empty()) {
          if (write_queue_stopping) {
            break;
          }
          continue;
        }

        queued = std::move(write_queue.front());
        write_queue.pop_front();
        pending_write_bytes -= queued.frame.size();
      }

      events.call([&](DaemonState& state) {
        state.render_metrics.pending_client_output_bytes.fetch_sub(
            queued.frame.size(), std::memory_order_relaxed);
      });

      const auto write_started_at = std::chrono::steady_clock::now();
      if (!write_all_overlapped(pipe, queued.frame)) {
        output_closed.store(true, std::memory_order_relaxed);
        stop_requested.store(true, std::memory_order_relaxed);
        events.call([&](DaemonState& state) {
          state.render_metrics.write_failures.fetch_add(1, std::memory_order_relaxed);
        });
        break;
      }

      const auto write_elapsed = std::chrono::steady_clock::now() - write_started_at;
      if (write_elapsed > kSlowClientWriteTimeout / 2) {
        events.call([&](DaemonState& state) {
          state.render_metrics.slow_clients.fetch_add(1, std::memory_order_relaxed);
        });
        log_event(
            LogLevel::Warn,
            "daemon.attach",
            "slow_client_write",
            {{"client_id", std::to_string(client_id)},
             {"session_id", std::to_string(session_id)},
             {"bytes", std::to_string(queued.frame.size())},
             {"elapsed_ms",
              std::to_string(
                  std::chrono::duration_cast<std::chrono::milliseconds>(write_elapsed).count())}});
      }

      if (queued.kind != AttachWriteKind::Output) {
        continue;
      }

      if (queued.sequences_after_write) {
        commit_output_sequences(*queued.sequences_after_write);
      }

      if (queued.baseline_after_write) {
        std::lock_guard baseline_lock(scene_baseline_mutex);
        client_scene_baseline = std::move(*queued.baseline_after_write);
      }

      if (queued.establishes_baseline) {
        client_render_baseline_unknown.store(false, std::memory_order_release);
      }

      const auto write_micros = elapsed_us(write_started_at);
      events.call([&](DaemonState& state) {
        state.render_metrics.frames_written.fetch_add(1, std::memory_order_relaxed);
        state.render_metrics.bytes_written.fetch_add(queued.frame.size(), std::memory_order_relaxed);
        state.render_metrics.client_write_duration_us.fetch_add(
            write_micros, std::memory_order_relaxed);
        update_atomic_peak(state.render_metrics.max_client_write_duration_us, write_micros);
        if (queued.partial_frame) {
          state.render_metrics.partial_frames_written.fetch_add(1, std::memory_order_relaxed);
          state.render_metrics.dirty_panes_rendered.fetch_add(
              queued.dirty_pane_count, std::memory_order_relaxed);
        } else {
          state.render_metrics.full_frames_written.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  }};

  const auto stop_writer = [&](bool drain_queue) {
    std::size_t abandoned_write_bytes = 0;
    {
      std::lock_guard queue_lock(write_queue_mutex);
      write_queue_stopping = true;
      if (!drain_queue) {
        abandoned_write_bytes = pending_write_bytes;
        write_queue.clear();
        pending_write_bytes = 0;
      }
    }
    if (abandoned_write_bytes > 0) {
      events.call([&](DaemonState& state) {
        state.render_metrics.pending_client_output_bytes.fetch_sub(
            abandoned_write_bytes, std::memory_order_relaxed);
        state.render_metrics.skipped_frames.fetch_add(1, std::memory_order_relaxed);
      });
    }
    if (!drain_queue) {
      CancelIoEx(pipe, nullptr);
    }
    write_queue_changed.notify_all();
    if (writer_thread.joinable()) {
      writer_thread.join();
    }
  };

  const auto reserve_status_row = [&] {
    const auto current_settings = settings_snapshot();
    std::lock_guard stream_lock(stream_mutex);
    return current_settings.status_bar_enabled ||
           status_has_visible_temporary(status_state, std::chrono::steady_clock::now()) ||
           copy_mode.active;
  };

  const auto render_active_window_scene = [&](
                                            std::string& error,
                                            std::optional<AttachScrollAction> scroll,
                                            std::optional<AttachCopyModeAction> copy_action,
                                            SceneInvalidationPolicy scene_policy,
                                            const std::unordered_set<PaneId>& dirty_panes) {
    if (client_render_baseline_unknown.load(std::memory_order_acquire) &&
        scene_policy != SceneInvalidationPolicy::InitialAttach &&
        scene_policy != SceneInvalidationPolicy::FullScene) {
      scene_policy = SceneInvalidationPolicy::FullScene;
    }

    std::string scene_frame;
    bool partial_frame = false;
    const auto render_started_at = std::chrono::steady_clock::now();
    std::uint64_t geometry_micros = 0;
    std::uint64_t snapshot_micros = 0;
    std::uint64_t state_micros = 0;
    std::uint64_t diff_micros = 0;
    std::uint64_t queue_micros = 0;
    RenderFrameStats render_stats;
    std::unordered_map<PaneId, std::uint64_t> frame_next_sequences;
    RenderDiffState pending_scene_baseline;

    {
      std::lock_guard render_order_lock(render_mutex);
      const auto current_settings = settings_snapshot();
      const bool reserve_row_for_frame = [&] {
        std::lock_guard stream_lock(stream_mutex);
        return current_settings.status_bar_enabled ||
               status_has_visible_temporary(status_state, std::chrono::steady_clock::now()) ||
               copy_mode.active ||
               (copy_action && *copy_action != AttachCopyModeAction::Exit);
      }();

      auto frame = events.call([&](DaemonState& state) {
        return active_window_frame(
            state,
            session_id,
            current_columns.load(std::memory_order_relaxed),
            current_rows.load(std::memory_order_relaxed),
            current_settings.status_bar_enabled,
            reserve_row_for_frame,
            error);
      });
      if (!frame) {
        return false;
      }

      const auto resize_plan = events.call([&](DaemonState& state) {
        return collect_pending_pane_resizes(state, session_id, *frame);
      });
      std::size_t applied_resizes = 0;
      std::vector<PendingPaneResize> failed_resizes;
      failed_resizes.reserve(resize_plan.pending.size());
      for (const auto& resize : resize_plan.pending) {
        if (!resize.shell || resize.shell->resize(resize.columns, resize.rows)) {
          ++applied_resizes;
          continue;
        }

        failed_resizes.push_back(resize);
        log_event(
            LogLevel::Warn,
            "daemon.pane",
            "resize_failed",
            {{"session_id", std::to_string(session_id)},
             {"window_id", std::to_string(resize.window_id)},
             {"pane_id", std::to_string(resize.pane_id)},
             {"process_id", std::to_string(resize.shell->process_id())},
             {"columns", std::to_string(resize.columns)},
             {"rows", std::to_string(resize.rows)}});
      }
      if (applied_resizes > 0 || !failed_resizes.empty()) {
        events.call([&](DaemonState& state) {
          record_pane_resize_results(state, session_id, applied_resizes, failed_resizes);
        });
      }
      if ((applied_resizes > 0 || !failed_resizes.empty()) && should_log(LogLevel::Debug)) {
        log_event(
            LogLevel::Debug,
            "daemon.attach",
            "scene_after_resize",
            {{"client_id", std::to_string(client_id)},
             {"session_id", std::to_string(session_id)},
             {"panes", std::to_string(frame->panes.size())},
             {"applied_resizes", std::to_string(applied_resizes)},
             {"failed_resizes", std::to_string(failed_resizes.size())}});
      }
      geometry_micros = elapsed_us(render_started_at);

      const bool has_scrolled_viewport = [&] {
        std::lock_guard stream_lock(stream_mutex);
        return std::ranges::any_of(viewport_states, [](const auto& entry) {
          return entry.second.offset > 0;
        });
      }();
      const bool needs_full_history = [&] {
        if (copy_action) {
          return true;
        }
        std::lock_guard stream_lock(stream_mutex);
        if (copy_mode.active) {
          return true;
        }
        if (scroll || has_scrolled_viewport) {
          return false;
        }
        return false;
      }();
      const bool bounded_scroll_history = [&] {
        if ((!scroll && !has_scrolled_viewport) || copy_action) {
          return false;
        }
        std::lock_guard stream_lock(stream_mutex);
        return !copy_mode.active;
      }();
      const auto snapshot_mode = needs_full_history ? PtyOutputSnapshotMode::FullHistory
                                                    : PtyOutputSnapshotMode::ScreenOnly;
      const bool dirty_rows_only_snapshot =
          render_mode_allows_dirty_row_snapshots(render_mode) &&
          scene_policy == SceneInvalidationPolicy::OutputDelta &&
          !dirty_panes.empty() &&
          !scroll && !copy_action && snapshot_mode == PtyOutputSnapshotMode::ScreenOnly;
      const bool can_use_live_line_views =
          terminal_engine_kind_from_environment() == TerminalEngineKind::V2 &&
          snapshot_mode == PtyOutputSnapshotMode::ScreenOnly &&
          !scroll && !copy_action && !has_scrolled_viewport;

      std::unordered_map<PaneId, PtyOutputSnapshot> snapshots;
      if (!can_use_live_line_views) {
        snapshots.reserve(frame->panes.size());
        const auto snapshot_started_at = std::chrono::steady_clock::now();
        for (const auto& pane : frame->panes) {
          snapshots.emplace(
              pane.rect.pane_id,
              pane.shell->output_snapshot(snapshot_mode, true, dirty_rows_only_snapshot));
        }
        snapshot_micros = elapsed_us(snapshot_started_at);
        if (should_log(LogLevel::Debug)) {
          log_event(
              LogLevel::Debug,
              "daemon.attach",
              "scene_snapshots_ready",
              {{"client_id", std::to_string(client_id)},
               {"session_id", std::to_string(session_id)},
               {"snapshots", std::to_string(snapshots.size())},
               {"history", needs_full_history ? "full" : "screen-only"}});
        }
      }

      {
        const auto state_started_at = std::chrono::steady_clock::now();
        std::lock_guard stream_lock(stream_mutex);
        (void)status_expire_temporary(status_state, std::chrono::steady_clock::now());
        if (!can_use_live_line_views) {
          update_viewport_states(*frame, snapshots, viewport_states, copy_mode);
        }
        if (scroll) {
          (void)apply_active_viewport_scroll(*frame, snapshots, viewport_states, *scroll);
        }
        if (!can_use_live_line_views && bounded_scroll_history) {
          for (const auto& pane : frame->panes) {
            const auto snapshot = snapshots.find(pane.rect.pane_id);
            if (snapshot == snapshots.end()) {
              continue;
            }
            const auto viewport = viewport_states.find(pane.rect.pane_id);
            if (viewport == viewport_states.end() || viewport->second.offset == 0) {
              continue;
            }
            const int frame_pane_rows =
                frame->pane_rows > 0
                    ? frame->pane_rows
                    : std::max(
                          1,
                          frame->rows -
                              (frame->status_bar_enabled && frame->rows > 1 ? 1 : 0));
            const int height = body_height(pane.rect, frame_pane_rows);
            if (height <= 0) {
              continue;
            }
            const auto scrollback_lines = snapshot->second.screen.scrollback_line_count;
            const auto screen_lines =
                std::max(
                    snapshot->second.screen.line_snapshots.size(),
                    snapshot->second.screen.lines.size());
            const auto screen_line_count =
                screen_lines == 0
                    ? static_cast<std::size_t>(std::max(0, snapshot->second.screen.rows))
                    : screen_lines;
            const auto total_lines = scrollback_lines + screen_line_count;
            const auto visible_lines =
                std::min<std::size_t>(total_lines, static_cast<std::size_t>(height));
            const auto max_offset =
                total_lines > visible_lines ? total_lines - visible_lines : std::size_t{0};
            const auto clamped_offset = std::min(viewport->second.offset, max_offset);
            const auto first_visible = total_lines - visible_lines - clamped_offset;
            if (first_visible >= scrollback_lines) {
              continue;
            }
            const auto wanted_lines =
                std::min<std::size_t>(
                    static_cast<std::size_t>(height),
                    scrollback_lines - first_visible);
            snapshot->second.scrollback_included = true;
            snapshot->second.scrollback =
                pane.shell->output_snapshot(
                    PtyOutputSnapshotMode::FullHistory,
                    false,
                    false,
                    PtyOutputScrollbackRange{first_visible, wanted_lines})
                    .scrollback;
          }
        }
        if (!can_use_live_line_views && copy_action) {
          std::string copied_text;
          const auto applied = apply_copy_mode_action(
              *frame, snapshots, viewport_states, copy_mode, *copy_action, copied_text);
          switch (*copy_action) {
            case AttachCopyModeAction::Enter:
              status_set_temporary(
                  status_state,
                  "wmux: copy mode",
                  std::chrono::steady_clock::now());
              status_mode = StatusLineMode::Normal;
              break;
            case AttachCopyModeAction::Exit:
              status_set_temporary(
                  status_state,
                  "wmux: exited copy mode",
                  std::chrono::steady_clock::now());
              status_mode = StatusLineMode::Normal;
              break;
            case AttachCopyModeAction::StartSelection:
              status_set_temporary(
                  status_state,
                  "wmux: selection started",
                  std::chrono::steady_clock::now());
              status_mode = StatusLineMode::Normal;
              break;
            case AttachCopyModeAction::CopySelection:
              if (applied) {
                (void)events.call_event(
                    DaemonEvent::set_paste_buffer(copied_text, PasteBufferSource::CopyMode));
                const auto paste_limit = events.call([](DaemonState& state) {
                  std::lock_guard lock(state.mutex);
                  return state.config.values.limits.max_paste_buffer_bytes;
                });
                auto clipboard_text = bounded_paste_buffer_text(copied_text, paste_limit);
                std::string status =
                    "wmux: copied " + std::to_string(copied_text.size()) + " bytes";
                if (copied_text.size() > clipboard_text.size()) {
                  status += "; truncated";
                }
                status += "; clipboard queued";
                status_set_temporary(
                    status_state,
                    status,
                    std::chrono::steady_clock::now());
                status_mode = StatusLineMode::Normal;
                start_background_worker(
                    [clipboard_text = std::move(clipboard_text),
                     client_id,
                     session_id,
                     set_background_status] {
                      const auto clipboard =
                          platform_services().clipboard().write_text(clipboard_text);
                      if (clipboard.ok) {
                        if (!should_log(LogLevel::Debug)) {
                          return;
                        }
                        log_event(
                            LogLevel::Debug,
                            "daemon.attach",
                            "clipboard_write_ok",
                            {{"client_id", std::to_string(client_id)},
                             {"session_id", std::to_string(session_id)},
                             {"bytes", std::to_string(clipboard_text.size())}});
                        return;
                      }

                      log_event(
                          LogLevel::Warn,
                          "daemon.attach",
                          "clipboard_write_failed",
                          {{"client_id", std::to_string(client_id)},
                           {"session_id", std::to_string(session_id)},
                           {"error", clipboard.error}});
                      set_background_status("wmux: clipboard unavailable");
                    });
              } else {
                status_set_temporary(
                    status_state,
                    "wmux: no copy selection",
                    std::chrono::steady_clock::now());
                status_mode = StatusLineMode::Normal;
              }
              break;
            default:
              break;
          }
        }
        if (!can_use_live_line_views) {
          clamp_copy_mode_cursor(copy_mode, *frame, snapshots);
        }

        const bool force_full_latest_only = render_mode == AttachRenderMode::FullLatestOnly;
        partial_frame = !force_full_latest_only &&
                        scene_policy == SceneInvalidationPolicy::OutputDelta &&
                        !dirty_panes.empty() &&
                        !scroll && !copy_action;
        RenderStatus render_status;
        render_status.state = status_state;
        render_status.mode = status_mode;
        render_status.mouse_enabled = current_settings.mouse_enabled;
        render_status.synchronized_output_supported =
            current_settings.synchronized_output_supported;
        render_status.ui = current_settings.ui;
        const auto diff_started_at = std::chrono::steady_clock::now();
        const auto render_options_for_scene_policy = [&](SceneInvalidationPolicy policy) {
          RenderFrameOptions options;
          if (policy == SceneInvalidationPolicy::InitialAttach) {
            options.clear_terminal = false;
            options.draw_borders = true;
            options.draw_status = true;
            options.draw_pane_bodies = true;
            options.force_body_repaint = false;
            return options;
          }
          if (policy == SceneInvalidationPolicy::LatestViewport) {
            options.clear_terminal = false;
            options.draw_borders = true;
            options.draw_status = true;
            options.draw_pane_bodies = true;
            options.force_body_repaint = true;
            return options;
          }
          if (policy == SceneInvalidationPolicy::FullScene) {
            options.clear_terminal = false;
            options.draw_borders = true;
            options.draw_status = true;
            options.draw_pane_bodies = true;
            options.force_body_repaint = true;
            return options;
          }
          if (policy == SceneInvalidationPolicy::LayoutOnly) {
            options.clear_terminal = false;
            options.draw_borders = true;
            options.draw_status = true;
            options.draw_pane_bodies = true;
            options.force_body_repaint = false;
            options.preserve_layout_cache = true;
            options.repaint_body_on_geometry_change = true;
            return options;
          }
          if (policy == SceneInvalidationPolicy::SceneDelta) {
            options.clear_terminal = false;
            options.draw_borders = true;
            options.draw_status = true;
            options.draw_pane_bodies = true;
            options.force_body_repaint = false;
            return options;
          }
          return options;
        };
        {
          std::lock_guard baseline_lock(scene_baseline_mutex);
          pending_scene_baseline = client_scene_baseline;
        }
        const auto should_reset_baseline_for_scene_policy = [&](SceneInvalidationPolicy policy) {
          return policy == SceneInvalidationPolicy::InitialAttach ||
                 policy == SceneInvalidationPolicy::FullScene ||
                 policy == SceneInvalidationPolicy::LatestViewport ||
                 render_mode == AttachRenderMode::FullLatestOnly;
        };

        if (should_reset_baseline_for_scene_policy(scene_policy)) {
          reset_render_diff_state(pending_scene_baseline);
        }

        if (can_use_live_line_views) {
          std::unordered_map<PaneId, std::uint64_t> live_next_sequences;
          if (scene_frame.empty()) {
            RenderFrameOptions live_options = render_options_for_scene_policy(scene_policy);
            live_options.dirty_panes = dirty_panes;
            scene_frame = render_live_frame_update(
                *frame,
                viewport_states,
                copy_mode,
                render_status,
                live_options,
                pending_scene_baseline,
                live_next_sequences,
                scene_policy == SceneInvalidationPolicy::InitialAttach ? &render_stats : nullptr);
          }
          frame_next_sequences = std::move(live_next_sequences);
        } else {
          RenderFrameOptions snapshot_options = render_options_for_scene_policy(scene_policy);
          snapshot_options.dirty_panes = dirty_panes;
          scene_frame = render_frame_update(
              *frame,
              snapshots,
              viewport_states,
              copy_mode,
              render_status,
              snapshot_options,
              pending_scene_baseline);
        }
        diff_micros = elapsed_us(diff_started_at);
        if (should_log(LogLevel::Debug)) {
          log_event(
              LogLevel::Debug,
              "daemon.attach",
              "scene_rendered",
              {{"client_id", std::to_string(client_id)},
               {"session_id", std::to_string(session_id)},
               {"scene_policy", std::string{scene_invalidation_policy_name(scene_policy)}},
               {"bytes", std::to_string(scene_frame.size())},
               {"partial", partial_frame ? "true" : "false"}});
        }

        current_shells.clear();
        current_shells.reserve(frame->panes.size());
        frame_next_sequences.reserve(frame->panes.size());
        for (const auto& pane : frame->panes) {
          current_shells.emplace(pane.rect.pane_id, pane.shell);
          if (!can_use_live_line_views) {
            const auto snapshot = snapshots.find(pane.rect.pane_id);
            frame_next_sequences.emplace(
                pane.rect.pane_id,
                snapshot == snapshots.end() ? 1 : snapshot->second.next_sequence);
          }
        }
        const auto state_total = elapsed_us(state_started_at);
        state_micros = state_total > diff_micros ? state_total - diff_micros : 0;
      }

      if (scene_frame.empty()) {
        if (!frame_next_sequences.empty()) {
          commit_output_sequences(frame_next_sequences);
        }
        events.call([&](DaemonState& state) {
          state.render_metrics.skipped_frames.fetch_add(1, std::memory_order_relaxed);
        });
        return true;
      }

      if (scene_frame.size() > current_settings.limits.max_attach_render_frame_bytes) {
        log_event(
            LogLevel::Error,
            "daemon.attach",
            "render_frame_too_large",
            {{"client_id", std::to_string(client_id)},
             {"session_id", std::to_string(session_id)},
             {"bytes", std::to_string(scene_frame.size())},
             {"limit", std::to_string(current_settings.limits.max_attach_render_frame_bytes)}});
        output_closed = true;
        return false;
      }

      const auto render_micros = elapsed_us(render_started_at);
      last_render_duration_us.store(render_micros, std::memory_order_relaxed);
      const auto queue_started_at = std::chrono::steady_clock::now();
      const bool has_pending_scene_baseline = pending_scene_baseline.baseline_valid;
      const bool baseline_frame =
          scene_policy == SceneInvalidationPolicy::InitialAttach ||
          scene_policy == SceneInvalidationPolicy::FullScene ||
          scene_policy == SceneInvalidationPolicy::SceneDelta ||
          scene_policy == SceneInvalidationPolicy::LayoutOnly ||
          scene_policy == SceneInvalidationPolicy::LatestViewport;
      const bool replace_pending_output = baseline_frame;
      const bool establishes_baseline = has_pending_scene_baseline;
      if (!baseline_frame) {
        commit_output_sequences(frame_next_sequences);
      }
      if (!queue_attach_output(
              scene_frame,
              partial_frame,
              dirty_panes.size(),
              replace_pending_output,
              establishes_baseline,
              has_pending_scene_baseline
                  ? std::optional<RenderDiffState>{std::move(pending_scene_baseline)}
                  : std::nullopt,
              frame_next_sequences.empty() || !baseline_frame
                  ? std::nullopt
                  : std::optional<std::unordered_map<PaneId, std::uint64_t>>{
                        frame_next_sequences})) {
        output_closed.store(true, std::memory_order_relaxed);
        return false;
      }
      queue_micros = elapsed_us(queue_started_at);

      if (scene_policy == SceneInvalidationPolicy::InitialAttach) {
        std::uint64_t min_sequence = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t max_sequence = 0;
        std::size_t sequence_count = 0;
        for (const auto& [pane_id, sequence] : frame_next_sequences) {
          (void)pane_id;
          min_sequence = std::min(min_sequence, sequence);
          max_sequence = std::max(max_sequence, sequence);
        }
        sequence_count = frame_next_sequences.size();
        if (min_sequence == std::numeric_limits<std::uint64_t>::max()) {
          min_sequence = 0;
        }
        log_event(
            LogLevel::Info,
            "daemon.attach",
             "first_paint",
             {{"client_id", std::to_string(client_id)},
              {"session_id", std::to_string(session_id)},
             {"scene_policy", std::string{scene_invalidation_policy_name(scene_policy)}},
             {"panes", std::to_string(sequence_count)},
             {"frame_bytes", std::to_string(scene_frame.size())},
             {"render_us", std::to_string(render_micros)},
             {"geometry_us", std::to_string(geometry_micros)},
             {"snapshot_us", std::to_string(snapshot_micros)},
             {"state_us", std::to_string(state_micros)},
             {"frame_build_us", std::to_string(diff_micros)},
             {"queued_us", std::to_string(queue_micros)},
             {"pending_output_total_at_attach", "0"},
             {"fast_forward_skipped_frames_before_first_paint", "0"},
             {"visible_pane_rows", std::to_string(render_stats.visible_pane_rows)},
             {"rows_considered", std::to_string(render_stats.rows_considered)},
             {"rows_emitted", std::to_string(render_stats.rows_emitted)},
             {"rows_skipped_generation_cache",
              std::to_string(render_stats.rows_skipped_generation_cache)},
             {"rows_skipped_empty_default",
              std::to_string(render_stats.rows_skipped_empty_default)},
             {"body_bytes_emitted", std::to_string(render_stats.body_bytes_emitted)},
             {"border_status_bytes_emitted",
              std::to_string(render_stats.border_status_bytes_emitted)},
             {"cursor_bytes_emitted", std::to_string(render_stats.cursor_bytes_emitted)},
             {"alternate_screen_panes", std::to_string(render_stats.alternate_screen_panes)},
             {"pane_sequence_min", std::to_string(min_sequence)},
             {"pane_sequence_max", std::to_string(max_sequence)}});
      }

      events.call([&](DaemonState& state) {
        state.render_metrics.render_frame_duration_us.fetch_add(
            render_micros, std::memory_order_relaxed);
        update_atomic_peak(state.render_metrics.max_render_frame_duration_us, render_micros);
        state.render_metrics.render_geometry_duration_us.fetch_add(
            geometry_micros, std::memory_order_relaxed);
        update_atomic_peak(
            state.render_metrics.max_render_geometry_duration_us, geometry_micros);
        state.render_metrics.render_snapshot_duration_us.fetch_add(
            snapshot_micros, std::memory_order_relaxed);
        update_atomic_peak(
            state.render_metrics.max_render_snapshot_duration_us, snapshot_micros);
        state.render_metrics.render_state_duration_us.fetch_add(
            state_micros, std::memory_order_relaxed);
        update_atomic_peak(state.render_metrics.max_render_state_duration_us, state_micros);
        state.render_metrics.render_diff_duration_us.fetch_add(
            diff_micros, std::memory_order_relaxed);
        update_atomic_peak(state.render_metrics.max_render_diff_duration_us, diff_micros);
        state.render_metrics.render_queue_duration_us.fetch_add(
            queue_micros, std::memory_order_relaxed);
        update_atomic_peak(state.render_metrics.max_render_queue_duration_us, queue_micros);
      });
    }

    return true;
  };

  const auto render_initial_attach_scene = [&](std::string& error) {
    static const std::unordered_set<PaneId> kNoDirtyPanes;
    return render_active_window_scene(
        error,
        std::nullopt,
        std::nullopt,
        SceneInvalidationPolicy::InitialAttach,
        kNoDirtyPanes);
  };

  const auto render_scene_delta = [&](
                                      std::string& error,
                                      std::optional<AttachScrollAction> scroll,
                                      std::optional<AttachCopyModeAction> copy_action) {
    static const std::unordered_set<PaneId> kNoDirtyPanes;
    return render_active_window_scene(
        error, scroll, copy_action, SceneInvalidationPolicy::SceneDelta, kNoDirtyPanes);
  };

  const auto layout_active_window = [&](std::string& error) {
    static const std::unordered_set<PaneId> kNoDirtyPanes;
    return render_active_window_scene(
        error, std::nullopt, std::nullopt, SceneInvalidationPolicy::LayoutOnly, kNoDirtyPanes);
  };

  const auto refresh_attach_settings = [&] {
    auto latest = events.call([&](DaemonState& state) {
      return attach_settings_for_client(state, client_id);
    });
    if (!replace_settings(latest)) {
      return true;
    }

    log_event(
        LogLevel::Info,
        "daemon.attach",
        "settings_updated",
        {{"client_id", std::to_string(client_id)},
         {"session_id", std::to_string(session_id)},
         {"mouse", latest.mouse_enabled ? "on" : "off"},
         {"prefix", latest.prefix},
         {"status", latest.status_bar_enabled ? "on" : "off"},
         {"escape_time_ms", std::to_string(latest.escape_time_ms)},
         {"accent", latest.ui.accent_spec},
         {"border_style", latest.ui.smooth_borders ? "smooth" : "ascii"},
         {"key_bindings_bytes", std::to_string(latest.key_bindings.size())}});
    status_dirty.store(true, std::memory_order_release);
    return queue_attach_settings_event(latest);
  };

  {
    std::string error;
    log_event(
        LogLevel::Info,
        "daemon.attach",
         "first_paint_start",
         {{"client_id", std::to_string(client_id)},
          {"session_id", std::to_string(session_id)},
         {"scene_policy",
          std::string{scene_invalidation_policy_name(SceneInvalidationPolicy::InitialAttach)}}});
    if (!render_initial_attach_scene(error)) {
      if (!error.empty()) {
        (void)write_attach_error(error);
      }
      stop_requested = true;
      stop_writer(true);
      close_attach_pipe(pipe);
      (void)events.call_event(
          DaemonEvent::client_disconnected(client_id, AttachEndReason::OutputClosed));
      return;
    }
  }

  std::thread output_thread{[&] {
    auto last_output_redraw = std::chrono::steady_clock::time_point{};
    auto first_deferred_output = std::chrono::steady_clock::time_point{};
    auto last_deferred_output = std::chrono::steady_clock::time_point{};
    std::unordered_map<PaneId, std::size_t> pending_output_bytes_by_pane;
    std::size_t pending_output_total = 0;
    std::unordered_set<PaneId> deferred_dirty_panes;

    const auto set_pending_output_bytes = [&](PaneId pane_id, std::size_t bytes) {
      const auto previous = pending_output_bytes_by_pane[pane_id];
      if (bytes == previous) {
        return;
      }

      pending_output_bytes_by_pane[pane_id] = bytes;
      if (bytes > previous) {
        const auto delta = bytes - previous;
        pending_output_total += delta;
        events.call([&](DaemonState& state) {
          const auto current = state.render_metrics.pending_pane_output_bytes.fetch_add(
                                   delta, std::memory_order_relaxed) +
                               delta;
          update_atomic_peak(state.render_metrics.peak_pending_pane_output_bytes, current);
        });
      } else {
        const auto delta = previous - bytes;
        pending_output_total -= std::min(pending_output_total, delta);
        events.call([&](DaemonState& state) {
          state.render_metrics.pending_pane_output_bytes.fetch_sub(
              delta, std::memory_order_relaxed);
        });
      }
    };

    const auto clear_pending_output_bytes = [&] {
      if (pending_output_total == 0) {
        pending_output_bytes_by_pane.clear();
        return;
      }

      const auto total = pending_output_total;
      pending_output_bytes_by_pane.clear();
      pending_output_total = 0;
      events.call([&](DaemonState& state) {
        state.render_metrics.pending_pane_output_bytes.fetch_sub(
            total, std::memory_order_relaxed);
      });
    };

    while (!stop_requested) {
      reap_background_workers();
      if (!refresh_attach_settings()) {
        stop_requested = true;
        break;
      }
      bool status_expired = false;
      {
        std::lock_guard stream_lock(stream_mutex);
        status_expired = status_expire_temporary(status_state, std::chrono::steady_clock::now());
        if (status_expired && status_mode != StatusLineMode::CommandPrompt) {
          status_mode = StatusLineMode::Normal;
        }
      }
      if (status_dirty.exchange(false, std::memory_order_acq_rel) || status_expired) {
        std::string error;
        if (!layout_active_window(error)) {
          stop_requested = true;
          break;
        }
        last_output_redraw = std::chrono::steady_clock::now();
      }

      std::vector<std::pair<PaneId, std::shared_ptr<PtyProcess>>> shells;
      std::unordered_map<PaneId, std::uint64_t> sequences;
      {
        std::lock_guard stream_lock(stream_mutex);
        shells.reserve(current_shells.size());
        for (const auto& [pane_id, shell] : current_shells) {
          shells.emplace_back(pane_id, shell);
        }
        sequences = next_sequences;
      }

      if (shells.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        continue;
      }

      std::unordered_set<PaneId> dirty_panes;
      std::unordered_map<PaneId, std::uint64_t> drained_sequences;
      std::size_t coalesced_output_events = 0;
      for (const auto& [pane_id, shell] : shells) {
        const auto sequence = sequences.find(pane_id);
        if (sequence == sequences.end()) {
          continue;
        }

        const auto output =
            shell->wait_for_output_drain(sequence->second, std::chrono::milliseconds{0});
        if (output.bytes > 0) {
          const auto previous = pending_output_bytes_by_pane[pane_id];
          if (previous > 0 && output.bytes > previous) {
            ++coalesced_output_events;
          }
          set_pending_output_bytes(pane_id, output.bytes);
          dirty_panes.insert(pane_id);
          drained_sequences[pane_id] = output.next_sequence;
        }
      }

      const auto maybe_render_deferred_latest = [&]() -> bool {
        if (deferred_dirty_panes.empty()) {
          return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (last_deferred_output.time_since_epoch().count() != 0 &&
            now - last_deferred_output < kV2BurstQuietWindow) {
          return false;
        }

        std::string error;
        if (!render_active_window_scene(
                error,
                std::nullopt,
                std::nullopt,
                SceneInvalidationPolicy::LatestViewport,
                deferred_dirty_panes)) {
          stop_requested = true;
          return true;
        }
        deferred_dirty_panes.clear();
        first_deferred_output = std::chrono::steady_clock::time_point{};
        last_deferred_output = std::chrono::steady_clock::time_point{};
        last_output_redraw = std::chrono::steady_clock::now();
        return true;
      };

      if (!dirty_panes.empty()) {
        const auto now = std::chrono::steady_clock::now();
        bool v2_presenting_deferred_burst = false;
        const bool using_v2_engine =
            terminal_engine_kind_from_environment() == TerminalEngineKind::V2;
        const auto redraw_interval =
            using_v2_engine && render_mode == AttachRenderMode::Auto
                ? v2_smooth_output_redraw_interval(
                      last_render_duration_us.load(std::memory_order_relaxed))
                : output_redraw_interval_for_mode(
                      render_mode,
                      pending_output_total,
                      last_render_duration_us.load(std::memory_order_relaxed));
        if (last_output_redraw.time_since_epoch().count() != 0) {
          const auto next_allowed = last_output_redraw + redraw_interval;
          if (now < next_allowed) {
            std::this_thread::sleep_for(next_allowed - now);
          }
        }

        if (stop_requested) {
          break;
        }

        {
          std::lock_guard stream_lock(stream_mutex);
          sequences = next_sequences;
        }
        for (const auto& [pane_id, shell] : shells) {
          const auto sequence = sequences.find(pane_id);
          if (sequence == sequences.end()) {
            continue;
          }

          const auto output =
              shell->wait_for_output_drain(sequence->second, std::chrono::milliseconds{0});
          if (output.bytes > 0) {
            const auto previous = pending_output_bytes_by_pane[pane_id];
            if (previous > 0 && output.bytes > previous) {
              ++coalesced_output_events;
            }
            set_pending_output_bytes(pane_id, output.bytes);
            dirty_panes.insert(pane_id);
            drained_sequences[pane_id] = output.next_sequence;
          }
        }

        if (coalesced_output_events > 0) {
          events.call([&](DaemonState& state) {
            state.render_metrics.coalesced_output_events.fetch_add(
                coalesced_output_events, std::memory_order_relaxed);
          });
        }

        const bool v2_burst_fast_forward =
            using_v2_engine && render_mode == AttachRenderMode::Auto &&
            (pending_output_total >= kV2BurstFastForwardPendingBytes ||
             coalesced_output_events >= kV2BurstFastForwardCoalescedEvents ||
             last_render_duration_us.load(std::memory_order_relaxed) >= 16'000);
        if (v2_burst_fast_forward) {
          deferred_dirty_panes.insert(dirty_panes.begin(), dirty_panes.end());
          if (first_deferred_output.time_since_epoch().count() == 0) {
            first_deferred_output = now;
          }
          last_deferred_output = now;
          commit_output_sequences(drained_sequences);
          clear_pending_output_bytes();

          const bool presentation_overdue =
              now - first_deferred_output >= kV2BurstMaxPresentationInterval;
          if (!presentation_overdue) {
            events.call([&](DaemonState& state) {
              state.render_metrics.skipped_frames.fetch_add(1, std::memory_order_relaxed);
            });
            std::this_thread::sleep_for(kV2IdleOutputPoll);
            continue;
          }

          dirty_panes = deferred_dirty_panes;
          v2_presenting_deferred_burst = true;
        }

        const bool force_full_output =
            render_mode == AttachRenderMode::FullLatestOnly ||
            render_mode == AttachRenderMode::BacklogFullFrame ||
            (!using_v2_engine &&
             (pending_output_total >= kBacklogRenderPendingBytes ||
              coalesced_output_events >= kBacklogRenderCoalescedEvents));
        const bool v2_latest_viewport_output = v2_presenting_deferred_burst;
        const auto output_scene_policy =
            v2_latest_viewport_output
                ? SceneInvalidationPolicy::LatestViewport
                : (force_full_output
                       ? SceneInvalidationPolicy::FullScene
                       : SceneInvalidationPolicy::OutputDelta);

        std::string error;
        if (!render_active_window_scene(
                error,
                std::nullopt,
                std::nullopt,
                output_scene_policy,
                dirty_panes)) {
          stop_requested = true;
          break;
        }
        clear_pending_output_bytes();
        if (!deferred_dirty_panes.empty()) {
          deferred_dirty_panes.clear();
          first_deferred_output = std::chrono::steady_clock::time_point{};
          last_deferred_output = std::chrono::steady_clock::time_point{};
        }
        last_output_redraw = std::chrono::steady_clock::now();
      } else {
        if (maybe_render_deferred_latest()) {
          if (stop_requested) {
            break;
          }
          continue;
        }
        const bool using_v2_engine =
            terminal_engine_kind_from_environment() == TerminalEngineKind::V2;
        std::this_thread::sleep_for(
            using_v2_engine && render_mode == AttachRenderMode::Auto
                ? kV2IdleOutputPoll
                : std::chrono::milliseconds{8});
      }
    }
    clear_pending_output_bytes();
  }};

  AttachEndReason end_reason = AttachEndReason::ClientDisconnected;
  while (!stop_requested) {
    reap_background_workers();
    AttachFrame frame;
    if (!read_attach_frame(pipe, frame, end_reason, stop_requested)) {
      break;
    }

    if (frame.type == AttachFrameType::Detach) {
      end_reason = AttachEndReason::Detached;
      break;
    }

    if (frame.type == AttachFrameType::Command) {
      std::string error;
      const auto current_settings = settings_snapshot();
      auto command_result = events.call_event(DaemonEvent::attach_command(
          client_id,
          session_id,
          frame.payload,
          static_cast<std::uint16_t>(current_columns.load(std::memory_order_relaxed)),
          static_cast<std::uint16_t>(current_rows.load(std::memory_order_relaxed)),
          current_settings.status_bar_enabled));
      {
        if (command_result.ok) {
          set_temporary_status(command_result.status);
        } else {
          set_temporary_status(
              command_result.error.empty() ? "wmux: command failed" : command_result.error);
        }
      }
      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Resize) {
      const auto dimensions = parse_attach_resize_payload(frame.payload);
      if (!dimensions) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      current_columns.store(static_cast<short>(dimensions->first), std::memory_order_relaxed);
      current_rows.store(static_cast<short>(dimensions->second), std::memory_order_relaxed);
      const auto resize_result = events.call_event(DaemonEvent::client_resize(
          client_id,
          dimensions->first,
          dimensions->second));
      if (!resize_result.changed) {
        continue;
      }

      std::string error;
      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::MouseFocus) {
      const auto focus = parse_attach_mouse_focus_payload(frame.payload);
      if (!focus) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      const bool reserved_status_row = reserve_status_row();
      const auto focus_result = events.call_event(DaemonEvent::mouse_focus(
          client_id,
          session_id,
          *focus,
          static_cast<std::uint16_t>(current_columns.load(std::memory_order_relaxed)),
          static_cast<std::uint16_t>(current_rows.load(std::memory_order_relaxed)),
          reserved_status_row));
      if (!focus_result.changed) {
        if (!focus_result.ok && !focus_result.error.empty()) {
          end_reason = AttachEndReason::ProtocolError;
          break;
        }
        continue;
      }

      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::MouseEvent) {
      const auto mouse = parse_attach_mouse_event_payload(frame.payload);
      if (!mouse) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      if (mouse->action == AttachMouseAction::Wheel) {
        const auto scroll =
            mouse->button == AttachMouseButton::WheelUp
                ? AttachScrollAction::LineUp
                : mouse->button == AttachMouseButton::WheelDown
                      ? AttachScrollAction::LineDown
                      : AttachScrollAction::Bottom;
        if (scroll == AttachScrollAction::Bottom) {
          continue;
        }

        if (!render_scene_delta(error, scroll, std::nullopt)) {
          end_reason = AttachEndReason::OutputClosed;
          break;
        }
        continue;
      }

      const bool reserved_status_row = reserve_status_row();
      const auto mouse_result = events.call_event(DaemonEvent::attach_mouse_event(
          client_id,
          session_id,
          *mouse,
          static_cast<std::uint16_t>(current_columns.load(std::memory_order_relaxed)),
          static_cast<std::uint16_t>(current_rows.load(std::memory_order_relaxed)),
          reserved_status_row));
      if (!mouse_result.changed) {
        if (!mouse_result.ok && !mouse_result.error.empty()) {
          end_reason = AttachEndReason::ProtocolError;
          break;
        }
        continue;
      }
      if (mouse->action == AttachMouseAction::Drag) {
        {
          std::lock_guard stream_lock(stream_mutex);
          status_mode = StatusLineMode::MouseDrag;
          status_set_temporary(
              status_state,
              "wmux: pane resized",
              std::chrono::steady_clock::now(),
              kPrefixStatusMessageTtl);
        }
      } else if (mouse->action == AttachMouseAction::Press) {
        set_temporary_status("wmux: pane selected");
      }

      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Scroll) {
      const auto scroll = parse_attach_scroll_payload(frame.payload);
      if (!scroll) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      if (!render_scene_delta(error, *scroll, std::nullopt)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::CopyMode) {
      const auto action = parse_attach_copy_mode_payload(frame.payload);
      if (!action) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      std::string error;
      if (!render_scene_delta(error, std::nullopt, *action)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Status) {
      set_status_frame(frame.payload);

      std::string error;
      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::CommandMode) {
      const auto current_settings = settings_snapshot();
      auto command_result = events.call_event(DaemonEvent::command_mode_command(
          client_id,
          session_id,
          session_name,
          frame.payload,
          static_cast<std::uint16_t>(current_columns.load(std::memory_order_relaxed)),
          static_cast<std::uint16_t>(current_rows.load(std::memory_order_relaxed)),
          current_settings.status_bar_enabled));
      if (!command_result.session_name.empty()) {
        session_name = command_result.session_name;
      }
      set_temporary_status(command_result.status);

      std::string error;
      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    if (frame.type == AttachFrameType::Paste) {
      if (!frame.payload.empty()) {
        end_reason = AttachEndReason::ProtocolError;
        break;
      }

      auto paste_result = events.call_event(DaemonEvent::paste(client_id, session_id));
      if (!paste_result.ok) {
        end_reason = AttachEndReason::ShellClosed;
        break;
      }

      if (!paste_result.shell) {
        set_temporary_status(paste_result.status);
        std::string error;
        if (!layout_active_window(error)) {
          end_reason = AttachEndReason::OutputClosed;
          break;
        }
        continue;
      }

      {
        std::lock_guard stream_lock(stream_mutex);
        viewport_states[paste_result.pane_id].offset = 0;
        status_set_temporary(
            status_state,
            paste_result.status,
            std::chrono::steady_clock::now());
        status_mode = StatusLineMode::Normal;
      }

      if (!paste_result.text.empty()) {
        start_background_worker(
            [shell = paste_result.shell,
             text = std::move(paste_result.text),
             client_id,
             session_id,
             pane_id = paste_result.pane_id,
             set_background_status] {
              const bool ok = shell->write_input_throttled(
                  text,
                  kMaxPasteWriteChunkBytes,
                  std::chrono::milliseconds{kPasteWriteChunkDelayMs});
              if (ok) {
                if (!should_log(LogLevel::Debug)) {
                  return;
                }
                log_event(
                    LogLevel::Debug,
                    "daemon.attach",
                    "paste_write_ok",
                    {{"client_id", std::to_string(client_id)},
                     {"session_id", std::to_string(session_id)},
                     {"pane_id", std::to_string(pane_id)},
                     {"bytes", std::to_string(text.size())}});
                return;
              }

              log_event(
                  LogLevel::Warn,
                  "daemon.attach",
                  "paste_write_failed",
                  {{"client_id", std::to_string(client_id)},
                   {"session_id", std::to_string(session_id)},
                   {"pane_id", std::to_string(pane_id)},
                  {"bytes", std::to_string(text.size())}});
              set_background_status("wmux: paste failed; active shell closed");
            });
      }

      std::string error;
      if (!layout_active_window(error)) {
        end_reason = AttachEndReason::OutputClosed;
        break;
      }
      continue;
    }

    std::vector<std::uint8_t> input_bytes(frame.payload.begin(), frame.payload.end());
    auto input_result =
        events.call_event(DaemonEvent::client_input(client_id, session_id, std::move(input_bytes)));
    if (!input_result.ok || !input_result.shell) {
      end_reason = AttachEndReason::ShellClosed;
      break;
    }

    {
      std::lock_guard stream_lock(stream_mutex);
      viewport_states[input_result.pane_id].offset = 0;
    }

    if (!input_result.shell->write_input(frame.payload)) {
      end_reason = AttachEndReason::ShellClosed;
      break;
    }
  }

  stop_requested = true;
  if (output_thread.joinable()) {
    output_thread.join();
  }
  stop_writer(false);
  join_background_workers();

  if (output_closed && end_reason == AttachEndReason::ClientDisconnected) {
    end_reason = AttachEndReason::OutputClosed;
  }

  close_attach_pipe(pipe);
  (void)events.call_event(DaemonEvent::client_disconnected(client_id, end_reason));
}

}  // namespace

std::wstring widen(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int required =
      MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
  return wide;
}

bool write_all(HANDLE pipe, std::string_view bytes) {
  const auto start = std::chrono::steady_clock::now();
  while (!bytes.empty()) {
    if (std::chrono::steady_clock::now() - start > kSlowClientWriteTimeout) {
      log_event(
          LogLevel::Warn,
          "daemon.attach",
          "write_timeout",
          {{"remaining_bytes", std::to_string(bytes.size())}});
      return false;
    }

    const auto bytes_to_write =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
    DWORD bytes_written = 0;
    const BOOL ok = WriteFile(pipe, bytes.data(), bytes_to_write, &bytes_written, nullptr);
    if (!ok || bytes_written == 0) {
      return false;
    }

    bytes.remove_prefix(bytes_written);
  }

  return true;
}

std::wstring current_user_sid_string() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes > 0) {
      std::vector<unsigned char> buffer(bytes);
      if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        LPWSTR sid_text = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
          std::wstring sid{sid_text};
          LocalFree(sid_text);
          CloseHandle(token);
          return sid;
        }
      }
    }
    CloseHandle(token);
  }
  return {};
}

class PipeSecurityAttributes {
 public:
  PipeSecurityAttributes() {
    const auto sid = current_user_sid_string();
    if (sid.empty()) {
      return;
    }

    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &descriptor_,
            nullptr)) {
      log_event(
          LogLevel::Warn,
          "daemon.ipc",
          "pipe_acl_create_failed",
          {{"win32_error", std::to_string(GetLastError())}});
      return;
    }

    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
  }

  PipeSecurityAttributes(const PipeSecurityAttributes&) = delete;
  PipeSecurityAttributes& operator=(const PipeSecurityAttributes&) = delete;

  ~PipeSecurityAttributes() {
    if (descriptor_ != nullptr) {
      LocalFree(descriptor_);
    }
  }

  SECURITY_ATTRIBUTES* get() {
    return descriptor_ != nullptr ? &attributes_ : nullptr;
  }

 private:
  PSECURITY_DESCRIPTOR descriptor_{nullptr};
  SECURITY_ATTRIBUTES attributes_{};
};

void close_pipe(HANDLE pipe) {
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

void close_attach_pipe(HANDLE pipe) {
  CancelIoEx(pipe, nullptr);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

HANDLE create_server_pipe(const std::wstring& endpoint, std::uint32_t open_mode_flags) {
  PipeSecurityAttributes security;
  return CreateNamedPipeW(
      endpoint.c_str(),
      PIPE_ACCESS_DUPLEX | static_cast<DWORD>(open_mode_flags),
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      4096,
      4096,
      0,
      security.get());
}

bool connect_named_pipe(HANDLE pipe) {
  return ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
}

bool connect_named_pipe_overlapped(HANDLE pipe, const std::atomic_bool& should_stop) {
  LocalHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.valid()) {
    return false;
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  if (ConnectNamedPipe(pipe, &overlapped)) {
    return true;
  }

  DWORD error = GetLastError();
  if (error == ERROR_PIPE_CONNECTED) {
    SetEvent(event.get());
    return true;
  }
  if (error != ERROR_IO_PENDING) {
    return false;
  }

  while (!should_stop.load(std::memory_order_relaxed)) {
    const DWORD wait_result = WaitForSingleObject(event.get(), 50);
    if (wait_result == WAIT_TIMEOUT) {
      continue;
    }
    if (wait_result != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &overlapped);
      return false;
    }

    DWORD transferred = 0;
    if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
      return true;
    }

    error = GetLastError();
    return error == ERROR_PIPE_CONNECTED;
  }

  CancelIoEx(pipe, &overlapped);
  return false;
}

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id) {
  disconnect_attach_pipes(attach_client_pipes_for_session(state, session_id));
}

void disconnect_all_attach_clients(DaemonState& state) {
  disconnect_attach_pipes(all_attach_client_pipes(state));
}

std::optional<DaemonEventResult> handle_attach_daemon_event(
    DaemonState& state,
    const DaemonEvent& event) {
  assert_daemon_state_mutation_allowed("handle_attach_daemon_event");

  DaemonEventResult result;
  result.handled = true;

  const auto require_client = [&]() -> bool {
    if (!event.client_id) {
      result.ok = false;
      result.error = "wmux: stale attach event without client id\n";
      return false;
    }

    std::lock_guard lock(state.mutex);
    if (state.attach_clients.find(*event.client_id) == state.attach_clients.end()) {
      result.ok = false;
      result.error = "wmux: stale attach client\n";
      if (should_log(LogLevel::Debug)) {
        log_event(
            LogLevel::Debug,
            "daemon.attach",
            "stale_event",
            {{"event", std::string{daemon_event_kind_name(event.kind)}},
             {"client_id", std::to_string(*event.client_id)}});
      }
      return false;
    }

    return true;
  };

  switch (event.kind) {
    case DaemonEventKind::AttachStart: {
      std::string error;
      auto target = target_for_attach(state, event.command, error);
      if (!target) {
        result.ok = false;
        result.error = error;
        result.has_response = true;
        result.response = make_response_json(false, error);
        return result;
      }

#ifdef _WIN32
      const ClientId client_id =
          register_attach_client(
              state,
              target->session_id,
              target->session_name,
              event.pipe,
              event.columns,
              event.rows,
              event.command.terminal_capabilities,
              event.command.terminal_capabilities_provided);
#else
      const ClientId client_id = 0;
#endif
      result.client_id = client_id;
      result.session_id = target->session_id;
      result.session_name = target->session_name;
      result.attach_settings = attach_settings_for_client(state, client_id);
      {
        std::lock_guard lock(state.mutex);
        const auto client = state.attach_clients.find(client_id);
        if (client != state.attach_clients.end()) {
          const bool supports_sgr_mouse = client->second.client.terminal_caps.supports_sgr_mouse;
          if (!supports_sgr_mouse) {
            if (should_log(LogLevel::Debug)) {
              log_event(
                  LogLevel::Debug,
                  "daemon.attach",
                  "mouse_capability_disabled",
                  {{"client_id", std::to_string(client_id)},
                   {"session_id", std::to_string(target->session_id)}});
            }
          }
        }
      }
      log_event(
          LogLevel::Info,
          "daemon.attach",
          "lifecycle",
          {{"event", "AttachStart"},
           {"client_id", std::to_string(client_id)},
           {"session_id", std::to_string(target->session_id)},
           {"session_name", target->session_name}});
      return result;
    }

    case DaemonEventKind::ClientDisconnected:
      if (!event.client_id) {
        result.ok = false;
        result.error = "wmux: stale attach disconnect without client id\n";
        return result;
      }
      unregister_attach_client(state, *event.client_id, event.attach_end_reason);
      return result;

    case DaemonEventKind::AttachCommand: {
      if (!require_client()) {
        return result;
      }

      log_event(
          LogLevel::Info,
          "daemon.attach",
          "command_received",
          {{"client_id", std::to_string(*event.client_id)},
           {"session_id", std::to_string(event.session_id)},
           {"command", event.text}});
      result.ok = execute_attach_command(
          state,
          *event.client_id,
          event.session_id,
          event.text,
          bounded_attach_columns(event.columns),
          bounded_attach_rows(event.rows),
          event.status_bar_enabled,
          result.status,
          result.error);
      result.changed = result.ok;
      log_event(
          result.ok ? LogLevel::Info : LogLevel::Warn,
          "daemon.attach",
          "command_completed",
          {{"client_id", std::to_string(*event.client_id)},
           {"session_id", std::to_string(event.session_id)},
           {"command", event.text},
           {"ok", result.ok ? "true" : "false"},
           {"message", result.error}});
      return result;
    }

    case DaemonEventKind::CommandModeCommand: {
      if (!require_client()) {
        return result;
      }

      std::string session_name = event.session_name;
      result.ok = execute_command_mode_command(
          state,
          *event.client_id,
          event.session_id,
          session_name,
          bounded_attach_columns(event.columns),
          bounded_attach_rows(event.rows),
          event.status_bar_enabled,
          event.text,
          result.status);
      result.session_name = std::move(session_name);
      result.changed = true;
      return result;
    }

    case DaemonEventKind::MouseFocus: {
      if (!require_client()) {
        return result;
      }
      if (!daemon_mouse_setting_enabled(state)) {
        if (should_log(LogLevel::Debug)) {
          log_event(
              LogLevel::Debug,
              "daemon.mouse",
              "ignored_disabled",
              {{"client_id", std::to_string(*event.client_id)}, {"event", "focus"}});
        }
        return result;
      }
      if (should_log(LogLevel::Debug)) {
        log_event(
            LogLevel::Debug,
            "daemon.mouse",
            "focus",
            {{"client_id", std::to_string(*event.client_id)},
             {"column", std::to_string(event.focus.column)},
             {"row", std::to_string(event.focus.row)}});
      }

      const auto command =
          select_pane_at_mouse_command(*event.client_id, event.focus.column, event.focus.row);
      const auto command_result = execute_runtime_attach_command(
          state,
          *event.client_id,
          command,
          bounded_attach_columns(event.columns),
          bounded_attach_rows(event.rows),
          event.reserve_status_row,
          nullptr);
      if (command_result.status == CommandStatus::Success) {
        result.changed = true;
        return result;
      }
      if (command_result.status == CommandStatus::NoOp) {
        return result;
      }
      result.ok = false;
      result.error = command_result.message.value_or("wmux: mouse focus failed");
      return result;
    }

    case DaemonEventKind::MouseEvent: {
      if (!require_client()) {
        return result;
      }
      if (!daemon_mouse_setting_enabled(state)) {
        if (should_log(LogLevel::Debug)) {
          log_event(
              LogLevel::Debug,
              "daemon.mouse",
              "ignored_disabled",
              {{"client_id", std::to_string(*event.client_id)}, {"event", "mouse"}});
        }
        return result;
      }
      if (should_log(LogLevel::Debug)) {
        log_event(
            LogLevel::Debug,
            "daemon.mouse",
            "event",
            {{"client_id", std::to_string(*event.client_id)},
             {"column", std::to_string(event.attach_mouse.column)},
             {"row", std::to_string(event.attach_mouse.row)},
             {"button_code", std::to_string(event.attach_mouse.button_code)},
             {"action", std::to_string(static_cast<int>(event.attach_mouse.action))}});
      }

      MouseDragState drag;
      {
        std::lock_guard lock(state.mutex);
        const auto client = state.attach_clients.find(*event.client_id);
        if (client == state.attach_clients.end()) {
          result.ok = false;
          result.error = "wmux: stale attach client\n";
          return result;
        }
        drag.active = client->second.client.mouse_drag_active;
        drag.target = client->second.client.mouse_drag_target;
      }

      const auto changed = handle_interactive_mouse_event(
          state,
          *event.client_id,
          event.attach_mouse,
          bounded_attach_columns(event.columns),
          bounded_attach_rows(event.rows),
          event.reserve_status_row,
          drag,
          result.error);

      {
        std::lock_guard lock(state.mutex);
        const auto client = state.attach_clients.find(*event.client_id);
        if (client != state.attach_clients.end()) {
          client->second.client.mouse_drag_active = drag.active;
          client->second.client.mouse_drag_target = drag.target;
        }
      }

      if (!changed) {
        result.ok = result.error.empty();
        return result;
      }

      result.changed = *changed;
      return result;
    }

    case DaemonEventKind::Paste: {
      if (!require_client()) {
        return result;
      }

      PasteBuffer paste_buffer;
      {
        std::lock_guard lock(state.mutex);
        paste_buffer = state.paste_buffer;
      }

      if (paste_buffer.text.empty()) {
        result.status = "wmux: paste buffer empty";
        return result;
      }

      auto shell = active_shell_for_session(state, event.session_id, result.error);
      if (!shell) {
        result.ok = false;
        return result;
      }

      result.shell = shell->shell;
      result.pane_id = shell->pane_id;
      const auto snapshot = result.shell->output_snapshot(PtyOutputSnapshotMode::ScreenOnly);
      result.text =
          prepare_paste_text_for_terminal(paste_buffer.text, snapshot.screen.bracketed_paste_mode);
      result.status = "wmux: paste queued " + std::to_string(paste_buffer.text.size()) + " bytes";
      if (paste_buffer.truncated) {
        result.status += " (truncated from " + std::to_string(paste_buffer.original_bytes) + ")";
      }
      if (snapshot.screen.bracketed_paste_mode) {
        result.status += "; bracketed";
      }
      result.changed = true;
      return result;
    }

    case DaemonEventKind::ClientInput: {
      if (!require_client()) {
        return result;
      }

      auto shell = active_shell_for_session(state, event.session_id, result.error);
      if (!shell) {
        result.ok = false;
        return result;
      }

      result.shell = shell->shell;
      result.pane_id = shell->pane_id;
      result.text.assign(event.bytes.begin(), event.bytes.end());
      result.changed = true;
      return result;
    }

    default:
      return std::nullopt;
  }
}

bool wait_for_no_attach_clients(DaemonState& state, std::chrono::milliseconds timeout) {
  std::unique_lock lock(state.mutex);
  return state.attach_clients_changed.wait_for(
      lock, timeout, [&] { return state.attach_clients.empty(); });
}

void reap_finished_attach_workers(DaemonState& state) {
  auto workers = take_finished_attach_workers(state);
  join_workers(workers, "worker_join");
}

void join_all_attach_workers(DaemonState& state) {
  std::vector<std::pair<ClientId, std::thread>> workers;
  {
    std::unique_lock lock(state.mutex);
    state.attach_workers_changed.wait(lock, [&] {
      return std::ranges::all_of(state.attach_workers, [](const auto& worker) {
        return worker.done && worker.done->load(std::memory_order_acquire);
      });
    });

    workers.reserve(state.attach_workers.size());
    for (auto& worker : state.attach_workers) {
      workers.emplace_back(worker.client_id, std::move(worker.thread));
    }
    state.attach_workers.clear();
  }

  join_workers(workers, "worker_join_shutdown");
}

AttachDispatch dispatch_attach_connection(
    HANDLE pipe,
    const IpcRequest& request,
    RequestId request_id,
    DaemonEventLoop& events) {
  if (!is_attach_start_request(request.type)) {
    return AttachDispatch::NotAttach;
  }

  auto attach_start = events.call_event(DaemonEvent::attach_start(pipe, request));
  if (!attach_start.ok || !attach_start.client_id) {
    write_ipc_response_frame(
        pipe,
        IpcFrameKind::Error,
        request_id,
        attach_start.has_response
            ? attach_start.response
            : make_response_json(
                  false,
                  attach_start.error.empty() ? "wmux: attach failed\n" : attach_start.error));
    return AttachDispatch::Completed;
  }

  const short columns = bounded_attach_columns(request.terminal_columns);
  const short rows = bounded_attach_rows(request.terminal_rows);
  start_attach_worker(
      events,
      pipe,
      *attach_start.client_id,
      attach_start.session_id,
      std::move(attach_start.session_name),
      request_id,
      columns,
      rows,
      attach_start.attach_settings);
  return AttachDispatch::HandedOff;
}

void run_windows_attach_listener(DaemonEventLoop& events, std::atomic_bool& should_stop) {
  const auto endpoint = widen(attach_endpoint_name());

  while (!should_stop.load()) {
    events.call([](DaemonState& state) { reap_finished_attach_workers(state); });

    HANDLE pipe = create_server_pipe(endpoint, FILE_FLAG_OVERLAPPED);
    if (pipe == INVALID_HANDLE_VALUE) {
      log_event(
          LogLevel::Error,
          "daemon.attach",
          "pipe_create_failed",
          {{"win32_error", std::to_string(GetLastError())}});
      return;
    }

    if (!connect_named_pipe_overlapped(pipe, should_stop)) {
      if (should_stop.load(std::memory_order_relaxed)) {
        CloseHandle(pipe);
        break;
      }
      log_event(
          LogLevel::Warn,
          "daemon.attach",
          "connect_failed",
          {{"win32_error", std::to_string(GetLastError())}});
      CloseHandle(pipe);
      continue;
    }

    if (should_stop.load()) {
      close_attach_pipe(pipe);
      break;
    }

    IpcFrameParseResult request_frame;
    if (read_attach_start_frame(pipe, request_frame)) {
      const RequestId request_id = request_frame.header.request_id;
      if (!request_frame.ok) {
        log_event(
            LogLevel::Warn,
            "daemon.attach",
            "malformed_ipc_frame",
            {{"request_id", std::to_string(request_id)},
             {"error", std::string{ipc_frame_error_name(request_frame.error)}}});
        write_ipc_response_frame(
            pipe,
            IpcFrameKind::Error,
            request_id,
            make_response_json(false, request_frame.message + "\n"));
      } else if (request_frame.header.kind != IpcFrameKind::Control) {
        log_event(
            LogLevel::Warn,
            "daemon.attach",
            "wrong_frame_kind",
            {{"request_id", std::to_string(request_id)},
             {"kind", std::string{ipc_frame_kind_name(request_frame.header.kind)}}});
        write_ipc_response_frame(
            pipe,
            IpcFrameKind::Error,
            request_id,
            make_response_json(false, "wmux: attach start must use a control frame\n"));
      } else {
        const auto parsed = parse_request_json(request_frame.payload);
        if (!parsed) {
          log_event(
              LogLevel::Warn,
              "daemon.attach",
              "malformed_request",
              {{"request_id", std::to_string(request_id)}});
          write_ipc_response_frame(
              pipe,
              IpcFrameKind::Error,
              request_id,
              make_response_json(false, "wmux: malformed attach request\n"));
        } else {
          const auto attach_dispatch =
              dispatch_attach_connection(pipe, *parsed, request_id, events);
          if (attach_dispatch == AttachDispatch::HandedOff) {
            pipe = INVALID_HANDLE_VALUE;
            continue;
          }

          if (attach_dispatch == AttachDispatch::NotAttach) {
            log_event(
                LogLevel::Warn,
                "daemon.attach",
                "wrong_endpoint",
                {{"request_id", std::to_string(request_id)}, {"type", parsed->type}});
            write_ipc_response_frame(
                pipe,
                IpcFrameKind::Error,
                request_id,
                make_response_json(
                    false,
                    "wmux: attach endpoint accepts only attach requests\n"));
          }
        }
      }
    } else {
      log_event(LogLevel::Warn, "daemon.attach", "request_read_failed");
    }

    if (pipe != INVALID_HANDLE_VALUE) {
      close_attach_pipe(pipe);
    }
  }

  events.call([](DaemonState& state) { reap_finished_attach_workers(state); });
}

void wake_attach_listener() {
  const auto endpoint = widen(attach_endpoint_name());
  HANDLE pipe = CreateFileW(
      endpoint.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr);
  if (pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe);
  }
}

#else

void disconnect_attach_clients_for_session(DaemonState& state, SessionId session_id) {
  (void)state;
  (void)session_id;
}

void disconnect_all_attach_clients(DaemonState& state) {
  (void)state;
}

#endif

}  // namespace wmux::daemon_internal
