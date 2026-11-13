#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <utility>

#define MMC_EXPORT

#if !defined(FELIS_FUNCTION)
#if defined(_MSC_VER)

#define FELIS_FUNCTION __FUNCTION__
#elif defined(__GNUC__) || defined(__clang__)
#define FELIS_FUNCTION __PRETTY_FUNCTION__
#endif
#endif

namespace felis {
constexpr bool FELIS_DEBUG = false;

template<class>
struct function_traits;

template<class TRes, class... TArgs>
struct function_traits<TRes(TArgs...)> {
  using return_type = TRes;
};

template<class TRes, class... TArgs>
struct function_traits<TRes (*)(TArgs...)> : function_traits<TRes(TArgs...)> {
};

template <typename F>
struct iter_args {
  template <class... Args>
  inline F& invoke() {
    return self();
  }

  template <typename T, typename... Args>
  inline F& invoke(T&& arg, Args&&... args) {
    self()(std::forward<T>(arg));
    return invoke(std::forward<Args>(args)...);
  }

  // template <typename... Args>
  // inline F& operator()(Args&&... args) {
  //   return self()(std::forward<Args>(args)...);
  // }
  //
  // template <typename T>
  // void operator()(const std::vector<T>& args) {
  //   for (const auto& arg : args) {
  //     self()(arg);
  //   }
  // }
private:
  inline F& self() {
    return *static_cast<F *>(this);
  }
};
}
