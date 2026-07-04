#include "terminal_engine_v2_internal.hpp"

#include <algorithm>

namespace wmux::terminal_engine_v2 {
namespace {

constexpr std::uint32_t kReplacementCodepoint = 0xfffd;
constexpr std::size_t kMaxOscPayload = 4096;

bool is_printable_ascii(unsigned char byte) {
  return byte >= 0x20 && byte < 0x7f;
}

bool is_utf8_continuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

bool is_csi_final(unsigned char byte) {
  return byte >= 0x40 && byte <= 0x7e;
}

bool is_escape_final(unsigned char byte) {
  return byte >= 0x30 && byte <= 0x7e;
}

bool is_cancel_control(unsigned char byte) {
  return byte == 0x18 || byte == 0x1a;
}

}  // namespace

void VtParser::parse(std::span<const std::byte> bytes, ScreenWriter& writer) {
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  std::size_t index = 0;

  while (index < bytes.size()) {
    const unsigned char byte = data[index];
    ++sequence_length_;

    switch (state_) {
      case State::Ground: {
        if (utf8_remaining_ > 0) {
          if (feed_utf8_byte(byte, writer)) {
            ++index;
          }
          break;
        }

        if (is_printable_ascii(byte)) {
          const auto start = index;
          ++index;
          while (index < bytes.size() && is_printable_ascii(data[index])) {
            ++index;
          }
          writer.print_ascii_span(
              std::string_view{reinterpret_cast<const char*>(data + start), index - start});
          sequence_length_ = 0;
          break;
        }

        if (byte == 0x1b) {
          state_ = State::Escape;
          sequence_length_ = 1;
          ++index;
          break;
        }

        if (byte == 0x9b) {
          start_csi();
          ++index;
          break;
        }

        if (byte == 0x9d) {
          reset_osc();
          state_ = State::Osc;
          ++index;
          break;
        }

        if (byte < 0x20 || byte == 0x7f) {
          writer.execute_control(byte);
          sequence_length_ = 0;
          ++index;
          break;
        }

        if ((byte & 0xe0) == 0xc0) {
          utf8_codepoint_ = byte & 0x1f;
          utf8_remaining_ = 1;
          utf8_bytes_.assign(1, static_cast<char>(byte));
        } else if ((byte & 0xf0) == 0xe0) {
          utf8_codepoint_ = byte & 0x0f;
          utf8_remaining_ = 2;
          utf8_bytes_.assign(1, static_cast<char>(byte));
        } else if ((byte & 0xf8) == 0xf0) {
          utf8_codepoint_ = byte & 0x07;
          utf8_remaining_ = 3;
          utf8_bytes_.assign(1, static_cast<char>(byte));
        } else {
          writer.unknown(UnknownSequenceClass::Utf8, 1);
          writer.print_utf8(kReplacementCodepoint, "?");
          reset_utf8();
        }
        ++index;
        break;
      }

      case State::Escape:
        if (is_cancel_control(byte)) {
          writer.unknown(UnknownSequenceClass::Escape, sequence_length_, static_cast<char>(byte));
          enter_ground();
          ++index;
          break;
        }
        if (byte == '[') {
          start_csi();
          ++index;
          break;
        }
        if (byte == ']') {
          reset_osc();
          state_ = State::Osc;
          ++index;
          break;
        }
        if (byte == '(') {
          state_ = State::CharsetG0;
          ++index;
          break;
        }
        if (byte == ')') {
          state_ = State::CharsetG1;
          ++index;
          break;
        }
        if (is_escape_final(byte)) {
          writer.dispatch_escape(static_cast<char>(byte));
          enter_ground();
        } else {
          writer.unknown(UnknownSequenceClass::Escape, sequence_length_, static_cast<char>(byte));
          enter_ground();
        }
        ++index;
        break;

      case State::CharsetG0:
        writer.designate_character_set(0, static_cast<char>(byte));
        enter_ground();
        ++index;
        break;

      case State::CharsetG1:
        writer.designate_character_set(1, static_cast<char>(byte));
        enter_ground();
        ++index;
        break;

      case State::Csi:
        if (is_cancel_control(byte)) {
          writer.unknown(UnknownSequenceClass::Csi, sequence_length_, static_cast<char>(byte));
          enter_ground();
          ++index;
          break;
        }
        if (byte == 0x1b) {
          writer.unknown(UnknownSequenceClass::Csi, sequence_length_, static_cast<char>(byte));
          state_ = State::Escape;
          sequence_length_ = 1;
          ++index;
          break;
        }
        if (byte == '?' && csi_params_.count == 0 && !csi_have_value_) {
          csi_params_.private_mode = true;
        } else if (byte >= '0' && byte <= '9') {
          csi_have_value_ = true;
          csi_value_ = std::min(999999, (csi_value_ * 10) + (byte - '0'));
        } else if (byte == ';' || byte == ':') {
          push_csi_param();
        } else if (is_csi_final(byte)) {
          push_csi_param();
          writer.dispatch_csi(csi_params_, static_cast<char>(byte));
          enter_ground();
        } else if (byte >= 0x20 && byte <= 0x2f) {
          // CSI intermediates are rare in wmux's current supported set. Keep
          // them inside the sequence, then either dispatch on a final byte or
          // mark unknown if it never resolves.
        } else {
          state_ = State::CsiIgnore;
        }
        ++index;
        break;

      case State::CsiIgnore:
        if (is_cancel_control(byte)) {
          writer.unknown(UnknownSequenceClass::Csi, sequence_length_, static_cast<char>(byte));
          enter_ground();
          ++index;
          break;
        }
        if (byte == 0x1b) {
          writer.unknown(UnknownSequenceClass::Csi, sequence_length_, static_cast<char>(byte));
          state_ = State::Escape;
          sequence_length_ = 1;
          ++index;
          break;
        }
        if (is_csi_final(byte)) {
          writer.unknown(UnknownSequenceClass::Csi, sequence_length_, static_cast<char>(byte));
          enter_ground();
        }
        ++index;
        break;

      case State::Osc:
        if (is_cancel_control(byte)) {
          writer.unknown(UnknownSequenceClass::Osc, sequence_length_, static_cast<char>(byte));
          enter_ground();
        } else if (byte == 0x07) {
          writer.dispatch_osc(osc_buffer_);
          enter_ground();
        } else if (byte == 0x1b) {
          state_ = State::OscEscape;
        } else if (osc_buffer_.size() < kMaxOscPayload) {
          osc_buffer_.push_back(static_cast<char>(byte));
        } else {
          state_ = State::OscDiscard;
        }
        ++index;
        break;

      case State::OscEscape:
        if (byte == '\\') {
          writer.dispatch_osc(osc_buffer_);
          enter_ground();
        } else {
          if (osc_buffer_.size() < kMaxOscPayload) {
            osc_buffer_.push_back('\x1b');
            osc_buffer_.push_back(static_cast<char>(byte));
            state_ = State::Osc;
          } else {
            state_ = State::OscDiscard;
          }
        }
        ++index;
        break;

      case State::OscDiscard:
        if (byte == 0x07) {
          writer.unknown(UnknownSequenceClass::Osc, sequence_length_);
          enter_ground();
        } else if (byte == 0x1b) {
          state_ = State::OscDiscardEscape;
        }
        ++index;
        break;

      case State::OscDiscardEscape:
        if (byte == '\\') {
          writer.unknown(UnknownSequenceClass::Osc, sequence_length_);
          enter_ground();
        } else {
          state_ = State::OscDiscard;
        }
        ++index;
        break;
    }
  }

  writer.flush_print_run();
}

void VtParser::reset_utf8() {
  utf8_remaining_ = 0;
  utf8_codepoint_ = 0;
  utf8_bytes_.clear();
}

bool VtParser::feed_utf8_byte(unsigned char byte, ScreenWriter& writer) {
  if (!is_utf8_continuation(byte)) {
    emit_utf8_replacement(writer);
    return false;
  }
  utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3f);
  utf8_bytes_.push_back(static_cast<char>(byte));
  --utf8_remaining_;
  if (utf8_remaining_ == 0) {
    writer.print_utf8(utf8_codepoint_, utf8_bytes_);
    reset_utf8();
  }
  return true;
}

void VtParser::emit_utf8_replacement(ScreenWriter& writer) {
  if (utf8_remaining_ == 0) {
    return;
  }
  writer.unknown(UnknownSequenceClass::Utf8, utf8_bytes_.size());
  writer.print_utf8(kReplacementCodepoint, "?");
  reset_utf8();
}

void VtParser::start_csi() {
  csi_params_ = {};
  csi_value_ = 0;
  csi_have_value_ = false;
  state_ = State::Csi;
  sequence_length_ = 2;
}

void VtParser::push_csi_param() {
  csi_params_.push(csi_have_value_ ? csi_value_ : 0);
  csi_value_ = 0;
  csi_have_value_ = false;
}

void VtParser::reset_osc() {
  osc_buffer_.clear();
  sequence_length_ = 2;
}

void VtParser::enter_ground() {
  state_ = State::Ground;
  sequence_length_ = 0;
  csi_params_ = {};
  csi_value_ = 0;
  csi_have_value_ = false;
  osc_buffer_.clear();
}

}  // namespace wmux::terminal_engine_v2
