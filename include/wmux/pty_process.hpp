#pragma once

#include "wmux/terminal_grid.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace wmux {

struct PtyOutputSnapshot {
  std::string bytes;
  TerminalScreenSnapshot screen;
  TerminalScrollbackSnapshot scrollback;
  std::uint64_t next_sequence{1};
  std::size_t buffered_raw_bytes{0};
  std::uint64_t dropped_raw_chunks{0};
  bool alive{false};
};

struct PtyOutputChunk {
  std::string bytes;
  std::uint64_t next_sequence{1};
  bool sequence_compacted{false};
  bool alive{false};
};

class PtyProcess;

struct PtyProcessResult {
  std::shared_ptr<PtyProcess> process;
  std::string error;
};

class PtyProcess final {
 public:
  PtyProcess(const PtyProcess&) = delete;
  PtyProcess& operator=(const PtyProcess&) = delete;
  PtyProcess(PtyProcess&&) = delete;
  PtyProcess& operator=(PtyProcess&&) = delete;
  ~PtyProcess();

  static PtyProcessResult start(std::string_view command_line, short columns, short rows);

  bool write_input(std::string_view bytes);
  bool resize(short columns, short rows);
  std::uint32_t process_id() const;
  PtyOutputSnapshot output_snapshot() const;
  PtyOutputChunk wait_for_output(
      std::uint64_t next_sequence,
      std::chrono::milliseconds timeout) const;
  void terminate();

 private:
  struct Impl;

  explicit PtyProcess(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace wmux
