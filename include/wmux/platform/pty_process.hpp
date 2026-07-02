#pragma once

#include "wmux/pty_output.hpp"
#include "wmux/resource_limits.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace wmux {

struct PtySpawnOptions {
  std::string command_line;
  std::string executable;
  std::string source;
  std::string working_directory;
  ResourceLimits limits;
};

struct PtyProcessLifecycle {
  std::uint32_t process_id{0};
  std::chrono::steady_clock::time_point created_at{};
  bool terminating{false};
  bool reader_done{false};
  bool job_object_configured{false};
  bool job_object_assigned{false};
  bool pseudo_console_open{false};
  bool input_pipe_read_open{false};
  bool input_pipe_write_open{false};
  bool output_pipe_read_open{false};
  bool process_handle_open{false};
  bool primary_thread_handle_open{false};
  bool job_object_handle_open{false};
};

class PtyProcess;

struct PtyProcessResult {
  std::shared_ptr<PtyProcess> process;
  std::string error;
  PtySpawnOptions options;
};

// Platform-neutral facade for a pane PTY process. The current implementation is
// Windows ConPTY-backed, but daemon/core callers must depend only on this API.
class PtyProcess final {
 public:
  PtyProcess(const PtyProcess&) = delete;
  PtyProcess& operator=(const PtyProcess&) = delete;
  PtyProcess(PtyProcess&&) = delete;
  PtyProcess& operator=(PtyProcess&&) = delete;
  ~PtyProcess();

  static PtyProcessResult start(std::string_view command_line, short columns, short rows);
  static PtyProcessResult start(const PtySpawnOptions& options, short columns, short rows);

  bool write_input(std::string_view bytes);
  bool write_input_throttled(
      std::string_view bytes,
      std::size_t chunk_bytes,
      std::chrono::milliseconds delay_between_chunks);
  bool resize(short columns, short rows);
  std::uint32_t process_id() const;
  PtyProcessLifecycle lifecycle() const;
  PtyOutputSnapshot output_snapshot() const;
  PtyOutputChunk wait_for_output(
      std::uint64_t next_sequence,
      std::chrono::milliseconds timeout) const;
  bool terminate();

 private:
  struct Impl;

  explicit PtyProcess(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace wmux
