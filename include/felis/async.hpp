#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "felis/common.hpp"
#include "felis/log.hpp"

namespace felis::async {
struct io_context;

struct delayer {
  std::chrono::system_clock::duration timer_;

  void reset() {
    timer_ = std::chrono::seconds(2);
  }

  bool before(std::chrono::system_clock::time_point end_point) {
    auto at_least = end_point - std::chrono::system_clock::now();
    // std::chrono::system_clock::duration at_least =
    //    end_point - std::chrono::system_clock::now();
    // log::debug("before: at_least {}", at_least);
    if (at_least <= std::chrono::milliseconds(0)) {
      timer_ = std::chrono::milliseconds(0);
      return true;
    } else if (at_least < timer_) {
      timer_ = at_least;
      return false;
    }
    return false;
  }

  void exec_delay() const {
    if (timer_ == std::chrono::milliseconds(0)) {
      // log::debug("local_delayer::exec_delay time's up");
      return;
    }
    // log::debug("local_delayer::exec_delay delay");
    std::this_thread::sleep_for(timer_);
  }

  void shorter(std::chrono::system_clock::duration dura) {
  }
};

// task_context, thread slice
class strand {
public:
  io_context& parent_;
  std::stack<std::coroutine_handle<> > addresses_;

  struct Condition {
    virtual bool evaluate(delayer& local_delayer) = 0;
    // Original virtual destructor is not allowed to be 0
    virtual ~Condition() = default;
  };

  std::atomic_bool proceeding_ = false;

  strand(io_context& parent, std::coroutine_handle<> coro)
    : parent_(parent) {
    addresses_.push(coro);
  }

  strand(io_context& parent, strand&& obj) noexcept
    : parent_(parent), addresses_(std::move(obj.addresses_)),
      condition_(std::move(obj.condition_)) {
    proceeding_ = static_cast<bool>(obj.proceeding_);
  }

  auto stacks() const ->
    std::string {
    std::ostringstream ss;
    ss << "[";

    auto copy = addresses_;
    std::vector<decltype(copy.top().address())> addresses;

    addresses.reserve(copy.size());

    while (!copy.empty()) {
      addresses.push_back(copy.top().address());
      copy.pop();
    }

    for (auto it = addresses.rbegin(); it != addresses.rend(); ++it) {
      if (it != addresses.rbegin()) {
        ss << ", ";
      }
      ss << *it;
    }

    ss << "]";
    return ss.str();
  }

  bool evaluate_condition(delayer& d) {
    std::scoped_lock lock(condition_mutex_);
    if (!condition_) {
      return true;
    }
    return condition_->evaluate(d);
  }

  void set_condition(std::unique_ptr<Condition>&& condition) {
    std::scoped_lock lock(condition_mutex_);
    condition_ = std::move(condition);
  }

private:
  std::unique_ptr<Condition> condition_ = nullptr;
  std::mutex condition_mutex_;
};

struct promise_type_base {
  std::weak_ptr<strand> slice_;
  std::exception_ptr except_ = nullptr;
};

template<typename Result>
struct awaitable {
  struct promise_type;

  using Handle_Type = std::coroutine_handle<promise_type>;
  Handle_Type coro_handle_;

  struct promise_type : promise_type_base {
    std::unique_ptr<Result> result_ = nullptr;

    auto get_return_object() {
      return awaitable{Handle_Type::from_promise(*this)};
    }

    // I prefer not to mark them static
    constexpr std::suspend_always initial_suspend() {
      return {};
    }

    constexpr std::suspend_always final_suspend() noexcept {
      return {};
    }

    template<typename... Types>
    void return_value(Types&&... args) {
      result_ = std::make_unique<Result>(std::forward<Types>(args)...);
    }

    void unhandled_exception() {
      // I shall forward the exception, and terminate the thread.
      try {
        except_ = std::current_exception();
        std::rethrow_exception(except_);
      } catch (std::exception& exc) {
        auto slice = slice_.lock();

        if (slice) {
          log::debug(
            "slice: {}; current coroutine: {}; exception what: {}", (unsigned long) slice.get(),
            (unsigned long) slice->addresses_.top().address(), exc.what()
          );
        }
      }
      std::terminate();
    }
  };

  void resume() {
    coro_handle_.resume();
  }

  ~awaitable() {
    // if(coro_handle_) {
    // 	coro_handle_.destroy();
    // 	coro_handle_ = nullptr;
    // }
  }

#pragma region Awaitable_Implementation

  constexpr auto await_ready() {
    return false;
  }

  void await_suspend(std::coroutine_handle<> parent_handle) {
    // using casted_type = std::coroutine_handle<promise_type_base>;
    // auto *p_parent_handle_base = reinterpret_cast<casted_type *>(&parent_handle);
    auto coro_handle = std::coroutine_handle<promise_type_base>::from_address(
      parent_handle.address()
    );

    // auto slice = coro_handle.promise().slice_;
    std::weak_ptr<strand> former_slice = coro_handle.promise().slice_;
    coro_handle_.promise().slice_ = former_slice;
    former_slice.lock()->addresses_.push(coro_handle_);

    // h_.resume behaves as a shortcut, it's not required for normal running,
    // since the stack has been pushed. But it will be much faster if I call it here.
    coro_handle_.resume();
  }

  Result await_resume() {
    Result result = std::move(*coro_handle_.promise().result_);
    coro_handle_.destroy();
    coro_handle_ = nullptr;
    return result;
  }

#pragma endregion
};

template<>
struct awaitable<void> {
  struct promise_type;

  using Handle_Type = std::coroutine_handle<promise_type>;
  Handle_Type coro_handle_;

  struct promise_type : promise_type_base {
    auto get_return_object() {
      return awaitable{Handle_Type::from_promise(*this)};
    }

    constexpr std::suspend_always initial_suspend() {
      return {};
    }

    constexpr std::suspend_always final_suspend() noexcept {
      return {};
    }

    void return_void() {
    }

    void unhandled_exception() {
      try {
        except_ = std::current_exception();
        std::rethrow_exception(except_);
      } catch (std::exception& exc) {
        auto slice = slice_.lock();

        if (slice) {
          log::debug(
            "slice: {}; current coroutine: {}; exception what: {}", (unsigned long) slice.get(),
            (unsigned long) slice->addresses_.top().address(), exc.what()
          );
        }
      }
      std::terminate();
    }
  };

  void resume() {
    coro_handle_.resume();
  }

  ~awaitable() {
  }

  constexpr auto await_ready() {
    return false;
  }

  void await_suspend(std::coroutine_handle<> parent_handle) {
    auto coro_handle = std::coroutine_handle<promise_type_base>::from_address(
      parent_handle.address()
    );

    std::weak_ptr<strand> former_slice = coro_handle.promise().slice_;
    coro_handle_.promise().slice_ = former_slice;

    former_slice.lock()->addresses_.push(coro_handle_);

    coro_handle_.resume();
  }

  void await_resume() {
    coro_handle_.destroy();
    coro_handle_ = nullptr;
  }
};

struct io_context {
  std::vector<std::shared_ptr<strand> > context_;
  // std::list<thread_slice> context_;
  std::mutex mtx_context_;
  std::atomic_bool shutting_down_ = false;

  void shutdown() {
    shutting_down_ = true;
  }

  std::shared_ptr<strand> get_ready_coroutine() {
    std::scoped_lock mtx_context_lock(mtx_context_);

    using context_it = std::vector<std::shared_ptr<strand> >::iterator;

    for (context_it i = context_.begin(); i != context_.end(); ++i) {
      // exchange will return the previous value,
      // if it's true, the coroutine is already running.
      if ((*i)->proceeding_.exchange(true)) {
        continue;
      }
      return *i;
    }
    return nullptr;
  }

  void erase_coroutine(const std::shared_ptr<strand>& target) {
    std::scoped_lock lock(mtx_context_);

    std::erase_if(context_, [&](auto& x) { return x == target; });
  }

  template<typename TAsyncAwaitable>
  void spawn(TAsyncAwaitable&& coro) {
    std::scoped_lock mtx_context_lock(mtx_context_);

    std::shared_ptr<strand> slice = std::make_shared<strand>(*this, coro.coro_handle_);
    coro.coro_handle_.promise().slice_ = slice;
    context_.emplace_back(std::move(slice));
  }

  bool empty() {
    std::scoped_lock lock(mtx_context_);
    return context_.empty();
  }

  void run() {
    delayer local_delayer{};
    while ((!empty()) && (!shutting_down_)) {
      local_delayer.reset();

      std::shared_ptr<strand> sit = get_ready_coroutine();
      // Usually, if a thread is here, there should be something to execute.
      // Because the delayer automatically delays for the timespan to the
      // earliest available coroutines.
      if (!sit) {
        // local_delayer.exec_delay();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::yield();
        continue;
      }

      if constexpr (FELIS_DEBUG) {
        log::debug("selected {}", sit->stacks());
      }
      // If the condition is true, then the thread hasn't bound to
      // any specific thread_slice currently.

      bool finished = false;

      while (true) {
        if (!sit->evaluate_condition(local_delayer)) {
          break;
        }
        // Coroutine can call others and emplace them to the stack in io_context,
        // but it mustn't resume coroutines behind.
        if constexpr (FELIS_DEBUG) {
          log::debug("resuming {}", sit->stacks());
        }
        sit->addresses_.top().resume();

        // The most efficient way for it:
        // direct execute the coroutine by co_await,
        // and append it into the context list.

        // Case 1: The top coroutine appears to be done
        // The struct ensures that only the top coroutine can be done at one execution.
        if (auto& current_top = sit->addresses_.top(); current_top.done()) {
          // destroy() should be called after value is obtained, not here.
          sit->addresses_.pop();
          if (sit->addresses_.empty()) {
            // There's no other parents could destroy the root coroutine.
            current_top.destroy();
            erase_coroutine(sit);
            finished = true;
            break;
          }
        }
        // Case 2: The top coroutine hasn't completed,
        // but still returned handling back.
        // I shall go back to check the conditions...
        // If the condition is still none,
        // there's possibility that the coroutine just invoked a new one,
        // but hierarchy restrict from execution of parent coroutine.
      }
      // sit->proceeding_ must be set after while(true), since it keeps executing it.
      // otherwise other threads may obtain it sneakily.
      sit->proceeding_ = false;

      // A coroutine that finished on its first resume never set a condition, so
      // the delayer still holds its default wait. Sleeping here would stall this
      // thread (and any join on it) for the full default with no work pending.
      if (!finished) {
        local_delayer.exec_delay();
      }
    }
    if constexpr (FELIS_DEBUG) {
      log::debug(
        "io_context::run has ended since no candidate async action is provided"
        "  I guess. it's something you expect"
      );
    }
  }
};

struct deadline_timer {
  // Little struct does not necessarily need move constructor
  struct deadline_timer_condition : strand::Condition {
    std::chrono::system_clock::time_point end_time_point_;

    bool evaluate(delayer& local_delayer) override {
      return local_delayer.before(end_time_point_);
    }

    ~deadline_timer_condition() override = default;

    deadline_timer_condition(std::chrono::system_clock::time_point end_time_point)
      : end_time_point_(end_time_point) {
    }
  };

  struct deadline_timer_awaitable {
    deadline_timer_condition condition_;

    constexpr bool await_ready() {
      return false;
    }

    void await_suspend(std::coroutine_handle<> parent_handle) {
      auto parent_coro_handle = std::coroutine_handle<promise_type_base>::from_address(
        parent_handle.address()
      );

      parent_coro_handle.promise().slice_.lock()->set_condition(
        std::make_unique<deadline_timer_condition>(condition_)
      );
    }

    constexpr void await_resume() {
    }
  };

  auto expires_after(std::chrono::system_clock::duration dura) {
    return deadline_timer_awaitable{{std::chrono::system_clock::now() + dura}};
    // NOTE: use steady_clock to trade performance for precision
  }
};

template<typename T>
concept ImpossibleType = !std::is_same_v<T, T>;

template<typename>
inline constexpr bool always_false_v = false;

template<typename Job>
class multi_executor {
  static_assert(always_false_v<Job>, "multi_executor requires a function type with no parameters");
};

template<typename Result>
class multi_executor<Result()> {
public:
  using job_type = std::function<Result()>;

  // A submitted job's completion state. Copies share it, so the handle can be
  // kept by the submitter while the worker owns its own copy.
  struct completion {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  };

  // A handle to one submitted job: request_stop() asks the job to wind down and
  // wait() blocks until it has exited. A default-constructed handle is already
  // finished, so callers can hold one unconditionally.
  struct handle {
    std::stop_source stop_source;
    std::shared_ptr<completion> state;

    void request_stop() {
      stop_source.request_stop();
    }

    [[nodiscard]] bool finished() const {
      if (!state) {
        return true;
      }

      std::scoped_lock lock(state->mutex);
      return state->done;
    }

    void wait() {
      if (!state) {
        return;
      }

      std::unique_lock lock(state->mutex);
      state->cv.wait(lock, [this] { return state->done; });
    }
  };

  multi_executor(size_t thread_count) {
    for (size_t i = 0; i < thread_count; ++i) {
      ioc.spawn(async_load(i));
    }

    for (size_t i = 0; i < thread_count; ++i) {
      threads.emplace_back([this] { ioc.run(); });
    }
  }

  void push(job_type job) {
    std::scoped_lock lock(queue_mutex_);

    if (shutting_down_) {
      return;
    }
    queue_.push(std::move(job));
  }

  // Schedules `function(stop_token)` and returns a handle that can stop and
  // wait for it. The job runs on one of the executor's own threads; callers
  // never create a thread per task.
  template<typename Function>
    requires std::is_void_v<Result>
  handle spawn(Function&& function) {
    handle result;
    result.state = std::make_shared<completion>();

    auto state = result.state;
    auto source = result.stop_source;
    auto work = std::make_shared<std::decay_t<Function>>(
        std::forward<Function>(function)
    );

    {
      std::scoped_lock lock(queue_mutex_);

      if (shutting_down_) {
        std::scoped_lock done_lock(state->mutex);
        state->done = true;
        state->cv.notify_all();
        return result;
      }

      queue_.push([work, state, source]() mutable {
        try {
          (*work)(source.get_token());
        } catch (...) {
          // A job's failure is its own business; the executor must keep running.
        }

        {
          std::scoped_lock lock(state->mutex);
          state->done = true;
        }
        state->cv.notify_all();
      });
    }

    return result;
  }

  void shutdown() {
    shutting_down_ = true;

    for (auto& thread: threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

private:
  felis::async::awaitable<int> async_load(int id) {
    felis::async::deadline_timer timer;

    while (!shutting_down_) {
      job_type job;

      {
        std::scoped_lock lock(queue_mutex_);
        if (!queue_.empty()) {
          job = std::move(queue_.front());
          queue_.pop();
        }
      }

      if (job) {
        job();
      }

      co_await timer.expires_after(std::chrono::milliseconds(100));
    }

    co_return 0;
  }

  std::queue<job_type> queue_;
  std::mutex queue_mutex_;

  std::atomic_bool shutting_down_ = false;

  felis::async::io_context ioc;
  std::vector<std::thread> threads;
};
} // namespace felis::async
