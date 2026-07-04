#include "wmux/terminal_engine.hpp"

#include <string_view>

namespace wmux {

LegacyTerminalEngine::LegacyTerminalEngine() = default;

LegacyTerminalEngine::LegacyTerminalEngine(int columns, int rows) : grid_(columns, rows) {}

void LegacyTerminalEngine::resize(int columns, int rows) {
  grid_.resize(columns, rows);
}

void LegacyTerminalEngine::feed(std::span<const std::byte> bytes) {
  const auto* data = reinterpret_cast<const char*>(bytes.data());
  grid_.feed(std::string_view{data, bytes.size()});
}

void LegacyTerminalEngine::feed(std::string_view bytes) {
  const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
  feed(std::span<const std::byte>{data, bytes.size()});
}

int LegacyTerminalEngine::columns() const {
  return grid_.columns();
}

int LegacyTerminalEngine::rows() const {
  return grid_.rows();
}

CursorState LegacyTerminalEngine::cursor() const {
  return grid_.cursor();
}

TerminalLineView LegacyTerminalEngine::line_view(int row) const {
  return grid_.line_view(row);
}

std::uint64_t LegacyTerminalEngine::line_generation(int row) const {
  return grid_.line_generation(row);
}

ScrollbackView LegacyTerminalEngine::scrollback_view(int start, int count) const {
  return grid_.scrollback_view(start, count);
}

TerminalDamage LegacyTerminalEngine::consume_damage() {
  return grid_.consume_damage();
}

void LegacyTerminalEngine::set_scrollback_capacity(std::size_t capacity) {
  grid_.set_scrollback_capacity(capacity);
}

TerminalScreenSnapshot LegacyTerminalEngine::snapshot(
    bool consume_dirty,
    bool dirty_rows_only) const {
  return grid_.snapshot(consume_dirty, dirty_rows_only);
}

TerminalScrollbackSnapshot LegacyTerminalEngine::scrollback_snapshot() const {
  return grid_.scrollback_snapshot();
}

TerminalScrollbackSnapshot LegacyTerminalEngine::scrollback_snapshot_range(
    std::size_t first_line,
    std::size_t line_count) const {
  return grid_.scrollback_snapshot_range(first_line, line_count);
}

}  // namespace wmux
