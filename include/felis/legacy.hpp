#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include "felis/common.hpp"
#include "felis/log.hpp"

namespace felis {
// For specified error code
template <auto F, auto N, typename... Types>
auto checked_invoke(
    Types&&... Args)
{
  auto result = F(std::forward<Types>(Args)...);
  if (decltype(N)(result) != N) {
    log::debug("Invocation of function has failed: {} {:#x}", FELIS_FUNCTION,
               decltype(N)(result));
    raise("Invocation of function has failed");
  }
  return result;
}
}

namespace felis::legacy {
template <auto F, typename... Types>
auto yinvoke(
    Types&&... Args)
{
  return checked_invoke<F, 0UL>(std::forward<Types>(Args)...);
}

template <auto F, typename... Types>
auto einvoke(
    Types&&... Args)
{
  return checked_invoke<F, 0UL>(std::forward<Types>(Args)...);
}

template <typename T, T good>
struct rcheck
{
  void check(
      T v) const
  {
    if (v != good) {
      log::debug("Invocation of function has failed: {} {:#x}", FELIS_FUNCTION,
                 static_cast<unsigned long>(v));
      throw pinch{"Invocation of function has failed"};
    }
  }
  template <typename... Types>
  void operator=(
      Types&&... values) const
  {
    check(std::forward<Types>(values)...);
  }
};

#ifdef __linux__
std::string find_system_font(std::string_view name);
#endif


// A cross-instance guard for one assistant directory.
//
// An exclusive, process-lifetime lock on a file.
//
// The lock is an operating-system advisory lock (flock on POSIX, LockFileEx on
// Windows) held for as long as this object exists. That is what makes staleness a
// non-problem: the kernel releases the lock when the owning process exits, even
// after a crash, so a lock file left behind by a killed instance can never block
// the next one. For that reason the file is deliberately *not* deleted on
// release; deleting it would introduce a race where two processes open different
// inodes for the same path.
//
// The file also records the owning pid and the time it was taken, purely so a
// refused instance can tell the user who holds it. That text is diagnostic only:
// it is never used to decide whether the lock is stale (pids are reused).
class instance_lock {
public:
  instance_lock() = default;
  ~instance_lock();

  instance_lock(const instance_lock&) = delete;
  instance_lock& operator=(const instance_lock&) = delete;

  // Takes the lock at `path`, creating the parent directory when needed. Returns
  // false when another running process holds it; `error` then names that process
  // when its recorded pid can be read.
  bool acquire(const std::filesystem::path& path, std::string& error);

  // Releases the lock. Safe to call more than once, and from the destructor.
  void release();

  [[nodiscard]] bool held() const;

private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif

  std::filesystem::path path_;
};

}
