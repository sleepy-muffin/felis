#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <string>
#include <thread>

#include "felis/common.hpp"


namespace felis {
namespace log {
  // inline std::mutex _logger_bus;

  // Your message_type enum stays compatible
  enum class message_type { INFO, DEBUG, EXCEPTION };

  // Internal backend class
  class backend {
  public:
    backend()
      : output_(&std::cout), worker_([this] { run(); }) {
    }

    ~backend() {
      shutdown(); // automatic flush and stop
    }

    backend(const backend&) = delete;
    backend& operator=(const backend&) = delete;

    // Push a message into the queue
    void push(
      message_type type, std::string msg
    ) {
      {
        std::lock_guard lock(mtx_);
        queue_.emplace(type, std::move(msg));
      }
      cv_.notify_one();
    }

    void set_output_file(
      const std::filesystem::path& path, bool append = true
    ) {
      auto file = std::make_unique<std::ofstream>(
        path,
        std::ios::out | (append ? std::ios::app : std::ios::trunc)
      );

      if (!file->is_open()) {
        throw std::runtime_error(
          std::format("Failed to open log file: {}", path.string())
        );
      }

      std::lock_guard lock(output_mtx_);

      if (output_) {
        output_->flush();
      }

      file_ = std::move(file);
      output_ = file_.get();
    }

    void set_output_stdout() {
      std::lock_guard lock(output_mtx_);

      if (output_) {
        output_->flush();
      }

      file_.reset();
      output_ = &std::cout;
    }

    // Explicit shutdown (optional)
    void shutdown() {
      bool expected = false;
      if (!stopped_.compare_exchange_strong(expected, true))
        return; // already stopped

      {
        std::lock_guard lock(mtx_);
        stopping_ = true;
      }

      cv_.notify_all();

      if (worker_.joinable())
        worker_.join();

      std::lock_guard lock(output_mtx_);
      if (output_) {
        output_->flush();
      }
    }

    // std::FILE* file_ = nullptr;

    template<typename... Types>
    constexpr void info(
      const std::format_string<Types...>& Fmt, Types&&... Args
    ) {
      std::string info_str = std::format(Fmt, std::forward<Types>(Args)...);
      push(message_type::INFO, info_str);
    }

    template<typename... Types>
    constexpr void debug(
      const std::format_string<Types...>& Fmt, Types&&... Args
    ) {
      if constexpr (FELIS_DEBUG) {
        std::string info_str = std::format(Fmt, std::forward<Types>(Args)...);
        push(message_type::DEBUG, info_str);
      }
    }

    template<typename... Types>
    constexpr void error(
      const std::format_string<Types...>& Fmt, Types&&... Args
    ) {
      std::string info_str = std::format(Fmt, std::forward<Types>(Args)...);
      push(message_type::EXCEPTION, info_str);
    }

  private:
    void run() {
      for (;;) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });

        if (stopping_ && queue_.empty())
          break;

        auto [type, msg] = std::move(queue_.front());
        queue_.pop();

        lock.unlock();

        {
          std::lock_guard output_lock(output_mtx_);
          *output_ << msg << '\n';
          output_->flush();
        }
      }
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::pair<message_type, std::string> > queue_;
    bool stopping_ = false;
    std::atomic_bool stopped_ = false;

    std::mutex output_mtx_;
    std::unique_ptr<std::ofstream> file_;
    std::ostream* output_;

    std::thread worker_;
  };

  // Function-local static instance ensures automatic creation
  inline backend& singleton() {
    static backend b;
    return b;
  }

  inline void set_output_file(
    const std::filesystem::path& path, bool append = true
  ) {
    singleton().set_output_file(path, append);
  }

  inline void set_output_stdout() {
    singleton().set_output_stdout();
  }

  // Submit API matches your previous API exactly
  inline void submit(
    const message_type type, const std::string& msg
  ) {
    singleton().push(type, msg);
  }

  inline void submit(
    message_type type, std::string&& msg
  ) {
    singleton().push(type, std::move(msg));
  }


  template<typename... Types>
  constexpr void info(
    const std::format_string<Types...>& Fmt, Types&&... Args
  ) {
    singleton().info(Fmt, std::forward<Types>(Args)...);
  }

  template<typename... Types>
  constexpr void debug(
    const std::format_string<Types...>& Fmt, Types&&... Args
  ) {
    singleton().debug(Fmt, std::forward<Types>(Args)...);
  }

  template<typename... Types>
  constexpr void error(
    const std::format_string<Types...>& Fmt, Types&&... Args
  ) {
    singleton().error(Fmt, std::forward<Types>(Args)...);
  }

  // Optional kill() function for explicit shutdown
  inline void kill() {
    singleton().shutdown();
  }
} // namespace log

template<typename... Types>
constexpr void print(
  const std::format_string<Types...>& Fmt, Types&&... Args
) {
  std::string info_str = std::format(Fmt, std::forward<Types>(Args)...);
  log::submit(log::message_type::INFO, info_str);
}


struct pinch : std::exception {
  template<typename... Types>
   constexpr pinch(
    const std::format_string<Types...>& Fmt, Types&&... Args
  ) {
    std::string info_str = std::format(Fmt, std::forward<Types>(Args)...);
    msg_ = std::format("trace {}:{}", FELIS_FUNCTION, info_str);
  }

  // pinch(std::string_view func_name, std::string_view information) : std::exception{},
  // msg_(std::format("trace {}:{}", func_name, information)) { }

  pinch()
    : std::exception{}, msg_("{null}") {
  }

  [[nodiscard]] const char* what() const noexcept override {
    return msg_.c_str();
  }

  std::string msg_;
};

template<typename... Types>
void raise(
  const std::format_string<Types...>& Fmt, Types&&... Args
) {
  felis::print(Fmt, std::forward<Types>(Args)...);
  throw pinch(Fmt, std::forward<Types>(Args)...);
}

template<typename... Types>
void check(
  bool condition, const std::format_string<Types...>& Fmt, Types&&... Args
) {
  if (!condition) {
    raise(Fmt, std::forward<Types>(Args)...);
  }
}

inline void check(
  bool condition
) {
  check(condition, "");
}


struct profiler
{
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = std::chrono::duration<double>;

  profiler(
      size_t reserve, bool print_on_lap) : start(clock::now()), print_on_lap(print_on_lap)
  {
    laps.reserve(reserve);
  }

  void lap(
      std::optional<std::string_view> msg = std::nullopt)
  {
    time_point end = clock::now();
    const duration diff = end - start;
    laps.emplace_back(diff);

    if (print_on_lap) {
      print_duration(diff, msg);
    }

    reset(); // exclude emplace_back
  }

  void reset()
  {
    start = clock::now();
  }

  static void print_duration(
      duration value, std::optional<std::string_view> msg = std::nullopt)
  {
    const auto milliseconds = std::chrono::duration<double, std::milli>(value).count();

    if (msg.has_value()) {
      log::info("{}: {:.6f} ms", msg.value(), milliseconds);
    }
    else {
      log::info("{:.6f} ms", milliseconds);
    }
  }

  time_point start;
  bool print_on_lap;
  std::vector<duration> laps;
};


}
