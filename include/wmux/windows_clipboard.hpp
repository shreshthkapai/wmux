#pragma once

#include <string>
#include <string_view>

namespace wmux {

struct ClipboardWriteResult {
  bool ok{false};
  std::string error;
};

ClipboardWriteResult write_windows_clipboard_text(std::string_view text);

}  // namespace wmux
