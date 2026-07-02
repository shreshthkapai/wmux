#include "wmux/platform/pty_process.hpp"

#include "wmux/logging.hpp"
#include "wmux/resource_limits.hpp"

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
#include <tlhelp32.h>
#endif

namespace wmux {
namespace {

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

#ifdef _WIN32
struct ChildProcessEntry {
  DWORD process_id{0};
  DWORD parent_process_id{0};
};

std::vector<ChildProcessEntry> process_snapshot_entries() {
  std::vector<ChildProcessEntry> entries;
  UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
  if (!snapshot.valid()) {
    log_event(
        LogLevel::Warn,
        "pty",
        "process_snapshot_failed",
        {{"error", std::to_string(GetLastError())}});
    return entries;
  }

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) {
    log_event(
        LogLevel::Warn,
        "pty",
        "process_snapshot_first_failed",
        {{"error", std::to_string(GetLastError())}});
    return entries;
  }

  do {
    entries.push_back(ChildProcessEntry{entry.th32ProcessID, entry.th32ParentProcessID});
    entry.dwSize = sizeof(entry);
  } while (Process32NextW(snapshot.get(), &entry));

  return entries;
}

std::vector<DWORD> descendant_process_ids(DWORD root_process_id) {
  const auto entries = process_snapshot_entries();
  std::vector<DWORD> descendants;
  std::vector<DWORD> frontier{root_process_id};

  for (std::size_t index = 0; index < frontier.size(); ++index) {
    const auto parent = frontier[index];
    for (const auto& entry : entries) {
      if (entry.parent_process_id != parent || entry.process_id == root_process_id) {
        continue;
      }
      if (std::find(descendants.begin(), descendants.end(), entry.process_id) !=
          descendants.end()) {
        continue;
      }
      descendants.push_back(entry.process_id);
      frontier.push_back(entry.process_id);
    }
  }

  return descendants;
}

std::size_t terminate_descendant_processes(DWORD root_process_id) {
  auto descendants = descendant_process_ids(root_process_id);
  if (descendants.empty()) {
    return 0;
  }

  log_event(
      LogLevel::Info,
      "pty",
      "terminate_descendants_start",
      {{"process_id", std::to_string(root_process_id)},
       {"descendants", std::to_string(descendants.size())}});

  std::size_t terminated = 0;
  for (auto it = descendants.rbegin(); it != descendants.rend(); ++it) {
    UniqueHandle child{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, *it)};
    if (!child.valid()) {
      const auto error = GetLastError();
      if (error != ERROR_INVALID_PARAMETER) {
        log_event(
            LogLevel::Warn,
            "pty",
            "open_descendant_failed",
            {{"process_id", std::to_string(root_process_id)},
             {"child_process_id", std::to_string(*it)},
             {"error", std::to_string(error)}});
      }
      continue;
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(child.get(), &exit_code) && exit_code != STILL_ACTIVE) {
      continue;
    }

    if (!TerminateProcess(child.get(), 0)) {
      log_event(
          LogLevel::Warn,
          "pty",
          "terminate_descendant_failed",
          {{"process_id", std::to_string(root_process_id)},
           {"child_process_id", std::to_string(*it)},
           {"error", std::to_string(GetLastError())}});
      continue;
    }

    WaitForSingleObject(child.get(), 500);
    ++terminated;
  }

  log_event(
      LogLevel::Info,
      "pty",
      "terminate_descendants_done",
      {{"process_id", std::to_string(root_process_id)},
       {"terminated", std::to_string(terminated)}});
  return terminated;
}
#endif

}  // namespace

struct PtyProcess::Impl {
#ifdef _WIN32
  UniqueHandle input_read;
  UniqueHandle input_write;
  UniqueHandle output_read;
  UniqueHandle job;
  UniqueHandle process;
  UniqueHandle primary_thread;
  UniquePseudoConsole console;
  std::uint32_t process_id{0};
  bool job_configured{false};
  bool job_assigned{false};
  std::atomic_bool pseudo_console_open{false};
  std::atomic_bool input_pipe_read_open{false};
  std::atomic_bool input_pipe_write_open{false};
  std::atomic_bool output_pipe_read_open{false};
  std::atomic_bool process_handle_open{false};
  std::atomic_bool primary_thread_handle_open{false};
  std::atomic_bool job_object_handle_open{false};
#endif

  std::chrono::steady_clock::time_point created_at{std::chrono::steady_clock::now()};
  mutable std::mutex output_mutex;
  mutable std::condition_variable output_cv;
  std::deque<StoredOutputChunk> output_chunks;
  TerminalGrid screen;
  std::size_t max_raw_output_bytes{kMaxPaneRawOutputBytes};
  std::uint64_t next_sequence{1};
  std::uint64_t dropped_raw_chunks{0};
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

    while (buffered_bytes > max_raw_output_bytes && !output_chunks.empty()) {
      buffered_bytes -= output_chunks.front().bytes.size();
      output_chunks.pop_front();
      ++dropped_raw_chunks;
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
    snapshot.buffered_raw_bytes = buffered_bytes;
    snapshot.dropped_raw_chunks = dropped_raw_chunks;
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
    if (it != output_chunks.end() && it->sequence > requested_sequence) {
      output.sequence_compacted = true;
    }
    if (it != output_chunks.end()) {
      output.bytes.reserve(buffered_bytes);
      for (auto chunk = it; chunk != output_chunks.end(); ++chunk) {
        output.bytes += chunk->bytes;
        output.next_sequence = chunk->sequence + 1;
      }
    }

    return output;
  }

#ifdef _WIN32
  void close_handle(UniqueHandle& handle, std::atomic_bool& open_flag, std::string_view name) {
    if (handle.valid()) {
      log_event(
          LogLevel::Debug,
          "pty",
          "close_handle",
          {{"process_id", std::to_string(process_id)}, {"handle", std::string{name}}});
    }
    handle.reset();
    open_flag.store(false, std::memory_order_relaxed);
  }

  void close_console_locked() {
    if (console.valid()) {
      log_event(
          LogLevel::Debug,
          "pty",
          "close_pseudoconsole",
          {{"process_id", std::to_string(process_id)}});
    }
    console.reset();
    pseudo_console_open.store(false, std::memory_order_relaxed);
  }

  void mark_runtime_handles_open() {
    pseudo_console_open.store(console.valid(), std::memory_order_relaxed);
    input_pipe_read_open.store(input_read.valid(), std::memory_order_relaxed);
    input_pipe_write_open.store(input_write.valid(), std::memory_order_relaxed);
    output_pipe_read_open.store(output_read.valid(), std::memory_order_relaxed);
    process_handle_open.store(process.valid(), std::memory_order_relaxed);
    primary_thread_handle_open.store(primary_thread.valid(), std::memory_order_relaxed);
    job_object_handle_open.store(job.valid(), std::memory_order_relaxed);
  }

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
    return write_locked(bytes, 64 * 1024, std::chrono::milliseconds{0});
  }

  bool write_throttled(
      std::string_view bytes,
      std::size_t chunk_bytes,
      std::chrono::milliseconds delay_between_chunks) {
    std::lock_guard lock(write_mutex);
    return write_locked(bytes, chunk_bytes, delay_between_chunks);
  }

  bool write_locked(
      std::string_view bytes,
      std::size_t chunk_bytes,
      std::chrono::milliseconds delay_between_chunks) {
    chunk_bytes = std::max<std::size_t>(1, chunk_bytes);
    while (!bytes.empty()) {
      if (terminating.load(std::memory_order_acquire) || !input_write.valid()) {
        return false;
      }

      const auto bytes_to_write =
          static_cast<DWORD>(std::min<std::size_t>(bytes.size(), chunk_bytes));
      DWORD bytes_written = 0;
      const BOOL ok =
          WriteFile(input_write.get(), bytes.data(), bytes_to_write, &bytes_written, nullptr);
      if (!ok || bytes_written == 0) {
        return false;
      }

      bytes.remove_prefix(bytes_written);
      if (!bytes.empty() && delay_between_chunks.count() > 0) {
        std::this_thread::sleep_for(delay_between_chunks);
      }
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

  PtyProcessLifecycle lifecycle() const {
    std::lock_guard output_lock(output_mutex);
    PtyProcessLifecycle lifecycle;
    lifecycle.process_id = process_id;
    lifecycle.created_at = created_at;
    lifecycle.terminating = terminating.load(std::memory_order_acquire);
    lifecycle.reader_done = reader_done;
    lifecycle.job_object_configured = job_configured;
    lifecycle.job_object_assigned = job_assigned;
    lifecycle.pseudo_console_open = pseudo_console_open.load(std::memory_order_relaxed);
    lifecycle.input_pipe_read_open = input_pipe_read_open.load(std::memory_order_relaxed);
    lifecycle.input_pipe_write_open = input_pipe_write_open.load(std::memory_order_relaxed);
    lifecycle.output_pipe_read_open = output_pipe_read_open.load(std::memory_order_relaxed);
    lifecycle.process_handle_open = process_handle_open.load(std::memory_order_relaxed);
    lifecycle.primary_thread_handle_open =
        primary_thread_handle_open.load(std::memory_order_relaxed);
    lifecycle.job_object_handle_open = job_object_handle_open.load(std::memory_order_relaxed);
    return lifecycle;
  }

  bool terminate() {
    bool expected = false;
    if (!terminating.compare_exchange_strong(expected, true)) {
      log_event(
          LogLevel::Debug,
          "pty",
          "terminate_noop",
          {{"process_id", std::to_string(process_id)}});
      return false;
    }

    log_event(
        LogLevel::Info,
        "pty",
        "terminate_start",
        {{"process_id", std::to_string(process_id)},
         {"job_assigned", job_assigned ? "true" : "false"}});

    {
      std::lock_guard write_lock(write_mutex);
      close_handle(input_write, input_pipe_write_open, "input_write");
    }
    {
      std::lock_guard lock(console_mutex);
      close_console_locked();
    }

    bool job_terminated = false;
    if (job.valid() && job_assigned) {
      if (!TerminateJobObject(job.get(), 0)) {
        log_event(
            LogLevel::Warn,
            "pty",
            "terminate_job_failed",
            {{"process_id", std::to_string(process_id)},
             {"error", std::to_string(GetLastError())}});
      } else {
        job_terminated = true;
        log_event(
            LogLevel::Info,
            "pty",
            "terminate_job",
            {{"process_id", std::to_string(process_id)}});
      }
    }
    close_handle(job, job_object_handle_open, "job");

    if (!job_assigned || !job_terminated) {
      (void)terminate_descendant_processes(process_id);
    }

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
    close_handle(input_read, input_pipe_read_open, "input_read");
    close_handle(output_read, output_pipe_read_open, "output_read");
    close_handle(primary_thread, primary_thread_handle_open, "primary_thread");
    close_handle(process, process_handle_open, "process");
    log_event(
        LogLevel::Info,
        "pty",
        "terminated",
        {{"process_id", std::to_string(process_id)}});
    return true;
  }
#else
  bool write(std::string_view) {
    return false;
  }

  bool write_throttled(
      std::string_view,
      std::size_t,
      std::chrono::milliseconds) {
    return false;
  }

  bool resize(short, short) {
    return false;
  }

  PtyProcessLifecycle lifecycle() const {
    std::lock_guard lock(output_mutex);
    PtyProcessLifecycle lifecycle;
    lifecycle.created_at = created_at;
    lifecycle.terminating = terminating.load(std::memory_order_acquire);
    lifecycle.reader_done = reader_done;
    return lifecycle;
  }

  bool terminate() {
    bool expected = false;
    if (!terminating.compare_exchange_strong(expected, true)) {
      return false;
    }
    mark_reader_done();
    return true;
  }
#endif
};

PtyProcess::PtyProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PtyProcess::~PtyProcess() = default;

PtyProcessResult PtyProcess::start(std::string_view command_line, short columns, short rows) {
  PtySpawnOptions options;
  options.command_line = std::string{command_line};
  options.executable = std::string{command_line};
  options.source = "explicit";
  return start(options, columns, rows);
}

PtyProcessResult PtyProcess::start(const PtySpawnOptions& options, short columns, short rows) {
#ifdef _WIN32
  if (options.command_line.empty()) {
    return {nullptr, "wmux: shell command line is empty\n", options};
  }

  UniqueHandle input_read;
  UniqueHandle input_write;
  HANDLE raw_input_read = nullptr;
  HANDLE raw_input_write = nullptr;
  if (!CreatePipe(&raw_input_read, &raw_input_write, nullptr, 0)) {
    return {
        nullptr,
        windows_error_message("wmux: failed to create ConPTY input pipe", GetLastError()),
        options};
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
        windows_error_message("wmux: failed to create ConPTY output pipe", GetLastError()),
        options};
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
            std::to_string(static_cast<unsigned long>(console_result)) + ")\n",
        options};
  }
  UniquePseudoConsole console{raw_console};
  output_write.reset();

  UniqueHandle job{CreateJobObjectW(nullptr, nullptr)};
  bool job_configured = false;
  if (!job.valid()) {
    log_event(
        LogLevel::Warn,
        "pty",
        "job_create_failed",
        {{"error", std::to_string(GetLastError())}});
  } else {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &job_limits,
            sizeof(job_limits))) {
      log_event(
          LogLevel::Warn,
          "pty",
          "job_configure_failed",
          {{"error", std::to_string(GetLastError())}});
      job.reset();
    } else {
      job_configured = true;
    }
  }

  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  if (attribute_size == 0) {
    return {
        nullptr,
        windows_error_message("wmux: failed to size process attribute list", GetLastError()),
        options};
  }

  AttributeList attributes{attribute_size};
  if (!InitializeProcThreadAttributeList(attributes.list, 1, 0, &attribute_size)) {
    return {
        nullptr,
        windows_error_message("wmux: failed to initialize process attribute list", GetLastError()),
        options};
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
        windows_error_message("wmux: failed to attach ConPTY to process", GetLastError()),
        options};
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes.list;

  auto command = widen(options.command_line);
  auto working_directory = widen(options.working_directory);
  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(
      nullptr,
      command.data(),
      nullptr,
      nullptr,
      TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
      nullptr,
      working_directory.empty() ? nullptr : working_directory.c_str(),
      &startup.StartupInfo,
      &process_info);
  if (!created) {
    log_event(
        LogLevel::Error,
        "pty",
        "spawn_createprocess_failed",
        {{"shell_source", options.source},
         {"shell_executable", options.executable},
         {"cwd", options.working_directory},
         {"columns", std::to_string(columns)},
         {"rows", std::to_string(rows)},
         {"error", std::to_string(GetLastError())}});
    return {
        nullptr,
        windows_error_message("wmux: failed to start shell process", GetLastError()),
        options};
  }

  UniqueHandle process{process_info.hProcess};
  UniqueHandle thread{process_info.hThread};
  bool job_assigned = false;
  if (job.valid()) {
    if (!AssignProcessToJobObject(job.get(), process.get())) {
      log_event(
          LogLevel::Warn,
          "pty",
          "job_assign_failed",
          {{"process_id", std::to_string(process_info.dwProcessId)},
           {"error", std::to_string(GetLastError())}});
      job.reset();
      job_configured = false;
    } else {
      job_assigned = true;
    }
  }

  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
    TerminateProcess(process.get(), 1);
    return {
        nullptr,
        windows_error_message("wmux: failed to resume shell process", GetLastError()),
        options};
  }

  auto impl = std::make_unique<Impl>();
  impl->input_read = std::move(input_read);
  impl->input_write = std::move(input_write);
  impl->output_read = std::move(output_read);
  impl->job = std::move(job);
  impl->process = std::move(process);
  impl->primary_thread = std::move(thread);
  impl->process_id = process_info.dwProcessId;
  impl->job_configured = job_configured;
  impl->job_assigned = job_assigned;
  impl->console = std::move(console);
  impl->max_raw_output_bytes = options.limits.max_pane_raw_output_bytes;
  impl->screen.set_scrollback_capacity(options.limits.max_pane_scrollback_lines);
  impl->screen.resize(columns, rows);
  impl->mark_runtime_handles_open();

  auto pty = std::shared_ptr<PtyProcess>(new PtyProcess(std::move(impl)));
  pty->impl_->start_reader();
  log_event(
      LogLevel::Info,
      "pty",
      "spawn",
      {{"process_id", std::to_string(process_info.dwProcessId)},
       {"shell_source", options.source},
       {"shell_executable", options.executable},
       {"cwd", options.working_directory},
       {"columns", std::to_string(columns)},
       {"rows", std::to_string(rows)},
       {"job_assigned", job_assigned ? "true" : "false"}});
  return {pty, {}, options};
#else
  (void)options;
  (void)columns;
  (void)rows;
  return {nullptr, "wmux: ConPTY shell processes are only supported on Windows\n", options};
#endif
}

bool PtyProcess::write_input(std::string_view bytes) {
  return impl_->write(bytes);
}

bool PtyProcess::write_input_throttled(
    std::string_view bytes,
    std::size_t chunk_bytes,
    std::chrono::milliseconds delay_between_chunks) {
  return impl_->write_throttled(bytes, chunk_bytes, delay_between_chunks);
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

PtyProcessLifecycle PtyProcess::lifecycle() const {
  return impl_->lifecycle();
}

PtyOutputSnapshot PtyProcess::output_snapshot() const {
  return impl_->snapshot();
}

PtyOutputChunk PtyProcess::wait_for_output(
    std::uint64_t next_sequence,
    std::chrono::milliseconds timeout) const {
  return impl_->wait_for_output(next_sequence, timeout);
}

bool PtyProcess::terminate() {
  return impl_->terminate();
}

}  // namespace wmux
