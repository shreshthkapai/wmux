#include "wmux/pty_process.hpp"

#include "wmux/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace wmux {
namespace {

constexpr std::size_t kMaxBufferedOutputBytes = 4 * 1024 * 1024;

#ifdef _WIN32

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~UniqueHandle() {
    reset();
  }

  HANDLE get() const {
    return handle_;
  }

  HANDLE release() {
    const auto handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  void reset(HANDLE handle = nullptr) {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

  bool valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{nullptr};
};

class UniquePseudoConsole {
 public:
  UniquePseudoConsole() = default;
  explicit UniquePseudoConsole(HPCON console) : console_(console) {}
  UniquePseudoConsole(const UniquePseudoConsole&) = delete;
  UniquePseudoConsole& operator=(const UniquePseudoConsole&) = delete;

  UniquePseudoConsole(UniquePseudoConsole&& other) noexcept : console_(other.release()) {}

  UniquePseudoConsole& operator=(UniquePseudoConsole&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~UniquePseudoConsole() {
    reset();
  }

  HPCON get() const {
    return console_;
  }

  HPCON release() {
    const auto console = console_;
    console_ = nullptr;
    return console;
  }

  void reset(HPCON console = nullptr) {
    if (console_ != nullptr) {
      ClosePseudoConsole(console_);
    }
    console_ = console;
  }

  bool valid() const {
    return console_ != nullptr;
  }

 private:
  HPCON console_{nullptr};
};

std::wstring widen(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int required = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
  return wide;
}

std::string narrow(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  const int required = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string narrow_value(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      narrow_value.data(),
      required,
      nullptr,
      nullptr);
  return narrow_value;
}

std::string windows_error_message(std::string_view prefix, DWORD error_code) {
  wchar_t* raw_message = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD size = FormatMessageW(
      flags,
      nullptr,
      error_code,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&raw_message),
      0,
      nullptr);

  std::string message{prefix};
  message += " (";
  message += std::to_string(error_code);
  message += ")";
  if (size > 0 && raw_message != nullptr) {
    message += ": ";
    message += narrow(std::wstring_view{raw_message, size});
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
      message.pop_back();
    }
    LocalFree(raw_message);
  }
  message += "\n";
  return message;
}

struct AttributeList {
  explicit AttributeList(SIZE_T bytes) : storage(bytes) {
    list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  }

  ~AttributeList() {
    if (initialized) {
      DeleteProcThreadAttributeList(list);
    }
  }

  AttributeList(const AttributeList&) = delete;
  AttributeList& operator=(const AttributeList&) = delete;

  std::vector<std::byte> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST list{nullptr};
  bool initialized{false};
};

#endif

struct StoredOutputChunk {
  std::uint64_t sequence{0};
  std::string bytes;
};

}  // namespace

struct PtyProcess::Impl {
#ifdef _WIN32
  UniqueHandle input_read;
  UniqueHandle input_write;
  UniqueHandle output_read;
  UniqueHandle job;
  UniqueHandle process;
  UniquePseudoConsole console;
  std::uint32_t process_id{0};
#endif

  mutable std::mutex output_mutex;
  mutable std::condition_variable output_cv;
  std::deque<StoredOutputChunk> output_chunks;
  TerminalGrid screen;
  std::uint64_t next_sequence{1};
  std::size_t buffered_bytes{0};
  bool reader_done{false};

  mutable std::mutex write_mutex;
  mutable std::mutex console_mutex;
  std::thread reader_thread;
  std::atomic_bool terminating{false};

  ~Impl() {
    terminate();
  }

  void append_output(std::string bytes) {
    if (bytes.empty()) {
      return;
    }

    std::lock_guard lock(output_mutex);
    screen.feed(bytes);
    buffered_bytes += bytes.size();
    output_chunks.push_back(StoredOutputChunk{next_sequence++, std::move(bytes)});

    while (buffered_bytes > kMaxBufferedOutputBytes && !output_chunks.empty()) {
      buffered_bytes -= output_chunks.front().bytes.size();
      output_chunks.pop_front();
    }

    output_cv.notify_all();
  }

  void mark_reader_done() {
    std::lock_guard lock(output_mutex);
    reader_done = true;
    output_cv.notify_all();
  }

  PtyOutputSnapshot snapshot() const {
    std::lock_guard lock(output_mutex);

    PtyOutputSnapshot snapshot;
    snapshot.next_sequence = next_sequence;
    snapshot.alive = !reader_done;
    snapshot.screen = screen.snapshot();
    snapshot.scrollback = screen.scrollback_snapshot();
    snapshot.bytes.reserve(buffered_bytes);
    for (const auto& chunk : output_chunks) {
      snapshot.bytes += chunk.bytes;
    }

    return snapshot;
  }

  PtyOutputChunk wait_for_output(
      std::uint64_t requested_sequence,
      std::chrono::milliseconds timeout) const {
    std::unique_lock lock(output_mutex);
    output_cv.wait_for(lock, timeout, [&] {
      return reader_done || std::ranges::any_of(output_chunks, [&](const auto& chunk) {
               return chunk.sequence >= requested_sequence;
             });
    });

    PtyOutputChunk output;
    output.next_sequence = requested_sequence;
    output.alive = !reader_done;

    const auto it = std::ranges::find_if(output_chunks, [&](const auto& chunk) {
      return chunk.sequence >= requested_sequence;
    });
    if (it != output_chunks.end()) {
      output.bytes = it->bytes;
      output.next_sequence = it->sequence + 1;
    }

    return output;
  }

#ifdef _WIN32
  void start_reader() {
    reader_thread = std::thread([this] { read_loop(); });
  }

  void read_loop() {
    char buffer[4096];
    while (!terminating && output_read.valid()) {
      DWORD bytes_read = 0;
      const BOOL ok = ReadFile(
          output_read.get(),
          buffer,
          static_cast<DWORD>(sizeof(buffer)),
          &bytes_read,
          nullptr);
      if (!ok || bytes_read == 0) {
        break;
      }

      append_output(std::string{buffer, buffer + bytes_read});
    }

    log_event(
        LogLevel::Info,
        "pty",
        "reader_exit",
        {{"process_id", std::to_string(process_id)}});
    mark_reader_done();
  }

  bool write(std::string_view bytes) {
    std::lock_guard lock(write_mutex);
    while (!bytes.empty()) {
      if (!input_write.valid()) {
        return false;
      }

      const auto bytes_to_write =
          static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
      DWORD bytes_written = 0;
      const BOOL ok =
          WriteFile(input_write.get(), bytes.data(), bytes_to_write, &bytes_written, nullptr);
      if (!ok || bytes_written == 0) {
        return false;
      }

      bytes.remove_prefix(bytes_written);
    }

    return true;
  }

  bool resize(short columns, short rows) {
    if (columns <= 0 || rows <= 0) {
      return false;
    }

    std::lock_guard lock(console_mutex);
    if (terminating.load() || !console.valid()) {
      return false;
    }

    const COORD size{columns, rows};
    if (!SUCCEEDED(ResizePseudoConsole(console.get(), size))) {
      log_event(
          LogLevel::Warn,
          "pty",
          "resize_failed",
          {{"process_id", std::to_string(process_id)},
           {"columns", std::to_string(columns)},
           {"rows", std::to_string(rows)}});
      return false;
    }

    {
      std::lock_guard output_lock(output_mutex);
      screen.resize(columns, rows);
    }
    log_event(
        LogLevel::Info,
        "pty",
        "resize",
        {{"process_id", std::to_string(process_id)},
         {"columns", std::to_string(columns)},
         {"rows", std::to_string(rows)}});
    return true;
  }

  void terminate() {
    bool expected = false;
    if (!terminating.compare_exchange_strong(expected, true)) {
      return;
    }

    {
      std::lock_guard write_lock(write_mutex);
      input_write.reset();
    }
    {
      std::lock_guard lock(console_mutex);
      console.reset();
    }
    job.reset();

    if (reader_thread.joinable()) {
      CancelSynchronousIo(reader_thread.native_handle());
    }

    if (process.valid()) {
      const DWORD wait_result = WaitForSingleObject(process.get(), 500);
      if (wait_result == WAIT_TIMEOUT) {
        log_event(
            LogLevel::Warn,
            "pty",
            "terminate_process",
            {{"process_id", std::to_string(process_id)}});
        TerminateProcess(process.get(), 0);
        WaitForSingleObject(process.get(), 1000);
      }
    }

    if (reader_thread.joinable()) {
      reader_thread.join();
    }

    mark_reader_done();
    input_read.reset();
    output_read.reset();
    process.reset();
    log_event(
        LogLevel::Info,
        "pty",
        "terminated",
        {{"process_id", std::to_string(process_id)}});
  }
#else
  bool write(std::string_view) {
    return false;
  }

  bool resize(short, short) {
    return false;
  }

  void terminate() {
    bool expected = false;
    if (!terminating.compare_exchange_strong(expected, true)) {
      return;
    }
    mark_reader_done();
  }
#endif
};

PtyProcess::PtyProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PtyProcess::~PtyProcess() = default;

PtyProcessResult PtyProcess::start(std::string_view command_line, short columns, short rows) {
#ifdef _WIN32
  UniqueHandle input_read;
  UniqueHandle input_write;
  HANDLE raw_input_read = nullptr;
  HANDLE raw_input_write = nullptr;
  if (!CreatePipe(&raw_input_read, &raw_input_write, nullptr, 0)) {
    return {
        nullptr,
        windows_error_message("wmux: failed to create ConPTY input pipe", GetLastError())};
  }
  input_read.reset(raw_input_read);
  input_write.reset(raw_input_write);

  UniqueHandle output_read;
  UniqueHandle output_write;
  HANDLE raw_output_read = nullptr;
  HANDLE raw_output_write = nullptr;
  if (!CreatePipe(&raw_output_read, &raw_output_write, nullptr, 0)) {
    return {
        nullptr,
        windows_error_message("wmux: failed to create ConPTY output pipe", GetLastError())};
  }
  output_read.reset(raw_output_read);
  output_write.reset(raw_output_write);

  HPCON raw_console = nullptr;
  const COORD size{columns, rows};
  const HRESULT console_result =
      CreatePseudoConsole(size, input_read.get(), output_write.get(), 0, &raw_console);
  if (FAILED(console_result)) {
    return {
        nullptr,
        "wmux: failed to create ConPTY pseudoconsole (" +
            std::to_string(static_cast<unsigned long>(console_result)) + ")\n"};
  }
  UniquePseudoConsole console{raw_console};
  output_write.reset();

  UniqueHandle job{CreateJobObjectW(nullptr, nullptr)};
  if (!job.valid()) {
    return {
        nullptr, windows_error_message("wmux: failed to create shell job object", GetLastError())};
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
  job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(
          job.get(),
          JobObjectExtendedLimitInformation,
          &job_limits,
          sizeof(job_limits))) {
    return {
        nullptr,
        windows_error_message("wmux: failed to configure shell job object", GetLastError())};
  }

  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  if (attribute_size == 0) {
    return {
        nullptr,
        windows_error_message("wmux: failed to size process attribute list", GetLastError())};
  }

  AttributeList attributes{attribute_size};
  if (!InitializeProcThreadAttributeList(attributes.list, 1, 0, &attribute_size)) {
    return {
        nullptr,
        windows_error_message("wmux: failed to initialize process attribute list", GetLastError())};
  }
  attributes.initialized = true;

  if (!UpdateProcThreadAttribute(
          attributes.list,
          0,
          PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
          console.get(),
          sizeof(HPCON),
          nullptr,
          nullptr)) {
    return {
        nullptr,
        windows_error_message("wmux: failed to attach ConPTY to process", GetLastError())};
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes.list;

  auto command = widen(command_line);
  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(
      nullptr,
      command.data(),
      nullptr,
      nullptr,
      TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
      nullptr,
      nullptr,
      &startup.StartupInfo,
      &process_info);
  if (!created) {
    return {
        nullptr,
        windows_error_message("wmux: failed to start shell process", GetLastError())};
  }

  UniqueHandle process{process_info.hProcess};
  UniqueHandle thread{process_info.hThread};
  if (!AssignProcessToJobObject(job.get(), process.get())) {
    TerminateProcess(process.get(), 1);
    return {
        nullptr,
        windows_error_message("wmux: failed to assign shell process to job object", GetLastError())};
  }

  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
    TerminateProcess(process.get(), 1);
    return {
        nullptr, windows_error_message("wmux: failed to resume shell process", GetLastError())};
  }

  auto impl = std::make_unique<Impl>();
  impl->input_read = std::move(input_read);
  impl->input_write = std::move(input_write);
  impl->output_read = std::move(output_read);
  impl->job = std::move(job);
  impl->process = std::move(process);
  impl->process_id = process_info.dwProcessId;
  impl->console = std::move(console);
  impl->screen.resize(columns, rows);

  auto pty = std::shared_ptr<PtyProcess>(new PtyProcess(std::move(impl)));
  pty->impl_->start_reader();
  log_event(
      LogLevel::Info,
      "pty",
      "spawn",
      {{"process_id", std::to_string(process_info.dwProcessId)},
       {"columns", std::to_string(columns)},
       {"rows", std::to_string(rows)}});
  return {pty, {}};
#else
  (void)command_line;
  (void)columns;
  (void)rows;
  return {nullptr, "wmux: ConPTY shell processes are only supported on Windows\n"};
#endif
}

bool PtyProcess::write_input(std::string_view bytes) {
  return impl_->write(bytes);
}

bool PtyProcess::resize(short columns, short rows) {
  return impl_->resize(columns, rows);
}

std::uint32_t PtyProcess::process_id() const {
#ifdef _WIN32
  return impl_->process_id;
#else
  return 0;
#endif
}

PtyOutputSnapshot PtyProcess::output_snapshot() const {
  return impl_->snapshot();
}

PtyOutputChunk PtyProcess::wait_for_output(
    std::uint64_t next_sequence,
    std::chrono::milliseconds timeout) const {
  return impl_->wait_for_output(next_sequence, timeout);
}

void PtyProcess::terminate() {
  impl_->terminate();
}

}  // namespace wmux
