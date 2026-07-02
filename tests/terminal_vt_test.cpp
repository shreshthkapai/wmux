#include "wmux/terminal_vt.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

void parses_printable_controls_csi_and_osc() {
  wmux::TerminalVtParser parser;
  std::vector<wmux::TerminalVtOperation> operations;

  parser.feed("A\r\n\x1b[2;3H\x1b]2;wmux\a", operations);

  assert(operations.size() == 5);
  assert(operations[0].kind == wmux::TerminalVtOperationKind::Print);
  assert(operations[0].codepoint == 'A');
  assert(operations[1].kind == wmux::TerminalVtOperationKind::CarriageReturn);
  assert(operations[2].kind == wmux::TerminalVtOperationKind::LineFeed);
  assert(operations[3].kind == wmux::TerminalVtOperationKind::Csi);
  assert(operations[3].csi.parameters == "2;3");
  assert(operations[3].csi.final_byte == 'H');
  assert(operations[4].kind == wmux::TerminalVtOperationKind::Osc);
  assert(operations[4].osc.payload == "2;wmux");
}

void keeps_partial_sequences_pending_across_feeds() {
  wmux::TerminalVtParser parser;
  std::vector<wmux::TerminalVtOperation> operations;

  parser.feed("\x1b[31", operations);
  assert(operations.empty());

  parser.feed("mX", operations);
  assert(operations.size() == 2);
  assert(operations[0].kind == wmux::TerminalVtOperationKind::Csi);
  assert(operations[0].csi.parameters == "31");
  assert(operations[0].csi.final_byte == 'm');
  assert(operations[1].kind == wmux::TerminalVtOperationKind::Print);
  assert(operations[1].codepoint == 'X');
}

void rejects_oversized_csi_without_visible_bytes() {
  wmux::TerminalVtParser parser;
  std::vector<wmux::TerminalVtOperation> operations;
  std::string oversized = "\x1b[";
  oversized.append(300, '1');
  oversized.push_back('m');

  parser.feed(oversized, operations);

  assert(operations.size() == 1);
  assert(operations[0].kind == wmux::TerminalVtOperationKind::Unknown);
  assert(operations[0].unknown.sequence_class == wmux::TerminalVtUnknownClass::Csi);
}

void decodes_utf8_and_reports_invalid_sequences() {
  wmux::TerminalVtParser parser;
  std::vector<wmux::TerminalVtOperation> operations;
  std::string bytes;
  bytes.push_back(static_cast<char>(0xe4));
  bytes.push_back(static_cast<char>(0xb8));
  bytes.push_back(static_cast<char>(0xad));
  bytes.push_back(static_cast<char>(0xe4));
  bytes.push_back('X');

  parser.feed(bytes, operations);

  assert(operations.size() == 4);
  assert(operations[0].kind == wmux::TerminalVtOperationKind::Print);
  assert(operations[0].codepoint == 0x4e2d);
  assert(operations[1].kind == wmux::TerminalVtOperationKind::Unknown);
  assert(operations[1].unknown.sequence_class == wmux::TerminalVtUnknownClass::Utf8);
  assert(operations[2].kind == wmux::TerminalVtOperationKind::Print);
  assert(operations[2].codepoint == 0xfffd);
  assert(operations[3].kind == wmux::TerminalVtOperationKind::Print);
  assert(operations[3].codepoint == 'X');
}

}  // namespace

void run_terminal_vt_tests() {
  parses_printable_controls_csi_and_osc();
  keeps_partial_sequences_pending_across_feeds();
  rejects_oversized_csi_without_visible_bytes();
  decodes_utf8_and_reports_invalid_sequences();
}
