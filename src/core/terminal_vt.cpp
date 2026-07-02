#include "wmux/terminal_vt.hpp"

#include <utility>

namespace wmux {
namespace {

constexpr std::uint32_t kReplacementCodepoint = 0xfffd;
constexpr std::size_t kMaxCsiLength = 256;
constexpr std::size_t kMaxOscLength = 4096;

bool is_csi_final_byte(unsigned char byte) {
  return byte >= 0x40 && byte <= 0x7e;
}

bool is_utf8_continuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

}  // namespace

TerminalVtOperation TerminalVtOperation::print(std::uint32_t codepoint) {
  TerminalVtOperation operation;
  operation.kind = TerminalVtOperationKind::Print;
  operation.codepoint = codepoint;
  return operation;
}

TerminalVtOperation TerminalVtOperation::control(TerminalVtOperationKind kind) {
  TerminalVtOperation operation;
  operation.kind = kind;
  return operation;
}

TerminalVtOperation TerminalVtOperation::escape(char final_byte) {
  TerminalVtOperation operation;
  operation.kind = TerminalVtOperationKind::Escape;
  operation.escape_final = final_byte;
  return operation;
}

TerminalVtOperation TerminalVtOperation::make_csi(std::string parameters, char final_byte) {
  TerminalVtOperation operation;
  operation.kind = TerminalVtOperationKind::Csi;
  operation.csi.parameters = std::move(parameters);
  operation.csi.final_byte = final_byte;
  return operation;
}

TerminalVtOperation TerminalVtOperation::make_osc(std::string payload) {
  TerminalVtOperation operation;
  operation.kind = TerminalVtOperationKind::Osc;
  operation.osc.payload = std::move(payload);
  return operation;
}

TerminalVtOperation TerminalVtOperation::make_unknown(
    TerminalVtUnknownClass sequence_class,
    std::size_t length,
    char final_byte) {
  TerminalVtOperation operation;
  operation.kind = TerminalVtOperationKind::Unknown;
  operation.unknown.sequence_class = sequence_class;
  operation.unknown.length = length;
  operation.unknown.final_byte = final_byte;
  return operation;
}

void TerminalVtParser::reset() {
  state_ = State::Ground;
  csi_buffer_.clear();
  osc_buffer_.clear();
  discarded_sequence_length_ = 0;
  utf8_remaining_ = 0;
  utf8_codepoint_ = 0;
}

void TerminalVtParser::feed(
    std::string_view bytes,
    std::vector<TerminalVtOperation>& operations) {
  operations.reserve(operations.size() + bytes.size());

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto byte = static_cast<unsigned char>(bytes[index]);
    bool reprocess = true;
    while (reprocess) {
      reprocess = false;

      switch (state_) {
        case State::Ground:
          if (utf8_remaining_ > 0) {
            reprocess = !feed_utf8_byte(byte, operations);
          } else if (byte == 0x1b) {
            state_ = State::Escape;
          } else if (byte == '\r') {
            operations.push_back(TerminalVtOperation::control(
                TerminalVtOperationKind::CarriageReturn));
          } else if (byte == '\n') {
            operations.push_back(
                TerminalVtOperation::control(TerminalVtOperationKind::LineFeed));
          } else if (byte == '\b') {
            operations.push_back(
                TerminalVtOperation::control(TerminalVtOperationKind::Backspace));
          } else if (byte == '\t') {
            operations.push_back(TerminalVtOperation::control(TerminalVtOperationKind::Tab));
          } else if (byte >= 0x20 && byte < 0x80 && byte != 0x7f) {
            operations.push_back(TerminalVtOperation::print(byte));
          } else if (byte >= 0xc2 && byte <= 0xdf) {
            utf8_codepoint_ = byte & 0x1f;
            utf8_remaining_ = 1;
          } else if (byte >= 0xe0 && byte <= 0xef) {
            utf8_codepoint_ = byte & 0x0f;
            utf8_remaining_ = 2;
          } else if (byte >= 0xf0 && byte <= 0xf4) {
            utf8_codepoint_ = byte & 0x07;
            utf8_remaining_ = 3;
          } else if (byte >= 0x80) {
            operations.push_back(
                TerminalVtOperation::make_unknown(TerminalVtUnknownClass::Utf8, 1));
            operations.push_back(TerminalVtOperation::print(kReplacementCodepoint));
          }
          break;

        case State::Escape:
          if (byte == '[') {
            csi_buffer_.clear();
            state_ = State::Csi;
          } else if (byte == ']') {
            osc_buffer_.clear();
            state_ = State::Osc;
          } else {
            operations.push_back(TerminalVtOperation::escape(static_cast<char>(byte)));
            enter_ground();
          }
          break;

        case State::Csi:
          if (is_csi_final_byte(byte)) {
            operations.push_back(
                TerminalVtOperation::make_csi(std::move(csi_buffer_), static_cast<char>(byte)));
            csi_buffer_.clear();
            enter_ground();
          } else if (csi_buffer_.size() >= kMaxCsiLength) {
            discarded_sequence_length_ = csi_buffer_.size() + 1;
            csi_buffer_.clear();
            state_ = State::CsiDiscard;
          } else {
            csi_buffer_.push_back(static_cast<char>(byte));
          }
          break;

        case State::CsiDiscard:
          ++discarded_sequence_length_;
          if (is_csi_final_byte(byte)) {
            operations.push_back(TerminalVtOperation::make_unknown(
                TerminalVtUnknownClass::Csi,
                discarded_sequence_length_,
                static_cast<char>(byte)));
            enter_ground();
          }
          break;

        case State::Osc:
          if (byte == '\a') {
            operations.push_back(TerminalVtOperation::make_osc(std::move(osc_buffer_)));
            osc_buffer_.clear();
            enter_ground();
          } else if (byte == 0x1b) {
            state_ = State::OscEscape;
          } else if (osc_buffer_.size() >= kMaxOscLength) {
            discarded_sequence_length_ = osc_buffer_.size() + 1;
            osc_buffer_.clear();
            state_ = State::OscDiscard;
          } else {
            osc_buffer_.push_back(static_cast<char>(byte));
          }
          break;

        case State::OscEscape:
          if (byte == '\\') {
            operations.push_back(TerminalVtOperation::make_osc(std::move(osc_buffer_)));
            osc_buffer_.clear();
            enter_ground();
          } else {
            if (osc_buffer_.size() + 1 >= kMaxOscLength) {
              discarded_sequence_length_ = osc_buffer_.size() + 2;
              osc_buffer_.clear();
              state_ = State::OscDiscard;
            } else {
              osc_buffer_.push_back('\x1b');
              osc_buffer_.push_back(static_cast<char>(byte));
              state_ = State::Osc;
            }
          }
          break;

        case State::OscDiscard:
          ++discarded_sequence_length_;
          if (byte == '\a') {
            operations.push_back(
                TerminalVtOperation::make_unknown(
                    TerminalVtUnknownClass::Osc,
                    discarded_sequence_length_));
            enter_ground();
          } else if (byte == 0x1b) {
            state_ = State::OscDiscardEscape;
          }
          break;

        case State::OscDiscardEscape:
          ++discarded_sequence_length_;
          if (byte == '\\') {
            operations.push_back(
                TerminalVtOperation::make_unknown(
                    TerminalVtUnknownClass::Osc,
                    discarded_sequence_length_));
            enter_ground();
          } else {
            state_ = State::OscDiscard;
          }
          break;
      }
    }
  }
}

void TerminalVtParser::emit_pending_utf8_replacement(
    std::vector<TerminalVtOperation>& operations) {
  if (utf8_remaining_ == 0) {
    return;
  }

  operations.push_back(TerminalVtOperation::make_unknown(TerminalVtUnknownClass::Utf8, 1));
  operations.push_back(TerminalVtOperation::print(kReplacementCodepoint));
  utf8_remaining_ = 0;
  utf8_codepoint_ = 0;
}

bool TerminalVtParser::feed_utf8_byte(
    unsigned char byte,
    std::vector<TerminalVtOperation>& operations) {
  if (is_utf8_continuation(byte)) {
    utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3f);
    --utf8_remaining_;
    if (utf8_remaining_ == 0) {
      operations.push_back(TerminalVtOperation::print(utf8_codepoint_));
      utf8_codepoint_ = 0;
    }
    return true;
  }

  emit_pending_utf8_replacement(operations);
  return false;
}

void TerminalVtParser::enter_ground() {
  state_ = State::Ground;
  discarded_sequence_length_ = 0;
  utf8_remaining_ = 0;
  utf8_codepoint_ = 0;
}

std::string terminal_vt_unknown_class_name(TerminalVtUnknownClass sequence_class) {
  switch (sequence_class) {
    case TerminalVtUnknownClass::Escape:
      return "escape";
    case TerminalVtUnknownClass::Csi:
      return "csi";
    case TerminalVtUnknownClass::Osc:
      return "osc";
    case TerminalVtUnknownClass::Utf8:
      return "utf8";
  }

  return "unknown";
}

}  // namespace wmux
