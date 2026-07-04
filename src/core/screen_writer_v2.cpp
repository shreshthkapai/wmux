#include "terminal_engine_v2_internal.hpp"

namespace wmux::terminal_engine_v2 {
namespace {

bool same_attributes(const TerminalAttributes& left, const TerminalAttributes& right) {
  return left.bold == right.bold && left.dim == right.dim && left.italic == right.italic &&
         left.underline == right.underline && left.inverse == right.inverse &&
         left.foreground == right.foreground && left.background == right.background;
}

}  // namespace

ScreenWriter::ScreenWriter(GridCore& grid) : grid_(grid) {}

void ScreenWriter::print_ascii_span(std::string_view text) {
  if (text.empty()) {
    return;
  }
  if (!has_pending_print_run_) {
    start_print_run(text);
    return;
  }

  const auto* pending_end =
      pending_print_run_.ascii.data() + pending_print_run_.ascii.size();
  if (pending_end == text.data() &&
      same_attributes(pending_print_run_.attributes, grid_.current_attributes())) {
    pending_print_run_.ascii =
        std::string_view{pending_print_run_.ascii.data(),
                         pending_print_run_.ascii.size() + text.size()};
    return;
  }

  flush_print_run();
  start_print_run(text);
}

void ScreenWriter::print_utf8(std::uint32_t codepoint, std::string_view glyph) {
  flush_print_run();
  grid_.print_codepoint(codepoint, glyph);
}

void ScreenWriter::execute_control(unsigned char byte) {
  flush_print_run();
  grid_.execute_control(byte);
}

void ScreenWriter::dispatch_escape(char final_byte) {
  flush_print_run();
  grid_.dispatch_escape(final_byte);
}

void ScreenWriter::designate_character_set(int slot, char final_byte) {
  flush_print_run();
  grid_.designate_character_set(slot, final_byte);
}

void ScreenWriter::dispatch_csi(const CsiParams& params, char final_byte) {
  flush_print_run();
  grid_.dispatch_csi(params, final_byte);
}

void ScreenWriter::dispatch_osc(std::string_view payload) {
  flush_print_run();
  grid_.dispatch_osc(payload);
}

void ScreenWriter::unknown(UnknownSequenceClass sequence_class, std::size_t length, char final_byte) {
  flush_print_run();
  grid_.record_unknown(sequence_class, length, final_byte);
}

void ScreenWriter::flush_print_run() {
  if (!has_pending_print_run_ || pending_print_run_.ascii.empty()) {
    has_pending_print_run_ = false;
    pending_print_run_ = {};
    return;
  }
  grid_.print_ascii_span(pending_print_run_.ascii);
  has_pending_print_run_ = false;
  pending_print_run_ = {};
}

void ScreenWriter::start_print_run(std::string_view text) {
  const auto cursor = grid_.cursor();
  pending_print_run_ = PendingPrintRun{
      cursor.row,
      cursor.column,
      grid_.current_attributes(),
      text,
  };
  has_pending_print_run_ = true;
}

}  // namespace wmux::terminal_engine_v2
