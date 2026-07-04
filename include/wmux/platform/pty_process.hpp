#pragma once

#include "wmux/pty_output.hpp"
#include "wmux/resource_limits.hpp"
#include "wmux/terminal_engine.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
  std::uint64_t output_read_chunks{0};
  std::uint64_t output_read_bytes{0};
  std::uint64_t output_feed_duration_us{0};
  std::uint64_t max_output_feed_duration_us{0};
  std::uint64_t output_lock_wait_duration_us{0};
  std::uint64_t max_output_lock_wait_duration_us{0};
  std::uint64_t output_grid_feed_duration_us{0};
  std::uint64_t max_output_grid_feed_duration_us{0};
  std::uint64_t output_buffer_duration_us{0};
  std::uint64_t max_output_buffer_duration_us{0};
};

class PtyProcess;

struct PtyProcessResult {
  std::shared_ptr<PtyProcess> process;
  std::string error;
  PtySpawnOptions options;
};

struct PtyScreenRenderView {
  std::uint64_t next_sequence{1};
  bool alive{false};
  std::size_t buffered_raw_bytes{0};
  std::uint64_t dropped_raw_chunks{0};
  CursorState cursor;
  int columns{0};
  int rows{0};
  std::size_t scrollback_line_count{0};
  TerminalDamage damage;
  const ITerminalEngine* engine{nullptr};
};

using PtyScreenRenderCallback = std::function<void(const PtyScreenRenderView&)>;

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
  PtyOutputSnapshot output_snapshot(
      PtyOutputSnapshotMode mode = PtyOutputSnapshotMode::FullHistory,
      bool consume_dirty = false,
      bool dirty_rows_only = false,
      std::optional<PtyOutputScrollbackRange> scrollback_range = std::nullopt) const;
  void with_screen_render_view(
      bool consume_damage,
      const PtyScreenRenderCallback& callback) const;
  PtyOutputChunk wait_for_output(
      std::uint64_t next_sequence,
      std::chrono::milliseconds timeout) const;
  PtyOutputDrain wait_for_output_drain(
      std::uint64_t next_sequence,
      std::chrono::milliseconds timeout) const;
  bool terminate();

 private:
  struct Impl;

  explicit PtyProcess(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace wmux
