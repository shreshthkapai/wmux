#pragma once

#include "wmux/terminal_grid.hpp"

#include <cstdint>
#include <cstddef>
#include <string>

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

}  // namespace wmux
