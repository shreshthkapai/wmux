#include "wmux/terminal_engine.hpp"

#include "terminal_engine_v2_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

namespace wmux {
namespace {

std::string normalized_engine_name(const char* value) {
  if (value == nullptr) {
    return {};
  }

  std::string name{value};
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return name;
}

std::string terminal_engine_environment_value() {
#if defined(_MSC_VER)
  char* raw = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&raw, &size, "WMUX_TERMINAL_ENGINE") != 0 || raw == nullptr) {
    return {};
  }
  std::string value{raw};
  std::free(raw);
  return value;
#else
  return normalized_engine_name(std::getenv("WMUX_TERMINAL_ENGINE"));
#endif
}

}  // namespace

struct TerminalEngineV2::Impl {
  Impl() = default;
  Impl(int columns, int rows) : grid(columns, rows) {}

  terminal_engine_v2::GridCore grid;
  terminal_engine_v2::VtParser parser;
};

TerminalEngineV2::TerminalEngineV2() : impl_(std::make_unique<Impl>()) {}

TerminalEngineV2::TerminalEngineV2(int columns, int rows)
    : impl_(std::make_unique<Impl>(columns, rows)) {}

TerminalEngineV2::~TerminalEngineV2() = default;

TerminalEngineV2::TerminalEngineV2(TerminalEngineV2&&) noexcept = default;

TerminalEngineV2& TerminalEngineV2::operator=(TerminalEngineV2&&) noexcept = default;

void TerminalEngineV2::resize(int columns, int rows) {
  impl_->grid.resize(columns, rows);
}

void TerminalEngineV2::feed(std::span<const std::byte> bytes) {
  terminal_engine_v2::ScreenWriter writer{impl_->grid};
  impl_->parser.parse(bytes, writer);
}

int TerminalEngineV2::columns() const {
  return impl_->grid.columns();
}

int TerminalEngineV2::rows() const {
  return impl_->grid.rows();
}

CursorState TerminalEngineV2::cursor() const {
  return impl_->grid.cursor();
}

TerminalLineView TerminalEngineV2::line_view(int row) const {
  return impl_->grid.line_view(row);
}

std::uint64_t TerminalEngineV2::line_generation(int row) const {
  return impl_->grid.line_generation(row);
}

ScrollbackView TerminalEngineV2::scrollback_view(int start, int count) const {
  return impl_->grid.scrollback_view(start, count);
}

TerminalDamage TerminalEngineV2::consume_damage() {
  return impl_->grid.consume_damage();
}

void TerminalEngineV2::set_scrollback_capacity(std::size_t capacity) {
  impl_->grid.set_scrollback_capacity(capacity);
}

TerminalScreenSnapshot TerminalEngineV2::snapshot(bool consume_dirty, bool dirty_rows_only) const {
  return impl_->grid.snapshot(consume_dirty, dirty_rows_only);
}

TerminalScrollbackSnapshot TerminalEngineV2::scrollback_snapshot() const {
  return impl_->grid.scrollback_snapshot();
}

TerminalScrollbackSnapshot TerminalEngineV2::scrollback_snapshot_range(
    std::size_t first_line,
    std::size_t line_count) const {
  return impl_->grid.scrollback_snapshot_range(first_line, line_count);
}

TerminalEngineKind terminal_engine_kind_from_environment() {
  const auto name = normalized_engine_name(terminal_engine_environment_value().c_str());
  if (name == "v2") {
    return TerminalEngineKind::V2;
  }
  return TerminalEngineKind::Legacy;
}

std::string terminal_engine_kind_name(TerminalEngineKind kind) {
  switch (kind) {
    case TerminalEngineKind::V2:
      return "v2";
    case TerminalEngineKind::Legacy:
    default:
      return "legacy";
  }
}

std::unique_ptr<ITerminalEngine> make_terminal_engine() {
  return make_terminal_engine(80, 24);
}

std::unique_ptr<ITerminalEngine> make_terminal_engine(int columns, int rows) {
  switch (terminal_engine_kind_from_environment()) {
    case TerminalEngineKind::V2:
      return std::make_unique<TerminalEngineV2>(columns, rows);
    case TerminalEngineKind::Legacy:
    default:
      return std::make_unique<LegacyTerminalEngine>(columns, rows);
  }
}

}  // namespace wmux
