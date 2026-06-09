#include "daemon_state.hpp"

#include <filesystem>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace wmux::daemon_internal {

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
    }

    event(state_);
  }

  worker_id_ = {};
}

void apply_daemon_config(
    DaemonState& state,
    std::filesystem::path path,
    ConfigParseResult result,
    bool file_exists) {
  std::lock_guard lock(state.mutex);
  state.config.path = std::move(path);
  state.config.file_exists = file_exists;
  state.config.values = std::move(result.config);
  state.config.errors = std::move(result.errors);
  state.mouse_enabled = state.config.values.mouse_enabled;
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
  std::size_t runtime_window_count = 0;
  std::size_t runtime_pane_count = 0;
  std::size_t live_shell_count = 0;
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
      state.attach_workers.size(),
      state.render_metrics.frames_written.load(std::memory_order_relaxed),
      state.render_metrics.full_frames_written.load(std::memory_order_relaxed),
      state.render_metrics.partial_frames_written.load(std::memory_order_relaxed),
      state.render_metrics.skipped_frames.load(std::memory_order_relaxed),
      state.render_metrics.dirty_panes_rendered.load(std::memory_order_relaxed),
      state.render_metrics.bytes_written.load(std::memory_order_relaxed),
      state.render_metrics.write_failures.load(std::memory_order_relaxed),
      state.mouse_enabled,
      state.config};
}

DaemonAttachSettings daemon_attach_settings(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return {
      state.mouse_enabled,
      state.config.values.prefix,
      state.config.values.status_bar_enabled};
}

void set_daemon_mouse_enabled(DaemonState& state, bool enabled) {
  std::lock_guard lock(state.mutex);
  state.mouse_enabled = enabled;
}

std::vector<std::shared_ptr<PtyProcess>> take_all_shells(DaemonState& state) {
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

}  // namespace wmux::daemon_internal
