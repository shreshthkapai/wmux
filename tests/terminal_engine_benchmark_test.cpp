#include "wmux/terminal_engine.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kDefaultFixtureBytes = 10u * 1024u * 1024u;
constexpr std::size_t kFeedChunkBytes = 64u * 1024u;

struct BenchmarkFixture {
  std::string name;
  std::string bytes;
  double target_ms_per_mb{0.0};
};

struct BenchmarkResult {
  std::string name;
  std::size_t bytes{0};
  double elapsed_ms{0.0};
  double ms_per_mb{0.0};
  double target_ms_per_mb{0.0};
};

std::string env_value(std::string_view name) {
#if defined(_MSC_VER)
  char* raw = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&raw, &size, std::string{name}.c_str()) != 0 || raw == nullptr) {
    return {};
  }
  std::string value{raw};
  std::free(raw);
  return value;
#else
  if (const char* raw = std::getenv(std::string{name}.c_str())) {
    return raw;
  }
  return {};
#endif
}

bool env_bool(std::string_view name, bool default_value) {
  const auto value = env_value(name);
  if (value.empty()) {
    return default_value;
  }
  return value == "1" || value == "true" || value == "TRUE" || value == "on" ||
         value == "ON" || value == "yes" || value == "YES";
}

std::string escaped_preview(std::string_view text, std::size_t limit = 220) {
  std::string out;
  std::size_t used = 0;
  for (unsigned char ch : text) {
    if (used++ >= limit) {
      out += "...";
      break;
    }
    if (ch == '\x1b') {
      out += "\\e";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (ch < 0x20 || ch == 0x7f) {
      out += "\\x";
      constexpr char digits[] = "0123456789ABCDEF";
      out.push_back(digits[(ch >> 4) & 0xf]);
      out.push_back(digits[ch & 0xf]);
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  return out;
}

std::size_t env_size(std::string_view name, std::size_t default_value) {
  const auto value = env_value(name);
  if (value.empty()) {
    return default_value;
  }

  try {
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::stoull(value)));
  } catch (...) {
    return default_value;
  }
}

void append_until_size(std::string& output, std::string_view pattern, std::size_t target_bytes) {
  assert(!pattern.empty());
  while (output.size() < target_bytes) {
    output.append(pattern);
  }
}

std::string repeated_pattern(std::string_view pattern, std::size_t target_bytes) {
  std::string output;
  output.reserve(target_bytes);
  append_until_size(output, pattern, target_bytes);
  return output;
}

std::string read_binary_file(std::string_view path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file{std::string{path}, std::ios::binary};
  if (!file) {
    std::cerr << "wmux benchmark: failed to open capture file: " << path << "\n";
    return {};
  }
  return std::string{
      std::istreambuf_iterator<char>{file},
      std::istreambuf_iterator<char>{}};
}

std::string plain_ascii_lines(std::size_t target_bytes) {
  return repeated_pattern(
      "2026-07-04T12:00:00Z INFO worker=17 task=terminal-feed line=000000 status=ok\r\n",
      target_bytes);
}

std::string ansi_colored_log_lines(std::size_t target_bytes) {
  return repeated_pattern(
      "\x1b[32mINFO\x1b[0m "
      "\x1b[38;5;39mwmux\x1b[0m "
      "\x1b[90mrequest_id=18446744073709551615\x1b[0m "
      "pane=3 bytes=4096 status=\x1b[1;33mcoalesced\x1b[0m\r\n",
      target_bytes);
}

std::string scroll_heavy_bottom_output(std::size_t target_bytes) {
  return repeated_pattern("scroll-heavy bottom output row with enough text to wrap naturally 000000\r\n",
                          target_bytes);
}

std::string codex_like_transcript(std::size_t target_bytes) {
  return repeated_pattern(
      "\x1b[?25l"
      "\r\x1b[2K"
      "\x1b[1mCodex\x1b[0m resumed conversation 019f23e5-a519-7341-8542-62e6682b84d9\r\n"
      "\x1b[90mthinking\x1b[0m inspecting terminal engine hot path and applying patch\r\n"
      "\x1b[38;5;39mupdate\x1b[0m generated 1428 chunks, coalescing latest state only\r\n"
      "\x1b[?25h",
      target_bytes);
}

std::string unicode_wide_mixed_output(std::size_t target_bytes) {
  return repeated_pattern(
      "ascii \xe4\xb8\xad\xe6\x96\x87 wide \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb "
      "combining e\xcc\x81 flags \xf0\x9f\x87\xba\xf0\x9f\x87\xb8 mixed output\r\n",
      target_bytes);
}

std::string resize_redraw_burst(std::size_t target_bytes) {
  return repeated_pattern(
      "resize burst frame line before resize\r\n"
      "\x1b[2J\x1b[H"
      "after clear and home cursor for redraw burst\r\n",
      target_bytes);
}

std::vector<BenchmarkFixture> benchmark_fixtures(std::size_t fixture_bytes) {
  std::vector<BenchmarkFixture> fixtures{
      {"10MB plain ASCII lines", plain_ascii_lines(fixture_bytes), 50.0},
      {"10MB ANSI colored log lines", ansi_colored_log_lines(fixture_bytes), 100.0},
      {"10MB scroll-heavy bottom output", scroll_heavy_bottom_output(fixture_bytes), 150.0},
      {"Codex captured raw transcript", codex_like_transcript(fixture_bytes), 150.0},
      {"Resize/redraw burst", resize_redraw_burst(fixture_bytes), 150.0},
      {"Unicode/wide mixed output", unicode_wide_mixed_output(fixture_bytes), 200.0},
  };
  const auto capture_path = env_value("WMUX_BENCHMARK_CAPTURE_FILE");
  if (!capture_path.empty()) {
    auto capture = read_binary_file(capture_path);
    if (!capture.empty()) {
      fixtures.push_back({"Captured PTY transcript", std::move(capture), 150.0});
    }
  }
  return fixtures;
}

void feed_in_chunks(wmux::ITerminalEngine& engine, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto chunk_size = std::min(kFeedChunkBytes, bytes.size() - offset);
    const auto* chunk = reinterpret_cast<const std::byte*>(bytes.data() + offset);
    engine.feed(std::span<const std::byte>{chunk, chunk_size});
    offset += chunk_size;
  }
}

void apply_resize_burst(wmux::ITerminalEngine& engine, std::size_t step) {
  if (step % 4 == 0) {
    engine.resize(100, 30);
  } else if (step % 4 == 1) {
    engine.resize(132, 40);
  } else if (step % 4 == 2) {
    engine.resize(80, 24);
  } else {
    engine.resize(120, 35);
  }
}

BenchmarkResult run_fixture(
    wmux::ITerminalEngine& engine,
    const BenchmarkFixture& fixture,
    bool include_resize_burst) {
  const auto start = std::chrono::steady_clock::now();

  std::size_t offset = 0;
  std::size_t step = 0;
  while (offset < fixture.bytes.size()) {
    if (include_resize_burst) {
      apply_resize_burst(engine, step);
    }

    const auto chunk_size = std::min(kFeedChunkBytes, fixture.bytes.size() - offset);
    const auto* chunk = reinterpret_cast<const std::byte*>(fixture.bytes.data() + offset);
    engine.feed(std::span<const std::byte>{chunk, chunk_size});
    offset += chunk_size;
    ++step;
  }

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration<double, std::milli>(end - start).count();
  const auto mb = static_cast<double>(fixture.bytes.size()) / (1024.0 * 1024.0);
  return BenchmarkResult{
      fixture.name,
      fixture.bytes.size(),
      elapsed,
      mb > 0.0 ? elapsed / mb : elapsed,
      fixture.target_ms_per_mb,
  };
}

std::unique_ptr<wmux::ITerminalEngine> make_benchmark_engine() {
  const auto engine_name = env_value("WMUX_BENCHMARK_ENGINE");
  const auto columns = static_cast<int>(env_size("WMUX_BENCHMARK_COLUMNS", 120));
  const auto rows = static_cast<int>(env_size("WMUX_BENCHMARK_ROWS", 35));
  if (engine_name == "legacy") {
    return std::make_unique<wmux::LegacyTerminalEngine>(columns, rows);
  }
  return std::make_unique<wmux::TerminalEngineV2>(columns, rows);
}

bool should_enforce_benchmarks() {
#if defined(NDEBUG)
  constexpr bool kDefault = true;
#else
  constexpr bool kDefault = false;
#endif
  return env_bool("WMUX_BENCHMARK_ENFORCE", kDefault);
}

void assert_no_corruption(wmux::ITerminalEngine& engine) {
  const auto snapshot = engine.snapshot();
  assert(snapshot.columns == engine.columns());
  assert(snapshot.rows == engine.rows());
  assert(snapshot.lines.size() == static_cast<std::size_t>(engine.rows()));
  assert(snapshot.line_snapshots.size() == static_cast<std::size_t>(engine.rows()));
  for (const auto& line : snapshot.lines) {
    assert(line.size() >= static_cast<std::size_t>(engine.columns()));
  }
}

bool nondefault_attributes(const wmux::TerminalAttributes& attributes) {
  return attributes.bold || attributes.dim || attributes.italic || attributes.underline ||
         attributes.inverse || attributes.foreground != -1 || attributes.background != -1;
}

void dump_final_screen(const BenchmarkFixture& fixture, wmux::ITerminalEngine& engine) {
  if (!env_bool("WMUX_BENCHMARK_DUMP_FINAL_SCREEN", false)) {
    return;
  }
  if (fixture.name != "Captured PTY transcript") {
    return;
  }

  const auto snapshot = engine.snapshot();
  std::cout << "captured final screen dump\n";
  std::cout << "size: " << snapshot.columns << "x" << snapshot.rows
            << " cursor=" << snapshot.cursor_column << "," << snapshot.cursor_row
            << " visible=" << (snapshot.cursor_visible ? "yes" : "no")
            << " alternate=" << (snapshot.alternate_screen ? "yes" : "no")
            << " title=\"" << escaped_preview(snapshot.title, 80) << "\"\n";

  for (std::size_t row = 0; row < snapshot.line_snapshots.size(); ++row) {
    const auto& line = snapshot.line_snapshots[row];
    std::size_t styled = 0;
    std::size_t nonblank = 0;
    std::size_t extended = 0;
    for (std::size_t column = 0; column < line.attributes.size(); ++column) {
      if (nondefault_attributes(line.attributes[column])) {
        ++styled;
      }
    }
    for (const auto& cell : line.cells) {
      if (!cell.empty() && cell != " ") {
        ++nonblank;
      }
      if (cell.size() > 1) {
        ++extended;
      }
    }
    std::cout << "row " << std::setw(2) << row
              << " styled=" << std::setw(3) << styled
              << " nonblank=" << std::setw(3) << nonblank
              << " extended=" << std::setw(3) << extended
              << " text=\"" << escaped_preview(line.text) << "\"\n";
  }
}

void print_result(const BenchmarkResult& result, bool enforced) {
  std::cout << std::left << std::setw(34) << result.name << " "
            << std::right << std::fixed << std::setprecision(2) << std::setw(8)
            << static_cast<double>(result.bytes) / (1024.0 * 1024.0) << " MiB  "
            << std::fixed << std::setprecision(2) << std::setw(10) << result.elapsed_ms
            << " ms  " << std::setw(8) << result.ms_per_mb << " ms/MB  target < "
            << result.target_ms_per_mb << " ms/MB";
  if (!enforced) {
    std::cout << "  (not enforced)";
  }
  std::cout << "\n";
}

void fail_benchmark(const BenchmarkResult& result) {
  std::cerr << "terminal engine benchmark acceptance failure: " << result.name
            << " measured " << result.ms_per_mb << " ms/MB, target < "
            << result.target_ms_per_mb << " ms/MB\n";
  std::exit(1);
}

void run_correctness_probe(const BenchmarkFixture& fixture) {
  wmux::LegacyTerminalEngine legacy{80, 24};
  wmux::TerminalEngineV2 v2{80, 24};
  const auto probe_size = std::min<std::size_t>(fixture.bytes.size(), 256u * 1024u);
  const std::string_view probe{fixture.bytes.data(), probe_size};
  feed_in_chunks(legacy, probe);
  feed_in_chunks(v2, probe);
  assert(legacy.snapshot().lines == v2.snapshot().lines);
  assert_no_corruption(v2);
}

}  // namespace

void run_terminal_engine_benchmark_tests() {
  const auto fixture_bytes = env_size("WMUX_BENCHMARK_FIXTURE_BYTES", kDefaultFixtureBytes);
  const bool enforce = should_enforce_benchmarks();
  std::cout << "wmux terminal engine benchmark fixtures\n";
  std::cout << "engine: "
            << (env_value("WMUX_BENCHMARK_ENGINE") == "legacy" ? "legacy" : "v2") << "\n";
  std::cout << "fixture bytes: " << fixture_bytes << "\n";
  std::cout << "enforce acceptance: " << (enforce ? "yes" : "no") << "\n";

  const auto fixtures = benchmark_fixtures(fixture_bytes);
  for (const auto& fixture : fixtures) {
    run_correctness_probe(fixture);

    auto engine = make_benchmark_engine();
    const bool resize_burst = fixture.name == "Resize/redraw burst";
    const auto result = run_fixture(*engine, fixture, resize_burst);
    assert_no_corruption(*engine);
    dump_final_screen(fixture, *engine);
    print_result(result, enforce);

    if (enforce && result.ms_per_mb > result.target_ms_per_mb) {
      fail_benchmark(result);
    }
  }
}
