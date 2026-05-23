#include "daemon_state.hpp"

#include <sstream>

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
    case WindowError::None:
      out << "wmux: window operation failed\n";
      break;
  }

  return out.str();
}

DaemonStats daemon_stats(DaemonState& state) {
  std::lock_guard lock(state.mutex);
  return {state.sessions.session_count(), state.attach_clients.size()};
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
        if (window.shell) {
          shells.push_back(std::move(window.shell));
        }
      }
    }
    state.runtimes.clear();
  }

  return shells;
}

}  // namespace wmux::daemon_internal
