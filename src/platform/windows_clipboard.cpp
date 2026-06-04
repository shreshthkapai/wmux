#include "wmux/windows_clipboard.hpp"

#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wmux {
namespace {

#ifdef _WIN32

std::string last_error_message(std::string_view operation) {
  return std::string{operation} + " failed with error " + std::to_string(GetLastError());
}

std::wstring widen_with_code_page(std::string_view text, UINT code_page, DWORD flags) {
  if (text.empty()) {
    return {};
  }

  const auto input_size = static_cast<int>(text.size());
  const int required =
      MultiByteToWideChar(code_page, flags, text.data(), input_size, nullptr, 0);
  if (required <= 0) {
    return {};
  }

  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  const int written = MultiByteToWideChar(
      code_page, flags, text.data(), input_size, wide.data(), required);
  if (written <= 0) {
    return {};
  }
  wide.resize(static_cast<std::size_t>(written));
  return wide;
}

std::wstring widen_clipboard_text(std::string_view text) {
  auto wide = widen_with_code_page(text, CP_UTF8, MB_ERR_INVALID_CHARS);
  if (!wide.empty() || text.empty()) {
    return wide;
  }

  wide = widen_with_code_page(text, CP_ACP, 0);
  if (!wide.empty()) {
    return wide;
  }

  wide.reserve(text.size());
  for (const unsigned char byte : text) {
    wide.push_back(static_cast<wchar_t>(byte));
  }
  return wide;
}

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
    for (int attempt = 0; attempt < 8; ++attempt) {
      open_ = OpenClipboard(nullptr) != 0;
      if (open_) {
        return true;
      }
      Sleep(5);
    }
    return open_;
  }

 private:
  bool open_{false};
};

class MovableGlobalMemory {
 public:
  explicit MovableGlobalMemory(SIZE_T bytes)
      : handle_{GlobalAlloc(GMEM_MOVEABLE, bytes)} {}

  MovableGlobalMemory(const MovableGlobalMemory&) = delete;
  MovableGlobalMemory& operator=(const MovableGlobalMemory&) = delete;

  ~MovableGlobalMemory() {
    if (handle_ != nullptr) {
      GlobalFree(handle_);
    }
  }

  HGLOBAL get() const {
    return handle_;
  }

  HGLOBAL release() {
    const auto released = handle_;
    handle_ = nullptr;
    return released;
  }

 private:
  HGLOBAL handle_{nullptr};
};

#endif

}  // namespace

ClipboardWriteResult write_windows_clipboard_text(std::string_view text) {
#ifdef _WIN32
  auto wide = widen_clipboard_text(text);
  wide.push_back(L'\0');

  const auto byte_count = static_cast<SIZE_T>(wide.size() * sizeof(wchar_t));
  MovableGlobalMemory memory{byte_count};
  if (memory.get() == nullptr) {
    return ClipboardWriteResult{false, last_error_message("GlobalAlloc")};
  }

  void* locked = GlobalLock(memory.get());
  if (locked == nullptr) {
    return ClipboardWriteResult{false, last_error_message("GlobalLock")};
  }
  std::memcpy(locked, wide.data(), byte_count);
  GlobalUnlock(memory.get());

  ClipboardSession clipboard;
  if (!clipboard.open()) {
    return ClipboardWriteResult{false, last_error_message("OpenClipboard")};
  }

  if (EmptyClipboard() == 0) {
    return ClipboardWriteResult{false, last_error_message("EmptyClipboard")};
  }

  if (SetClipboardData(CF_UNICODETEXT, memory.get()) == nullptr) {
    return ClipboardWriteResult{false, last_error_message("SetClipboardData")};
  }
  memory.release();
  return ClipboardWriteResult{true, {}};
#else
  (void)text;
  return ClipboardWriteResult{false, "Windows clipboard is not available on this platform"};
#endif
}

}  // namespace wmux
