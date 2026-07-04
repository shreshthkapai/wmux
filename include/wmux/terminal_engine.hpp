#pragma once

#include "wmux/terminal_grid.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wmux {

// OS-agnostic pane terminal engine boundary.
//
// A virtual call per PTY feed chunk is acceptable. Implementations must not
// route bytes, cells, or parser operations through virtual calls.
class ITerminalEngine {
 public:
  virtual ~ITerminalEngine() = default;

  virtual void resize(int columns, int rows) = 0;
  virtual void feed(std::span<const std::byte> bytes) = 0;
  virtual int columns() const = 0;
  virtual int rows() const = 0;
  virtual CursorState cursor() const = 0;
  virtual TerminalLineView line_view(int row) const = 0;
  virtual std::uint64_t line_generation(int row) const = 0;
  virtual ScrollbackView scrollback_view(int start, int count) const = 0;
  virtual TerminalDamage consume_damage() = 0;

  virtual void set_scrollback_capacity(std::size_t capacity) = 0;
  virtual TerminalScreenSnapshot snapshot(
      bool consume_dirty = false,
      bool dirty_rows_only = false) const = 0;
  virtual TerminalScrollbackSnapshot scrollback_snapshot() const = 0;
  virtual TerminalScrollbackSnapshot scrollback_snapshot_range(
      std::size_t first_line,
      std::size_t line_count) const = 0;
};

// Robust fallback engine backed by the current TerminalGrid implementation.
// This keeps the known-correct engine available while faster engines are built
// behind the same chunk-level boundary.
class LegacyTerminalEngine final : public ITerminalEngine {
 public:
  LegacyTerminalEngine();
  LegacyTerminalEngine(int columns, int rows);

  void resize(int columns, int rows) override;
  void feed(std::span<const std::byte> bytes) override;
  void feed(std::string_view bytes);
  int columns() const override;
  int rows() const override;
  CursorState cursor() const override;
  TerminalLineView line_view(int row) const override;
  std::uint64_t line_generation(int row) const override;
  ScrollbackView scrollback_view(int start, int count) const override;
  TerminalDamage consume_damage() override;

  void set_scrollback_capacity(std::size_t capacity) override;
  TerminalScreenSnapshot snapshot(
      bool consume_dirty = false,
      bool dirty_rows_only = false) const override;
  TerminalScrollbackSnapshot scrollback_snapshot() const override;
  TerminalScrollbackSnapshot scrollback_snapshot_range(
      std::size_t first_line,
      std::size_t line_count) const override;

 private:
  TerminalGrid grid_;
};

// Experimental engine boundary for the next ingestion architecture. It is
// intentionally opt-in while it grows behind ITerminalEngine.
class TerminalEngineV2 final : public ITerminalEngine {
 public:
  TerminalEngineV2();
  TerminalEngineV2(int columns, int rows);
  ~TerminalEngineV2() override;

  TerminalEngineV2(const TerminalEngineV2&) = delete;
  TerminalEngineV2& operator=(const TerminalEngineV2&) = delete;
  TerminalEngineV2(TerminalEngineV2&&) noexcept;
  TerminalEngineV2& operator=(TerminalEngineV2&&) noexcept;

  void resize(int columns, int rows) override;
  void feed(std::span<const std::byte> bytes) override;
  int columns() const override;
  int rows() const override;
  CursorState cursor() const override;
  TerminalLineView line_view(int row) const override;
  std::uint64_t line_generation(int row) const override;
  ScrollbackView scrollback_view(int start, int count) const override;
  TerminalDamage consume_damage() override;
  void set_scrollback_capacity(std::size_t capacity) override;
  TerminalScreenSnapshot snapshot(
      bool consume_dirty = false,
      bool dirty_rows_only = false) const override;
  TerminalScrollbackSnapshot scrollback_snapshot() const override;
  TerminalScrollbackSnapshot scrollback_snapshot_range(
      std::size_t first_line,
      std::size_t line_count) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class TerminalEngineKind {
  Legacy,
  V2,
};

TerminalEngineKind terminal_engine_kind_from_environment();
std::string terminal_engine_kind_name(TerminalEngineKind kind);
std::unique_ptr<ITerminalEngine> make_terminal_engine();
std::unique_ptr<ITerminalEngine> make_terminal_engine(int columns, int rows);

}  // namespace wmux
