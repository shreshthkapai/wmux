#include "wmux/windows_clipboard.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

#ifdef _WIN32

class ClipboardSession {
 public:
  ClipboardSession() = default;
  ClipboardSession(const ClipboardSession&) = delete;
  ClipboardSession& operator=(const ClipboardSession&) = delete;

  ~ClipboardSession() {
    if (open_) {
      CloseClipboard();
    }
  }

  bool open() {
    open_ = OpenClipboard(nullptr) != 0;
    return open_;
  }

 private:
  bool open_{false};
};

std::optional<std::wstring> read_clipboard_text() {
  ClipboardSession clipboard;
  if (!clipboard.open()) {
    return std::nullopt;
  }

  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data == nullptr) {
    return std::nullopt;
  }

  const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
  if (text == nullptr) {
    return std::nullopt;
  }

  std::wstring value{text};
  GlobalUnlock(data);
  return value;
}

std::string utf8_from_wide(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }

  const auto input_size = static_cast<int>(text.size());
  const int required = WideCharToMultiByte(
      CP_UTF8, 0, text.data(), input_size, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }

  std::string utf8(static_cast<std::size_t>(required), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8, 0, text.data(), input_size, utf8.data(), required, nullptr, nullptr);
  if (written <= 0) {
    return {};
  }
  utf8.resize(static_cast<std::size_t>(written));
  return utf8;
}

bool clipboard_smoke_enabled() {
  char value[2]{};
  const DWORD size = GetEnvironmentVariableA("WMUX_TEST_CLIPBOARD", value, sizeof(value));
  return size == 1 && value[0] == '1';
}

void optionally_round_trips_windows_clipboard_text() {
  if (!clipboard_smoke_enabled()) {
    return;
  }

  const auto previous = read_clipboard_text();
  const std::string probe{"wmux clipboard smoke\r\nlambda \xce\xbb"};
  const auto result = wmux::write_windows_clipboard_text(probe);
  const auto actual = read_clipboard_text();

  if (previous) {
    (void)wmux::write_windows_clipboard_text(utf8_from_wide(*previous));
  }

  assert(result.ok);
  assert(actual);
  assert(*actual == L"wmux clipboard smoke\r\nlambda \x03bb");
}

#else

void optionally_round_trips_windows_clipboard_text() {}

#endif

}  // namespace

void run_windows_clipboard_tests() {
  optionally_round_trips_windows_clipboard_text();
}
