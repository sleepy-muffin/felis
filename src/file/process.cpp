#include "felis/file/process.hpp"

#include <felis/log.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>
#include <mutex>

#else // POSIX
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace felis::process::detail
{

// A wall-clock bound for the child. An absent point means "no timeout": the
// caller asked to wait forever.
class deadline {
public:
  explicit deadline(std::chrono::milliseconds timeout) {
    if (timeout.count() > 0) {
      end_ = std::chrono::steady_clock::now() + timeout;
    }
  }

  bool expired() const {
    return end_.has_value() && std::chrono::steady_clock::now() >= *end_;
  }

  // How long a wait may block, in milliseconds. Never more than `cap`, never
  // negative, so it can be handed straight to poll() / WaitForSingleObject().
  int poll_milliseconds(int cap) const {
    if (!end_) return cap;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          *end_ - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) return 0;
    if (left > cap) return cap;
    return static_cast<int>(left);
  }

private:
  std::optional<std::chrono::steady_clock::time_point> end_;
};

// True when an environment variable name looks like it carries a credential.
//
// The markers are matched on alphanumeric word boundaries, so OPENAI_API_KEY
// and GH_TOKEN are dropped while KEYBOARD_LAYOUT survives. Underscore counts as
// a boundary on purpose: most providers separate words with it.
inline bool looks_secret(std::string_view name) {
  static constexpr std::string_view markers[] = {"KEY",      "TOKEN", "SECRET",
                                                 "PASSWORD", "PASSWD", "CREDENTIAL",
                                                 "AUTH"};
  std::string upper;
  upper.reserve(name.size());
  for (const char c : name) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }

  for (const std::string_view marker : markers) {
    std::size_t pos = 0;
    while ((pos = upper.find(marker, pos)) != std::string::npos) {
      const bool left_ok =
          pos == 0 || !std::isalnum(static_cast<unsigned char>(upper[pos - 1]));
      const std::size_t after = pos + marker.size();
      const bool right_ok =
          after == upper.size() || !std::isalnum(static_cast<unsigned char>(upper[after]));
      if (left_ok && right_ok) return true;
      pos = after;
    }
  }
  return false;
}

// True when the variable names a shell startup file. A non-interactive bash
// sources $BASH_ENV before the command (sh sources $ENV), which would let a
// stray file in the user's environment run inside a tool call. Dropped even
// though the name is not credential-like.
inline bool looks_shell_startup(std::string_view name) {
  return name == "BASH_ENV" || name == "ENV";
}

// Drops a trailing incomplete UTF-8 sequence so a byte-capped cut never leaves
// a broken character at the end of the head.
inline std::string utf8_head_cut(const std::string& text) {
  const std::size_t size = text.size();
  if (size == 0) return text;

  std::size_t lead_index = size; // one past the lead byte of the last character
  std::size_t continuations = 0;
  while (lead_index > 0 && continuations < 4 &&
         (static_cast<unsigned char>(text[lead_index - 1]) & 0xC0) == 0x80) {
    --lead_index;
    ++continuations;
  }
  if (lead_index == 0) return text; // nothing but continuation bytes: leave it alone

  const unsigned char lead = static_cast<unsigned char>(text[lead_index - 1]);
  std::size_t needed = 1;
  if ((lead & 0x80) == 0x00) {
    needed = 1;
  } else if ((lead & 0xE0) == 0xC0) {
    needed = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    needed = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    needed = 4;
  } else {
    return text; // not a valid lead byte, nothing sensible to trim
  }

  if (size - (lead_index - 1) >= needed) return text;
  return text.substr(0, lead_index - 1);
}

// How many leading bytes of the tail belong to a character whose head was cut
// away by the truncation, so they must be skipped.
inline std::size_t utf8_tail_start(const std::string& text) {
  std::size_t index = 0;
  while (index < text.size() && index < 4 &&
         (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
    ++index;
  }
  return index;
}

// Keeps the first half and the last half of a stream and counts what fell out
// of the middle. A long build log then still shows its first error and its
// final verdict instead of 32 KiB of compiler noise.
class capture {
public:
  explicit capture(std::size_t limit) {
    if (limit == 0) limit = 1;
    head_limit_ = limit / 2;
    tail_limit_ = limit - head_limit_;
    tail_.assign(tail_limit_, '\0');
  }

  void append(const char* data, std::size_t size) {
    if (data == nullptr || size == 0) return;

    std::size_t consumed = 0;
    if (head_.size() < head_limit_) {
      const std::size_t room = head_limit_ - head_.size();
      consumed = std::min(size, room);
      head_.append(data, consumed);
    }
    for (std::size_t i = consumed; i < size; ++i) push_tail(data[i]);
  }

  bool truncated() const { return dropped_ > 0; }

  std::string finish() const {
    std::string head = head_;
    std::string tail = tail_contents();
    if (dropped_ == 0) return head + tail;

    // Do not leave half a character on either side of the marker.
    const std::string trimmed_head = utf8_head_cut(head);
    const std::size_t skipped = utf8_tail_start(tail);
    const std::size_t lost =
        dropped_ + (head.size() - trimmed_head.size()) + skipped;

    std::string out;
    out.reserve(trimmed_head.size() + tail.size() + 40);
    out += trimmed_head;
    out += "\n… ";
    out += std::to_string(lost);
    out += " bytes truncated …\n";
    out += tail.substr(skipped);
    return out;
  }

private:
  void push_tail(char value) {
    if (tail_limit_ == 0) {
      ++dropped_;
      return;
    }
    if (tail_size_ < tail_limit_) {
      tail_[(tail_pos_ + tail_size_) % tail_limit_] = value;
      ++tail_size_;
      return;
    }
    // Full: overwrite the oldest byte and move the window forward.
    tail_[tail_pos_] = value;
    tail_pos_ = (tail_pos_ + 1) % tail_limit_;
    ++dropped_;
  }

  std::string tail_contents() const {
    std::string out;
    out.reserve(tail_size_);
    const std::size_t start = (tail_pos_ + tail_limit_ - tail_size_) % tail_limit_;
    for (std::size_t i = 0; i < tail_size_; ++i) {
      out.push_back(tail_[(start + i) % tail_limit_]);
    }
    return out;
  }

  std::string head_;
  std::size_t head_limit_ = 0;

  // Ring buffer for the newest bytes.
  std::string tail_;
  std::size_t tail_limit_ = 0;
  std::size_t tail_pos_ = 0; // next slot to write
  std::size_t tail_size_ = 0;
  std::size_t dropped_ = 0;
};

} // namespace felis::process::detail

#ifdef _WIN32

namespace felis::process::detail
{

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), size);
  return out;
}

std::string narrow(std::wstring_view utf16) {
  if (utf16.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(),
                                       static_cast<int>(utf16.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

// True when the bytes decode cleanly as UTF-8. Pure-ASCII output — the common
// case — is valid UTF-8, so ordinary commands never take the transcode path.
bool is_valid_utf8(std::string_view bytes) {
  if (bytes.empty()) return true;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                             static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

// Decodes `bytes` as `code_page` and re-encodes the text as UTF-8. Best effort:
// an empty result means the code page could not represent the bytes.
std::string transcode_to_utf8(std::string_view bytes, UINT code_page) {
  if (bytes.empty()) return {};
  const int size = MultiByteToWideChar(code_page, 0, bytes.data(),
                                       static_cast<int>(bytes.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(code_page, 0, bytes.data(), static_cast<int>(bytes.size()),
                      wide.data(), size);
  return narrow(wide);
}

// cmd.exe is switched to code page 65001 so its builtins and console programs
// emit UTF-8, but a program that ignores the console code page — a Python tool
// on a Chinese Windows, say — still writes the system's OEM code page. Rather
// than guess, keep the bytes when they are already valid UTF-8 and, when they
// are not, decode them as the console's code page and re-encode as UTF-8.
std::string decode_console_output(std::string_view bytes) {
  if (is_valid_utf8(bytes)) return std::string(bytes);

  std::string decoded = transcode_to_utf8(bytes, GetOEMCP());
  if (!decoded.empty()) return decoded;

  decoded = transcode_to_utf8(bytes, GetACP());
  if (!decoded.empty()) return decoded;

  return std::string(bytes);
}

// The message of the last failed Win32 call, without its trailing newline.
std::string last_error() {
  const DWORD code = GetLastError();
  wchar_t* text = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  if (length == 0 || text == nullptr) return "error " + std::to_string(code);

  std::wstring message(text, length);
  LocalFree(text);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return narrow(message) + " (" + std::to_string(code) + ")";
}

// Where the command shell lives. ComSpec is the documented answer; cmd.exe is
// the fallback when the variable is missing or empty.
std::wstring shell_path() {
  const DWORD needed = GetEnvironmentVariableW(L"ComSpec", nullptr, 0);
  if (needed > 0) {
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"ComSpec", value.data(), needed);
    if (written > 0 && written < needed) {
      value.resize(written);
      return value;
    }
  }
  return L"cmd.exe";
}

// True when the shell is cmd.exe, the only one this layer knows how to put
// into UTF-8 mode.
bool is_cmd_exe(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);

  std::wstring lower;
  lower.reserve(name.size());
  for (const wchar_t character : name) {
    lower.push_back(static_cast<wchar_t>(::towlower(character)));
  }

  return lower == L"cmd.exe";
}

// "<shell>" /d /s /c "<command>" — the shape cmd.exe documents for passing a
// whole command line through, quotes included.
std::wstring command_line(const std::wstring& shell, const std::string& command) {
  std::wstring line;
  line.reserve(shell.size() + command.size() + 16);
  line += L'"';
  line += shell;
  line += L"\" /d /s /c \"";
  line += widen(command);
  line += L'"';
  return line;
}

// Copies the current environment without credential-looking entries, in the
// double-null-terminated layout CreateProcessW wants.
std::vector<wchar_t> environment_block() {
  std::vector<wchar_t> block;
  wchar_t* raw = GetEnvironmentStringsW();
  if (raw == nullptr) {
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
  }

  for (const wchar_t* cursor = raw; *cursor != L'\0';) {
    const std::wstring_view entry{cursor};
    cursor += entry.size() + 1;

    // "=C:=C:\\dir" style entries carry the per-drive current directory; they
    // are not valid for a fresh environment block.
    if (entry.empty() || entry.front() == L'=') continue;
    const std::size_t equals = entry.find(L'=');
    if (equals == std::wstring_view::npos) continue;
    if (looks_secret(narrow(entry.substr(0, equals)))) continue;

    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  FreeEnvironmentStringsW(raw);

  if (block.empty()) block.push_back(L'\0');
  block.push_back(L'\0');
  return block;
}

// A Win32 handle that closes itself, so an early return on error cannot leak.
struct handle {
  HANDLE value = nullptr;

  handle() = default;
  explicit handle(HANDLE raw) : value(raw) {}
  ~handle() { reset(); }

  handle(const handle&) = delete;
  handle& operator=(const handle&) = delete;

  handle(handle&& other) noexcept : value(other.value) { other.value = nullptr; }
  handle& operator=(handle&& other) noexcept {
    if (this != &other) {
      reset();
      value = other.value;
      other.value = nullptr;
    }
    return *this;
  }

  void reset() {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    value = nullptr;
  }

  explicit operator bool() const {
    return value != nullptr && value != INVALID_HANDLE_VALUE;
  }
};

// One capture shared by the stdout and stderr readers, which run on separate
// threads when the streams are kept apart.
struct shared_capture {
  explicit shared_capture(std::size_t limit) : sink(limit) {}

  void append(const char* data, std::size_t size) {
    const std::lock_guard lock(mutex);
    sink.append(data, size);
  }

  std::string finish() {
    const std::lock_guard lock(mutex);
    return sink.finish();
  }

  bool truncated() {
    const std::lock_guard lock(mutex);
    return sink.truncated();
  }

  std::mutex mutex;
  capture sink;
};

void read_into(HANDLE pipe, const std::shared_ptr<shared_capture>& sink) {
  char buffer[8192];
  for (;;) {
    DWORD got = 0;
    if (ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) == FALSE) break;
    if (got == 0) break;
    sink->append(buffer, got);
  }
}

// A grandchild can inherit the write end of the pipe and keep it open after the
// shell is reaped. Waiting forever for the reader would hang the tool, so give
// it a moment and then cancel its blocking ReadFile.
void join_reader(std::thread& reader) {
  if (!reader.joinable()) return;
  if (WaitForSingleObject(reader.native_handle(), 5000) != WAIT_OBJECT_0) {
    CancelSynchronousIo(reader.native_handle());
  }
  reader.join();
}

result run_windows(const spec& request) {
  result out;

  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof inheritable;
  inheritable.bInheritHandle = TRUE;

  struct stream {
    handle read_end;
    handle write_end;
    std::thread reader;
  };
  std::vector<stream> streams;
  const std::size_t stream_count = request.merge_stderr ? 1 : 2;
  streams.reserve(stream_count);
  for (std::size_t i = 0; i < stream_count; ++i) {
    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (CreatePipe(&read_raw, &write_raw, &inheritable, 0) == FALSE) {
      out.error = "CreatePipe failed: " + last_error();
      return out;
    }
    stream created;
    created.read_end = handle(read_raw);
    created.write_end = handle(write_raw);
    // The parent's read end must not leak into the child.
    SetHandleInformation(created.read_end.value, HANDLE_FLAG_INHERIT, 0);
    streams.push_back(std::move(created));
  }

  handle null_input(CreateFileW(L"NUL", GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                OPEN_EXISTING, 0, nullptr));

  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_input ? null_input.value : GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = streams.front().write_end.value;
  startup.hStdError = request.merge_stderr ? streams.front().write_end.value
                                           : streams.back().write_end.value;

  // A job object with KILL_ON_JOB_CLOSE takes the whole tree down, including
  // anything the command spawned, which TerminateProcess alone would orphan.
  handle job(CreateJobObjectW(nullptr, nullptr));
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                            sizeof limits);
  }

  const std::wstring shell = shell_path();

  // The capture is UTF-8, but cmd.exe writes in the OEM code page. Switching
  // the child's code page keeps non-ASCII output readable for the builtins and
  // any program that honours it; a different ComSpec is left alone, since it may
  // not understand chcp. `2>&1` keeps a chcp failure off the merged output.
  std::string payload = request.command;
  if (is_cmd_exe(shell)) {
    payload = "chcp 65001 >nul 2>&1 & " + payload;
  }

  std::wstring line = command_line(shell, payload);
  std::vector<wchar_t> mutable_line(line.begin(), line.end());
  mutable_line.push_back(L'\0');

  const std::wstring working_directory =
      request.cwd.empty() ? std::wstring() : request.cwd.wstring();
  std::vector<wchar_t> environment;
  if (request.sanitize_environment) environment = environment_block();

  PROCESS_INFORMATION process_info{};
  const BOOL started = CreateProcessW(
      shell.c_str(), mutable_line.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
      environment.empty() ? nullptr : environment.data(),
      working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
      &process_info);

  // The child owns the write ends now; if the parent kept one open the readers
  // would never reach end-of-file.
  for (stream& s : streams) s.write_end.reset();

  if (started == FALSE) {
    out.error = "CreateProcess failed: " + last_error();
    return out;
  }

  handle process(process_info.hProcess);
  handle primary_thread(process_info.hThread);
  // If the job was created but the assignment failed, killing the job would
  // kill nothing, so fall back to terminating the process directly.
  bool in_job = false;
  if (job) {
    in_job = AssignProcessToJobObject(job.value, process.value) != FALSE;
  }

  auto sink = std::make_shared<shared_capture>(request.max_output_bytes);
  for (stream& s : streams) {
    s.reader = std::thread(read_into, s.read_end.value, sink);
  }

  const deadline limit{request.timeout};
  bool cancelled = false;
  bool timed_out = false;
  for (;;) {
    if (request.stop.stop_requested()) {
      cancelled = true;
      break;
    }
    if (limit.expired()) {
      timed_out = true;
      break;
    }
    const int slice = std::max(1, limit.poll_milliseconds(200));
    const DWORD waited =
        WaitForSingleObject(process.value, static_cast<DWORD>(slice));
    if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
  }

  if (cancelled || timed_out) {
    if (in_job) {
      TerminateJobObject(job.value, 1);
    } else {
      TerminateProcess(process.value, 1);
    }
    WaitForSingleObject(process.value, INFINITE);
    out.outcome = status::killed;
  } else {
    out.outcome = status::exited;
  }

  DWORD exit_code = 0;
  if (GetExitCodeProcess(process.value, &exit_code) != FALSE) {
    out.exit_code = static_cast<int>(exit_code);
  }

  for (stream& s : streams) join_reader(s.reader);

  // A program that ignores the console code page left OEM bytes behind; decode
  // them so Chinese output reaches the model as text, not mojibake.
  out.output = decode_console_output(sink->finish());
  out.truncated = sink->truncated();
  out.cancelled = cancelled;
  out.timed_out = timed_out;
  return out;
}

} // namespace felis::process::detail

#endif // _WIN32

#ifndef _WIN32

// Declared by the C library; the name is deliberately unmangled.
extern "C" char** environ;

namespace felis::process::detail
{

std::string errno_text(int code) {
  return std::string(std::strerror(code)) + " (" + std::to_string(code) + ")";
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// A copy of the environment without credential-looking entries. Built
// before fork, because after fork only async-signal-safe calls are allowed.
std::vector<std::string> sanitized_environment() {
  std::vector<std::string> entries;
  for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
    const std::string_view entry{*cursor};
    const std::size_t equals = entry.find('=');
    if (equals == std::string_view::npos) continue;
    const std::string_view name = entry.substr(0, equals);
    if (looks_secret(name) || looks_shell_startup(name)) continue;
    entries.emplace_back(entry);
  }
  return entries;
}

// The shell a command runs under, resolved once on first use.
//
// bash is preferred where it exists because `-o pipefail` makes a pipeline
// report its first failing command; without it `build | tail` exits 0 and the
// tool hands the model a false success. Where bash is absent (Alpine, BusyBox,
// BSD) /bin/sh is the POSIX-guaranteed fallback, minus pipefail.
struct shell_choice {
  const char* path;   // absolute path, so the child never needs PATH
  const char* argv0;  // what the command sees as $0
  bool pipefail;
};

const shell_choice& posix_shell() {
  static const shell_choice choice = [] {
    for (const char* candidate : {"/usr/bin/bash", "/bin/bash"}) {
      if (::access(candidate, X_OK) == 0) {
        return shell_choice{candidate, "bash", true};
      }
    }
    felis::log::info("bash not found; commands fall back to /bin/sh without pipefail");
    return shell_choice{"/bin/sh", "sh", false};
  }();
  return choice;
}

result run_posix(const spec& request) {
  result out;

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};

  if (::pipe(out_pipe) != 0) {
    out.error = "pipe failed: " + errno_text(errno);
    return out;
  }
  if (!request.merge_stderr && ::pipe(err_pipe) != 0) {
    out.error = "pipe failed: " + errno_text(errno);
    close_fd(out_pipe[0]);
    close_fd(out_pipe[1]);
    return out;
  }

  const std::vector<std::string> environment =
      request.sanitize_environment ? sanitized_environment() : std::vector<std::string>();
  std::vector<char*> envp;
  if (!environment.empty()) {
    envp.reserve(environment.size() + 1);
    for (const std::string& entry : environment) {
      envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);
  }

  // Resolved and built before fork: after fork only async-signal-safe calls are
  // allowed, and the once-initialised static could allocate.
  const shell_choice& shell = posix_shell();
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(shell.argv0));
  if (shell.pipefail) {
    argv.push_back(const_cast<char*>("-o"));
    argv.push_back(const_cast<char*>("pipefail"));
  }
  argv.push_back(const_cast<char*>("-c"));
  argv.push_back(const_cast<char*>(request.command.c_str()));
  argv.push_back(nullptr);

  const std::string working_directory =
      request.cwd.empty() ? std::string() : request.cwd.string();

  const pid_t pid = ::fork();
  if (pid < 0) {
    out.error = "fork failed: " + errno_text(errno);
    close_fd(out_pipe[0]);
    close_fd(out_pipe[1]);
    close_fd(err_pipe[0]);
    close_fd(err_pipe[1]);
    return out;
  }

  if (pid == 0) {
    // Child. Its own process group is what lets a timeout kill grandchildren
    // instead of leaving them behind.
    ::setpgid(0, 0);
    if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) {
      _exit(126);
    }

    ::dup2(out_pipe[1], STDOUT_FILENO);
    if (request.merge_stderr) {
      ::dup2(out_pipe[1], STDERR_FILENO);
    } else {
      ::dup2(err_pipe[1], STDERR_FILENO);
    }

    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    if (!request.merge_stderr) {
      ::close(err_pipe[0]);
      ::close(err_pipe[1]);
    }

    if (!envp.empty()) {
      ::execve(shell.path, argv.data(), envp.data());
    } else {
      ::execv(shell.path, argv.data());
    }
    _exit(127);
  }

  // Parent. Only the read ends stay open here.
  close_fd(out_pipe[1]);
  close_fd(err_pipe[1]);

  capture sink{request.max_output_bytes};
  std::vector<pollfd> watched;
  watched.push_back(pollfd{out_pipe[0], POLLIN, 0});
  if (!request.merge_stderr) watched.push_back(pollfd{err_pipe[0], POLLIN, 0});

  std::size_t open_streams = watched.size();
  bool cancelled = false;
  bool timed_out = false;
  bool reaped = false;
  int status = 0;
  const deadline limit{request.timeout};
  char buffer[8192];

  while (!reaped) {
    if (request.stop.stop_requested()) {
      cancelled = true;
      break;
    }
    if (limit.expired()) {
      timed_out = true;
      break;
    }

    if (open_streams == 0) {
      // Output is over but the process may still be running; wait for it in
      // small slices so a stop request stays responsive.
      const pid_t ended = ::waitpid(pid, &status, WNOHANG);
      if (ended == pid) {
        reaped = true;
        break;
      }
      if (ended < 0 && errno != EINTR) {
        reaped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    for (pollfd& entry : watched) entry.revents = 0;
    const int ready = ::poll(watched.data(), watched.size(), limit.poll_milliseconds(200));
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;

    for (pollfd& entry : watched) {
      if (entry.fd < 0 || entry.revents == 0) continue;
      ssize_t got = 0;
      do {
        got = ::read(entry.fd, buffer, sizeof buffer);
      } while (got < 0 && errno == EINTR);
      if (got > 0) {
        sink.append(buffer, static_cast<std::size_t>(got));
        continue;
      }
      close_fd(entry.fd);
      --open_streams;
    }
  }

  for (pollfd& entry : watched) close_fd(entry.fd);

  if (cancelled || timed_out) {
    // Negative pid: the whole process group.
    ::kill(-pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    out.outcome = status::killed;
  } else {
    if (!reaped) {
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
    out.outcome = status::exited;
    if (WIFEXITED(status)) {
      out.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      out.exit_code = -WTERMSIG(status);
    }
  }

  out.output = sink.finish();
  out.truncated = sink.truncated();
  out.cancelled = cancelled;
  out.timed_out = timed_out;
  return out;
}

} // namespace felis::process::detail

#endif // !_WIN32

namespace felis::process
{

result run(const spec& request) {
#ifdef _WIN32
  return detail::run_windows(request);
#else
  return detail::run_posix(request);
#endif
}

std::string shell_name() {
#ifdef _WIN32
  return "cmd.exe";
#else
  return detail::posix_shell().pipefail ? "bash (pipefail)" : "sh";
#endif
}

} // namespace felis::process
