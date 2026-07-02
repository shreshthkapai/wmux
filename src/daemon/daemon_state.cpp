#include "daemon_state.hpp"

#include "wmux/attach_keymap.hpp"
#include "wmux/logging.hpp"

#include <filesystem>
#include <stdexcept>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace wmux::daemon_internal {
namespace {

thread_local int g_daemon_state_mutation_depth = 0;

class DaemonStateMutationScope {
 public:
  DaemonStateMutationScope() {
    ++g_daemon_state_mutation_depth;
  }

  DaemonStateMutationScope(const DaemonStateMutationScope&) = delete;
  DaemonStateMutationScope& operator=(const DaemonStateMutationScope&) = delete;

  ~DaemonStateMutationScope() {
    --g_daemon_state_mutation_depth;
  }
};

bool has_pane_runtime_locked(const DaemonState& state, PaneId pane_id) {
  for (const auto& [session_id, session] : state.runtimes) {
    (void)session_id;
    for (const auto& [window_id, window] : session.windows) {
      (void)window_id;
      if (window.panes.find(pane_id) != window.panes.end()) {
        return true;
      }
    }
  }

  return false;
}

std::deque<DiagnosticEvent>& diagnostic_ring_for_category(
    DaemonDiagnosticRings& rings,
    DiagnosticEventCategory category) {
  switch (category) {
    case DiagnosticEventCategory::Command:
      return rings.commands;
    case DiagnosticEventCategory::Key:
      return rings.keys;
    case DiagnosticEventCategory::Process:
      return rings.processes;
    case DiagnosticEventCategory::ResizeLayout:
      return rings.resize_layout;
    case DiagnosticEventCategory::Error:
      return rings.errors;
  }

  return rings.errors;
}

const std::deque<DiagnosticEvent>& diagnostic_ring_for_category(
    const DaemonDiagnosticRings& rings,
    DiagnosticEventCategory category) {
  switch (category) {
    case DiagnosticEventCategory::Command:
      return rings.commands;
    case DiagnosticEventCategory::Key:
      return rings.keys;
    case DiagnosticEventCategory::Process:
      return rings.processes;
    case DiagnosticEventCategory::ResizeLayout:
      return rings.resize_layout;
    case DiagnosticEventCategory::Error:
      return rings.errors;
  }

  return rings.errors;
}

void append_diagnostic_event_to_ring(
    std::deque<DiagnosticEvent>& ring,
    const DiagnosticEvent& event) {
  ring.push_back(event);
  while (ring.size() > kDiagnosticEventRingCapacity) {
    ring.pop_front();
  }
}

void update_atomic_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value) {
  auto current_peak = peak.load(std::memory_order_relaxed);
  while (value > current_peak &&
         !peak.compare_exchange_weak(
             current_peak,
             value,
             std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

std::string attach_end_reason_name(AttachEndReason reason) {
  switch (reason) {
    case AttachEndReason::Detached:
      return "detached";
    case AttachEndReason::ClientDisconnected:
      return "client-disconnected";
    case AttachEndReason::ShellClosed:
      return "shell-closed";
    case AttachEndReason::OutputClosed:
      return "output-closed";
    case AttachEndReason::ProtocolError:
      return "protocol-error";
  }

  return "unknown";
}

void log_stale_event(const DaemonEvent& event, std::string_view target) {
  log_event(
      LogLevel::Debug,
      "daemon.event",
      "stale_drop",
      {{"event", std::string{daemon_event_kind_name(event.kind)}},
       {"target", std::string{target}},
       {"client_id", event.client_id ? std::to_string(*event.client_id) : ""},
       {"pane_id", event.pane_id == 0 ? "" : std::to_string(event.pane_id)}});
}

}  // namespace

std::string_view diagnostic_event_category_name(DiagnosticEventCategory category) {
  switch (category) {
    case DiagnosticEventCategory::Command:
      return "command";
    case DiagnosticEventCategory::Key:
      return "key";
    case DiagnosticEventCategory::Process:
      return "process";
    case DiagnosticEventCategory::ResizeLayout:
      return "resize-layout";
    case DiagnosticEventCategory::Error:
      return "error";
  }

  return "unknown";
}

std::vector<DiagnosticEvent> diagnostic_events_snapshot(
    const DaemonDiagnosticRings& rings,
    DiagnosticEventCategory category) {
  const auto& ring = diagnostic_ring_for_category(rings, category);
  return {ring.begin(), ring.end()};
}

void record_diagnostic_event_locked(DaemonState& state, DiagnosticEvent event) {
  assert_daemon_state_mutation_allowed("record_diagnostic_event_locked");
  if (event.sequence == 0) {
    event.sequence = state.diagnostics.next_sequence++;
  }
  if (event.timestamp == std::chrono::system_clock::time_point{}) {
    event.timestamp = std::chrono::system_clock::now();
  }

  const auto category = event.category;
  append_diagnostic_event_to_ring(diagnostic_ring_for_category(state.diagnostics, category), event);
  if (category != DiagnosticEventCategory::Error &&
      (event.level == "warn" || event.level == "error")) {
    DiagnosticEvent error_event = event;
    error_event.category = DiagnosticEventCategory::Error;
    append_diagnostic_event_to_ring(state.diagnostics.errors, error_event);
  }
}

void record_diagnostic_event(DaemonState& state, DiagnosticEvent event) {
  assert_daemon_state_mutation_allowed("record_diagnostic_event");
  std::lock_guard lock(state.mutex);
  record_diagnostic_event_locked(state, std::move(event));
}

DaemonEvent DaemonEvent::attach_start(
    PlatformPipeHandle pipe_value,
    IpcRequest command_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::AttachStart;
  event.pipe = pipe_value;
  event.command = std::move(command_value);
  event.columns = event.command.terminal_columns;
  event.rows = event.command.terminal_rows;
  return event;
}

DaemonEvent DaemonEvent::client_connected(ClientId client_id_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::ClientConnected;
  event.client_id = client_id_value;
  return event;
}

DaemonEvent DaemonEvent::client_disconnected(
    ClientId client_id_value,
    AttachEndReason reason) {
  DaemonEvent event;
  event.kind = DaemonEventKind::ClientDisconnected;
  event.client_id = client_id_value;
  event.attach_end_reason = reason;
  return event;
}

DaemonEvent DaemonEvent::client_input(
    ClientId client_id_value,
    SessionId session_id_value,
    std::vector<std::uint8_t> input_bytes) {
  DaemonEvent event;
  event.kind = DaemonEventKind::ClientInput;
  event.client_id = client_id_value;
  event.session_id = session_id_value;
  event.bytes = std::move(input_bytes);
  return event;
}

DaemonEvent DaemonEvent::decoded_key(ClientId client_id_value, DaemonKeyEvent key_event) {
  DaemonEvent event;
  event.kind = DaemonEventKind::DecodedKey;
  event.client_id = client_id_value;
  event.key = std::move(key_event);
  return event;
}

DaemonEvent DaemonEvent::mouse_event(ClientId client_id_value, MouseEvent mouse_event_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::MouseEvent;
  event.client_id = client_id_value;
  event.mouse = mouse_event_value;
  return event;
}

DaemonEvent DaemonEvent::attach_mouse_event(
    ClientId client_id_value,
    SessionId session_id_value,
    AttachMouseEventPayload mouse_event_value,
    std::uint16_t columns_value,
    std::uint16_t rows_value,
    bool reserve_status_row_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::MouseEvent;
  event.client_id = client_id_value;
  event.session_id = session_id_value;
  event.attach_mouse = mouse_event_value;
  event.columns = columns_value;
  event.rows = rows_value;
  event.reserve_status_row = reserve_status_row_value;
  return event;
}

DaemonEvent DaemonEvent::mouse_focus(
    ClientId client_id_value,
    SessionId session_id_value,
    AttachMouseFocusPayload focus_value,
    std::uint16_t columns_value,
    std::uint16_t rows_value,
    bool reserve_status_row_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::MouseFocus;
  event.client_id = client_id_value;
  event.session_id = session_id_value;
  event.focus = focus_value;
  event.columns = columns_value;
  event.rows = rows_value;
  event.reserve_status_row = reserve_status_row_value;
  return event;
}

DaemonEvent DaemonEvent::attach_command(
    ClientId client_id_value,
    SessionId session_id_value,
    std::string command_value,
    std::uint16_t columns_value,
    std::uint16_t rows_value,
    bool status_bar_enabled_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::AttachCommand;
  event.client_id = client_id_value;
  event.session_id = session_id_value;
  event.text = std::move(command_value);
  event.columns = columns_value;
  event.rows = rows_value;
  event.status_bar_enabled = status_bar_enabled_value;
  return event;
}

DaemonEvent DaemonEvent::command_mode_command(
    ClientId client_id_value,
    SessionId session_id_value,
    std::string session_name_value,
    std::string command_value,
    std::uint16_t columns_value,
    std::uint16_t rows_value,
    bool status_bar_enabled_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::CommandModeCommand;
  event.client_id = client_id_value;
  event.session_id = session_id_value;
  event.session_name = std::move(session_name_value);
  event.text = std::move(command_value);
  event.columns = columns_value;
  event.rows = rows_value;
  event.status_bar_enabled = status_bar_enabled_value;
  return event;
}

DaemonEvent DaemonEvent::ipc_command(
    std::optional<ClientId> client_id_value,
    RequestId request_id_value,
    IpcRequest command) {
  DaemonEvent event;
  event.kind = DaemonEventKind::IpcCommand;
  event.client_id = client_id_value;
  event.request_id = request_id_value;
  event.command = std::move(command);
  return event;
}

DaemonEvent DaemonEvent::pane_output(PaneId pane_id_value, std::vector<std::uint8_t> output_bytes) {
  DaemonEvent event;
  event.kind = DaemonEventKind::PaneOutput;
  event.pane_id = pane_id_value;
  event.bytes = std::move(output_bytes);
  return event;
}

DaemonEvent DaemonEvent::pane_exited(PaneId pane_id_value, std::int64_t exit_code_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::PaneExited;
  event.pane_id = pane_id_value;
  event.exit_code = exit_code_value;
  return event;
}

DaemonEvent DaemonEvent::client_resize(
    ClientId client_id_value,
    std::uint16_t columns_value,
    std::uint16_t rows_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::ClientResize;
  event.client_id = client_id_value;
  event.columns = columns_value;
  event.rows = rows_value;
  return event;
}

DaemonEvent DaemonEvent::paste(ClientId client_id_value, SessionId session_id_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::Paste;
  event.client_id = client_id_value;
  event.session_id = session_id_value;
  return event;
}

DaemonEvent DaemonEvent::set_paste_buffer(
    std::string text_value,
    PasteBufferSource source_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::PasteBufferSet;
  event.text = std::move(text_value);
  event.paste_source = source_value;
  return event;
}

DaemonEvent DaemonEvent::timer_event(DaemonTimerEvent timer_value) {
  DaemonEvent event;
  event.kind = DaemonEventKind::Timer;
  event.timer = timer_value;
  return event;
}

DaemonEvent DaemonEvent::shutdown() {
  DaemonEvent event;
  event.kind = DaemonEventKind::Shutdown;
  return event;
}

std::string_view daemon_event_kind_name(DaemonEventKind kind) {
  switch (kind) {
    case DaemonEventKind::AttachStart:
      return "AttachStart";
    case DaemonEventKind::ClientConnected:
      return "ClientConnected";
    case DaemonEventKind::ClientDisconnected:
      return "ClientDisconnected";
    case DaemonEventKind::ClientInput:
      return "ClientInput";
    case DaemonEventKind::DecodedKey:
      return "DecodedKey";
    case DaemonEventKind::MouseEvent:
      return "MouseEvent";
    case DaemonEventKind::MouseFocus:
      return "MouseFocus";
    case DaemonEventKind::AttachCommand:
      return "AttachCommand";
    case DaemonEventKind::CommandModeCommand:
      return "CommandModeCommand";
    case DaemonEventKind::Paste:
      return "Paste";
    case DaemonEventKind::PasteBufferSet:
      return "PasteBufferSet";
    case DaemonEventKind::IpcCommand:
      return "IpcCommand";
    case DaemonEventKind::PaneOutput:
      return "PaneOutput";
    case DaemonEventKind::PaneExited:
      return "PaneExited";
    case DaemonEventKind::ClientResize:
      return "ClientResize";
    case DaemonEventKind::Timer:
      return "Timer";
    case DaemonEventKind::Shutdown:
      return "Shutdown";
  }

  return "Unknown";
}

bool daemon_state_mutation_allowed() {
  return g_daemon_state_mutation_depth > 0;
}

void assert_daemon_state_mutation_allowed(std::string_view source) {
#ifndef NDEBUG
  if (!daemon_state_mutation_allowed()) {
    log_event(
        LogLevel::Error,
        "daemon.event",
        "wrong_thread_mutation",
        {{"source", std::string{source}}});
    throw std::logic_error{
        "wmux daemon state mutation outside event loop: " + std::string{source}};
  }
#else
  (void)source;
#endif
}

std::string quoted(std::string_view value) {
  std::ostringstream out;
  out << "'" << value << "'";
  return out.str();
}

std::string session_error_message(SessionError error, std::string_view name) {
  std::ostringstream out;

  switch (error) {
    case SessionError::EmptyName:
      out << "wmux: session name cannot be empty\n";
      break;
    case SessionError::DuplicateName:
      out << "wmux: session " << quoted(name) << " already exists\n";
      break;
    case SessionError::NotFound:
      out << "wmux: session " << quoted(name) << " not found\n";
      break;
    case SessionError::None:
      out << "wmux: session operation failed\n";
      break;
  }

  return out.str();
}

std::string window_error_message(WindowError error, std::string_view name) {
  std::ostringstream out;

  switch (error) {
    case WindowError::EmptyName:
      out << "wmux: window name cannot be empty\n";
      break;
    case WindowError::DuplicateName:
      out << "wmux: window " << quoted(name) << " already exists\n";
      break;
    case WindowError::SessionNotFound:
      out << "wmux: target session not found\n";
      break;
    case WindowError::WindowNotFound:
      out << "wmux: active window not found\n";
      break;
    case WindowError::LastWindow:
      out << "wmux: cannot kill the last window in a session\n";
      break;
    case WindowError::None:
      out << "wmux: window operation failed\n";
      break;
  }

  return out.str();
}

std::string pane_error_message(PaneError error) {
  std::ostringstream out;

  switch (error) {
    case PaneError::SessionNotFound:
      out << "wmux: target session not found\n";
      break;
    case PaneError::WindowNotFound:
      out << "wmux: active window not found\n";
      break;
    case PaneError::PaneNotFound:
      out << "wmux: active pane not found\n";
      break;
    case PaneError::LastPane:
      out << "wmux: cannot kill the last pane in a window\n";
      break;
    case PaneError::NoSplit:
      out << "wmux: active window has no pane splits\n";
      break;
    case PaneError::None:
      out << "wmux: pane operation failed\n";
      break;
  }

  return out.str();
}

DaemonEventLoop::DaemonEventLoop(DaemonState& state) : state_(state) {}

DaemonEventLoop::~DaemonEventLoop() {
  stop();
}

void DaemonEventLoop::start() {
  std::lock_guard lock(queue_mutex_);
  if (worker_.joinable()) {
    return;
  }

  stopping_ = false;
  worker_ = std::thread([this] { run(); });
}

void DaemonEventLoop::stop() {
  {
    std::lock_guard lock(queue_mutex_);
    stopping_ = true;
  }
  queue_changed_.notify_all();

  if (worker_.joinable() && !on_event_thread()) {
    worker_.join();
  }
}

void DaemonEventLoop::notify_attach_workers_changed() {
  state_.attach_workers_changed.notify_all();
}

void DaemonEventLoop::set_handler(
    std::function<DaemonEventResult(DaemonState&, const DaemonEvent&)> handler) {
  handler_ = std::move(handler);
}

DaemonEventResult DaemonEventLoop::call_event(DaemonEvent event) {
  return call([this, event = std::move(event)](DaemonState& state) mutable {
    if (handler_) {
      return handler_(state, event);
    }
    return handle_daemon_event(state, event);
  });
}

void DaemonEventLoop::post_event(DaemonEvent event) {
  post([this, event = std::move(event)](DaemonState& state) mutable {
    if (handler_) {
      (void)handler_(state, event);
      return;
    }
    (void)handle_daemon_event(state, event);
  });
}

bool DaemonEventLoop::on_event_thread() const {
  return worker_id_ != std::thread::id{} && std::this_thread::get_id() == worker_id_;
}

void DaemonEventLoop::enqueue(Event event) {
  {
    std::lock_guard lock(queue_mutex_);
    if (stopping_) {
      throw std::runtime_error{"wmux daemon event loop is stopped"};
    }
    queue_.push_back(std::move(event));
    const auto depth = static_cast<std::uint64_t>(queue_.size());
    state_.event_queue_depth.store(depth, std::memory_order_relaxed);
    update_atomic_peak(state_.peak_event_queue_depth, depth);
  }
  queue_changed_.notify_one();
}

void DaemonEventLoop::run() {
  worker_id_ = std::this_thread::get_id();

  while (true) {
    Event event;
    {
      std::unique_lock lock(queue_mutex_);
      queue_changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          break;
        }
        continue;
      }

      event = std::move(queue_.front());
      queue_.pop_front();
      state_.event_queue_depth.store(
          static_cast<std::uint64_t>(queue_.size()),
          std::memory_order_relaxed);
    }

    DaemonStateMutationScope mutation_scope;
    event(state_);
  }

  worker_id_ = {};
}

DaemonEventResult handle_daemon_event(DaemonState& state, const DaemonEvent& event) {
  assert_daemon_state_mutation_allowed("handle_daemon_event");

  switch (event.kind) {
    case DaemonEventKind::AttachStart:
      {
        DaemonEventResult result;
        result.ok = false;
        result.handled = true;
        result.has_response = true;
        result.response = make_response_json(false, "wmux: attach event handler is not installed\n");
        return result;
      }

    case DaemonEventKind::ClientConnected:
      {
        log_event(
            LogLevel::Info,
            "daemon.event",
            "client_connected",
            {{"client_id", event.client_id ? std::to_string(*event.client_id) : ""}});
        record_diagnostic_event(
            state,
            DiagnosticEvent{
                0,
                {},
                DiagnosticEventCategory::Process,
                "info",
                "client_connected",
                0,
                event.client_id.value_or(0),
                0,
                0,
                0,
                "client connected",
                {}});
      }
      return {};

    case DaemonEventKind::ClientDisconnected: {
      if (!event.client_id) {
        log_stale_event(event, "client");
        return {};
      }

      SessionId session_id{0};
      std::string session_name;
      {
        std::lock_guard lock(state.mutex);
        const auto client = state.attach_clients.find(*event.client_id);
        if (client == state.attach_clients.end()) {
          log_stale_event(event, "client");
          return {};
        }

        session_id = client->second.client.attached_session.value_or(0);
        session_name = client->second.session_name;
        state.attach_clients.erase(client);
      }
      state.attach_clients_changed.notify_all();
      log_event(
          LogLevel::Info,
          "daemon.event",
          "client_disconnected",
          {{"client_id", std::to_string(*event.client_id)},
           {"session_id", std::to_string(session_id)},
           {"session_name", session_name}});
      record_diagnostic_event(
          state,
          DiagnosticEvent{
              0,
              {},
              DiagnosticEventCategory::Process,
              "info",
              "client_disconnected",
              0,
              *event.client_id,
              session_id,
              0,
              0,
              "client disconnected",
              {{"reason", attach_end_reason_name(event.attach_end_reason)}}});
      return {};
    }

    case DaemonEventKind::ClientResize: {
      if (!event.client_id) {
        log_stale_event(event, "client");
        return {};
      }

      DaemonEventResult result;
      result.handled = true;
      std::lock_guard lock(state.mutex);
      const auto client = state.attach_clients.find(*event.client_id);
      if (client == state.attach_clients.end()) {
        log_stale_event(event, "client");
        return result;
      }

      state.render_metrics.client_resize_events.fetch_add(1, std::memory_order_relaxed);
      const auto previous_columns = client->second.client.size.columns;
      const auto previous_rows = client->second.client.size.rows;
      if (previous_columns == event.columns && previous_rows == event.rows) {
        state.render_metrics.client_resize_noops.fetch_add(1, std::memory_order_relaxed);
        return result;
      }

      client->second.client.size.columns = event.columns;
      client->second.client.size.rows = event.rows;
      client->second.client.render_state.dirty = true;
      record_diagnostic_event_locked(
          state,
          DiagnosticEvent{
              0,
              {},
              DiagnosticEventCategory::ResizeLayout,
              "info",
              "client_resize",
              0,
              *event.client_id,
              client->second.client.attached_session.value_or(0),
              client->second.client.active_window.value_or(0),
              client->second.client.active_pane.value_or(0),
              "client terminal resized",
              {{"columns", std::to_string(event.columns)},
               {"rows", std::to_string(event.rows)},
               {"previous_columns", std::to_string(previous_columns)},
               {"previous_rows", std::to_string(previous_rows)}}});
      result.changed = true;
      log_event(
          LogLevel::Debug,
          "daemon.event",
          "client_resize",
          {{"client_id", std::to_string(*event.client_id)},
           {"columns", std::to_string(event.columns)},
           {"rows", std::to_string(event.rows)},
           {"previous_columns", std::to_string(previous_columns)},
           {"previous_rows", std::to_string(previous_rows)}});
      return result;
    }

    case DaemonEventKind::DecodedKey: {
      if (!event.client_id) {
        log_stale_event(event, "client");
        return {};
      }

      std::lock_guard lock(state.mutex);
      const auto client = state.attach_clients.find(*event.client_id);
      if (client == state.attach_clients.end()) {
        log_stale_event(event, "client");
        return {};
      }
      record_diagnostic_event_locked(
          state,
          DiagnosticEvent{
              0,
              {},
              DiagnosticEventCategory::Key,
              "debug",
              "decoded_key",
              0,
              *event.client_id,
              client->second.client.attached_session.value_or(0),
              client->second.client.active_window.value_or(0),
              client->second.client.active_pane.value_or(0),
              "decoded key",
              {{"key", event.key.name}, {"bytes_len", std::to_string(event.key.bytes.size())}}});
      return {};
    }

    case DaemonEventKind::MouseEvent: {
      if (!event.client_id) {
        log_stale_event(event, "client");
        return {};
      }

      std::lock_guard lock(state.mutex);
      const auto client = state.attach_clients.find(*event.client_id);
      if (client == state.attach_clients.end()) {
        log_stale_event(event, "client");
        return {};
      }
      record_diagnostic_event_locked(
          state,
          DiagnosticEvent{
              0,
              {},
              DiagnosticEventCategory::Key,
              "debug",
              "mouse_event",
              0,
              *event.client_id,
              client->second.client.attached_session.value_or(0),
              client->second.client.active_window.value_or(0),
              client->second.client.active_pane.value_or(0),
              "mouse event",
              {{"column", std::to_string(event.mouse.column)},
               {"row", std::to_string(event.mouse.row)}}});
      return {};
    }

    case DaemonEventKind::ClientInput:
    case DaemonEventKind::MouseFocus:
    case DaemonEventKind::AttachCommand:
    case DaemonEventKind::CommandModeCommand:
    case DaemonEventKind::Paste: {
      if (!event.client_id) {
        log_stale_event(event, "client");
        return {};
      }

      std::lock_guard lock(state.mutex);
      if (state.attach_clients.find(*event.client_id) == state.attach_clients.end()) {
        log_stale_event(event, "client");
      }
      return {};
    }

    case DaemonEventKind::PasteBufferSet:
      {
        std::lock_guard lock(state.mutex);
        state.paste_buffer =
            make_paste_buffer(
                state.next_paste_buffer_id++,
                event.text,
                event.paste_source,
                state.config.values.limits.max_paste_buffer_bytes);
      }
      return {};

    case DaemonEventKind::PaneOutput:
    case DaemonEventKind::PaneExited: {
      std::lock_guard lock(state.mutex);
      if (!has_pane_runtime_locked(state, event.pane_id)) {
        log_stale_event(event, "pane");
        record_diagnostic_event_locked(
            state,
            DiagnosticEvent{
                0,
                {},
                DiagnosticEventCategory::Error,
                "debug",
                "stale_pane_event",
                0,
                0,
                0,
                0,
                event.pane_id,
                "stale pane event dropped",
                {{"event", std::string{daemon_event_kind_name(event.kind)}}}});
      } else if (event.kind == DaemonEventKind::PaneExited) {
        record_diagnostic_event_locked(
            state,
            DiagnosticEvent{
                0,
                {},
                DiagnosticEventCategory::Process,
                "info",
                "pane_exited",
                0,
                0,
                0,
                0,
                event.pane_id,
                "pane process exited",
                {{"exit_code", std::to_string(event.exit_code)}}});
      }
      return {};
    }

    case DaemonEventKind::Timer:
      log_event(
          LogLevel::Debug,
          "daemon.event",
          "timer",
          {{"sequence", std::to_string(event.timer.sequence)}});
      return {};

    case DaemonEventKind::Shutdown:
      {
        DaemonEventResult result;
        result.handled = true;
        result.request_shutdown = true;
        return result;
      }

    case DaemonEventKind::IpcCommand:
      {
        DaemonEventResult result;
        result.ok = false;
        result.handled = true;
        result.has_response = true;
        result.response = make_response_json(false, "wmux: command event handler is not installed\n");
        return result;
      }
  }

  return {};
}

void apply_daemon_config(
    DaemonState& state,
    std::filesystem::path path,
    ConfigParseResult result,
    bool file_exists) {
  assert_daemon_state_mutation_allowed("apply_daemon_config");
  std::size_t log_max_bytes = 0;
  {
    std::lock_guard lock(state.mutex);
    state.config.path = std::move(path);
    state.config.file_exists = file_exists;
    state.config.values = std::move(result.config);
    state.config.errors = std::move(result.errors);
    state.mouse_enabled = state.config.values.mouse_enabled;
    log_max_bytes = state.config.values.limits.max_log_file_bytes;
  }
  configure_logging(log_max_bytes);
}

void load_daemon_config(DaemonState& state) {
  const auto path = default_config_path();

  std::error_code exists_error;
  const bool file_exists = std::filesystem::exists(path, exists_error);
  auto result = load_config_file(path);
  apply_daemon_config(state, path, std::move(result), file_exists && !exists_error);
}

DaemonStats daemon_stats(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  const auto uptime_seconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - state.started_at)
          .count());
  std::size_t runtime_window_count = 0;
  std::size_t runtime_pane_count = 0;
  std::size_t live_shell_count = 0;
  std::size_t terminating_shell_count = 0;
  std::size_t job_object_shell_count = 0;
  std::size_t degraded_cleanup_shell_count = 0;
  for (const auto& [session_id, runtime] : state.runtimes) {
    (void)session_id;
    runtime_window_count += runtime.windows.size();
    for (const auto& [window_id, window] : runtime.windows) {
      (void)window_id;
      runtime_pane_count += window.panes.size();
      for (const auto& [pane_id, pane] : window.panes) {
        (void)pane_id;
        if (pane.shell) {
          ++live_shell_count;
          const auto lifecycle = pane.shell->lifecycle();
          if (lifecycle.terminating) {
            ++terminating_shell_count;
          }
          if (lifecycle.job_object_assigned) {
            ++job_object_shell_count;
          } else {
            ++degraded_cleanup_shell_count;
          }
        }
      }
    }
  }

  return {
      state.sessions.session_count(),
      state.attach_clients.size(),
      state.runtimes.size(),
      runtime_window_count,
      runtime_pane_count,
      live_shell_count,
      terminating_shell_count,
      job_object_shell_count,
      degraded_cleanup_shell_count,
      state.attach_workers.size(),
      uptime_seconds,
      state.event_queue_depth.load(std::memory_order_relaxed),
      state.peak_event_queue_depth.load(std::memory_order_relaxed),
      state.render_metrics.frames_written.load(std::memory_order_relaxed),
      state.render_metrics.full_frames_written.load(std::memory_order_relaxed),
      state.render_metrics.partial_frames_written.load(std::memory_order_relaxed),
      state.render_metrics.skipped_frames.load(std::memory_order_relaxed),
      state.render_metrics.coalesced_output_events.load(std::memory_order_relaxed),
      state.render_metrics.dirty_panes_rendered.load(std::memory_order_relaxed),
      state.render_metrics.bytes_written.load(std::memory_order_relaxed),
      state.render_metrics.pending_pane_output_bytes.load(std::memory_order_relaxed),
      state.render_metrics.peak_pending_pane_output_bytes.load(std::memory_order_relaxed),
      state.render_metrics.pending_client_output_bytes.load(std::memory_order_relaxed),
      state.render_metrics.peak_pending_client_output_bytes.load(std::memory_order_relaxed),
      state.render_metrics.render_frame_duration_us.load(std::memory_order_relaxed),
      state.render_metrics.max_render_frame_duration_us.load(std::memory_order_relaxed),
      state.render_metrics.slow_clients.load(std::memory_order_relaxed),
      state.render_metrics.write_failures.load(std::memory_order_relaxed),
      state.render_metrics.client_resize_events.load(std::memory_order_relaxed),
      state.render_metrics.client_resize_noops.load(std::memory_order_relaxed),
      state.render_metrics.pty_resize_requests.load(std::memory_order_relaxed),
      state.render_metrics.pty_resize_applied.load(std::memory_order_relaxed),
      state.render_metrics.pty_resize_skipped.load(std::memory_order_relaxed),
      state.render_metrics.pty_resize_failures.load(std::memory_order_relaxed),
      state.paste_buffer.id,
      state.paste_buffer.text.size(),
      state.paste_buffer.original_bytes,
      state.paste_buffer.truncated,
      state.paste_buffer.source,
      state.mouse_enabled,
      state.config,
      state.diagnostics.errors.size()};
}

DaemonAttachSettings daemon_attach_settings(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return {
      state.mouse_enabled,
      state.config.values.prefix,
      state.config.values.status_bar_enabled,
      state.config.values.escape_time_ms,
      state.config.values.limits,
      serialize_attach_key_binding_overrides(state.config.values.keys.bindings)};
}

void set_daemon_mouse_enabled(DaemonState& state, bool enabled) {
  assert_daemon_state_mutation_allowed("set_daemon_mouse_enabled");
  std::lock_guard lock(state.mutex);
  state.mouse_enabled = enabled;
}

void install_pane_runtime_shell_locked(
    DaemonState& state,
    SessionId session_id,
    WindowId window_id,
    PaneId pane_id,
    std::shared_ptr<PtyProcess> shell) {
  assert_daemon_state_mutation_allowed("install_pane_runtime_shell_locked");
  auto& pane_runtime = state.runtimes[session_id].windows[window_id].panes[pane_id];
  pane_runtime.shell = std::move(shell);
  pane_runtime.pty_columns = 0;
  pane_runtime.pty_rows = 0;
}

std::vector<std::shared_ptr<PtyProcess>> take_all_shells(DaemonState& state) {
  assert_daemon_state_mutation_allowed("take_all_shells");
  std::vector<std::shared_ptr<PtyProcess>> shells;
  {
    std::lock_guard lock(state.mutex);
    shells.reserve(state.runtimes.size());
    for (auto& [id, runtime] : state.runtimes) {
      (void)id;
      for (auto& [window_id, window] : runtime.windows) {
        (void)window_id;
        for (auto& [pane_id, pane] : window.panes) {
          (void)pane_id;
          if (pane.shell) {
            shells.push_back(std::move(pane.shell));
          }
        }
      }
    }
    state.runtimes.clear();
  }

  return shells;
}

void sync_attach_client_focus_locked(DaemonState& state, SessionId session_id) {
  assert_daemon_state_mutation_allowed("sync_attach_client_focus_locked");
  const auto active_window = state.sessions.active_window_id(session_id);
  const auto active_pane = state.sessions.active_pane_id(session_id);
  for (auto& [client_id, client] : state.attach_clients) {
    (void)client_id;
    if (client.client.attached_session != session_id) {
      continue;
    }

    client.client.active_window = active_window;
    client.client.active_pane = active_pane;
    client.client.render_state.dirty = true;
  }
}

}  // namespace wmux::daemon_internal
