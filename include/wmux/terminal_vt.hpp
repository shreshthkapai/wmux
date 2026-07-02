#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wmux {

enum class TerminalVtOperationKind {
  Print,
  CarriageReturn,
  LineFeed,
  Backspace,
  Tab,
  Escape,
  Csi,
  Osc,
  Unknown,
};

enum class TerminalVtUnknownClass {
  Escape,
  Csi,
  Osc,
  Utf8,
};

struct TerminalVtCsiOperation {
  std::string parameters;
  char final_byte{0};
};

struct TerminalVtOscOperation {
  std::string payload;
};

struct TerminalVtUnknownOperation {
  TerminalVtUnknownClass sequence_class{TerminalVtUnknownClass::Escape};
  std::size_t length{0};
  char final_byte{0};
};

struct TerminalVtOperation {
  TerminalVtOperationKind kind{TerminalVtOperationKind::Unknown};
  std::uint32_t codepoint{0};
  char escape_final{0};
  TerminalVtCsiOperation csi;
  TerminalVtOscOperation osc;
  TerminalVtUnknownOperation unknown;

  static TerminalVtOperation print(std::uint32_t codepoint);
  static TerminalVtOperation control(TerminalVtOperationKind kind);
  static TerminalVtOperation escape(char final_byte);
  static TerminalVtOperation make_csi(std::string parameters, char final_byte);
  static TerminalVtOperation make_osc(std::string payload);
  static TerminalVtOperation make_unknown(
      TerminalVtUnknownClass sequence_class,
      std::size_t length,
      char final_byte = 0);
};

class TerminalVtParser final {
 public:
  void reset();
  void feed(std::string_view bytes, std::vector<TerminalVtOperation>& operations);

 private:
  enum class State {
    Ground,
    Escape,
    Csi,
    CsiDiscard,
    Osc,
    OscEscape,
    OscDiscard,
    OscDiscardEscape,
  };

  void emit_pending_utf8_replacement(std::vector<TerminalVtOperation>& operations);
  bool feed_utf8_byte(unsigned char byte, std::vector<TerminalVtOperation>& operations);
  void enter_ground();

  State state_{State::Ground};
  std::string csi_buffer_;
  std::string osc_buffer_;
  std::size_t discarded_sequence_length_{0};
  int utf8_remaining_{0};
  std::uint32_t utf8_codepoint_{0};
};

std::string terminal_vt_unknown_class_name(TerminalVtUnknownClass sequence_class);

}  // namespace wmux
