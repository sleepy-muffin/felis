#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>

// Runs one shell command for a tool call, on every platform this project ships.
//
// The command is a shell command line, not an argv vector: it is handed to the
// platform shell (cmd.exe on Windows; bash, or /bin/sh where bash is absent,
// elsewhere) exactly as the model wrote it, so pipes, redirection and quoting
// behave the way a user expects.
// The child runs in its own process group / job object, so a timeout or a
// cancellation kills the whole tree instead of orphaning grandchildren.
namespace felis::process
{

// How a command finished.
enum class status {
  // The process exited on its own; exit_code is meaningful.
  exited,
  // A timeout or a stop request ended it; the tree was killed.
  killed,
  // The process could not be started at all.
  failed,
};

struct spec {
  // Shell command line, e.g. "cmake --build build && ctest".
  std::string command;

  // Working directory. Empty inherits the caller's.
  std::filesystem::path cwd;

  // How long to wait before killing the tree. Zero waits forever.
  std::chrono::milliseconds timeout{std::chrono::minutes(2)};

  // Cooperative cancellation, polled while the command runs.
  std::stop_token stop;

  // Merge stderr into stdout. A model reads a command's output as one stream,
  // so this is on unless a caller wants the streams apart.
  bool merge_stderr = true;

  // Upper bound on captured output, in bytes. The head and the tail are kept
  // and the middle is dropped and marked, so a long build log still shows its
  // first errors and its final verdict.
  std::size_t max_output_bytes = 32 * 1024;

  // Remove environment entries that look like credentials before the child
  // starts, so a shell command cannot read the provider key out of the
  // environment.
  bool sanitize_environment = true;
};

struct result {
  status outcome = status::failed;

  // Exit code, meaningful only for status::exited. A process that died from a
  // signal reports the negated signal number on POSIX systems.
  int exit_code = -1;

  // Captured output, already capped to spec::max_output_bytes and never cut in
  // the middle of a UTF-8 sequence.
  std::string output;

  // True when more output than the cap arrived and the middle was dropped.
  bool truncated = false;

  // True when spec::stop fired and the tree was killed for it.
  bool cancelled = false;

  // True when spec::timeout elapsed and the tree was killed for it.
  bool timed_out = false;

  // Human-readable reason, set only for status::failed.
  std::string error;
};

// Runs the command and waits for it. The calling thread blocks for the whole
// duration; the tool executor already runs off the UI thread. Safe to call
// from any thread.
result run(const spec& request);

// The shell run() hands the command to: "cmd.exe" on Windows, "bash
// (pipefail)" or "sh" elsewhere, whichever was found. Exposed so a tool
// description can name it.
std::string shell_name();

} // namespace felis::process
