#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

#include "felis/common.hpp"

namespace felis
{

template <typename T>
struct synchronized
{
	template <typename... Types>
	synchronized(
		Types&&... values)
		requires std::constructible_from<T, Types...>
		: data_(std::forward<Types>(values)...)
	{}

	synchronized() : data_() {}

	synchronized(synchronized&& other)
	noexcept(std::is_nothrow_move_constructible_v<T>)
	: data_([&other]() -> T {
		std::scoped_lock lock(other.mtx_);
		return std::move(other.data_);
	}())
	{}

	synchronized& operator=(synchronized&& other)
	noexcept(std::is_nothrow_move_assignable_v<T>)
	{
		if (this == &other) {
			return *this;
		}
		std::scoped_lock lock(mtx_, other.mtx_);
		data_ = std::move(other.data_);
		return *this;
	}

	synchronized(const synchronized&) = delete;
	synchronized& operator=(const synchronized&) = delete;

	struct guard
	{
		std::unique_lock<std::mutex> lock_;
		T* data_;

		guard(
			std::mutex& mtx, T* data) : lock_(mtx), data_(data)
		{}

		T* operator->()
		{
			return data_;
		}

		T& operator*()
		{
			return *data_;
		}
	};

	struct const_guard
	{
		std::unique_lock<std::mutex> lock_;
		const T* data_;

		const_guard(
			std::mutex& mtx, const T* data) : lock_(mtx), data_(data)
		{}

		const T* operator->() const
		{
			return data_;
		}

		const T& operator*() const
		{
			return *data_;
		}
	};

	guard lock()
	{
		return guard(mtx_, &data_);
	}

	const_guard lock() const
	{
		return const_guard(mtx_, &data_);
	}

  private:
	std::mutex mtx_;
	T data_;
};

template <typename T>
struct event
{
	using function_type = T;
	using return_type = typename function_traits<function_type>::return_type;
	using functional_type = std::function<T>;
	using container_type = std::vector<functional_type>;

	std::mutex invoking_;
	container_type delegates_;

	event(const event&) = delete;
	event operator=(const event&) = delete;

	event() {}
	event(
		size_t reserved)
	{
		delegates_.reserve(reserved);
	}
	event(event&& other)
	noexcept(std::is_nothrow_move_constructible_v<container_type>) :
	delegates_{ [&other]() {
		std::scoped_lock lock(other.invoking_);
		return std::move(other.delegates_);
	}() }
	{}
	event& operator=(
		event&& other) noexcept
	{
		if (this == &other) {
			return *this;
		}
		std::scoped_lock lock(invoking_, other.invoking_);
		delegates_ = std::move(other.delegates_);

		return *this;
	}

	template <typename... Types>
	void subscribe(
		Types&&... values)
		requires std::constructible_from<functional_type, Types...>
	{
		std::scoped_lock lock(invoking_);

		delegates_.emplace_back(std::forward<Types>(values)...);
	}

	// Invocation will lock the container until complete
	template <typename... Types>
	void invoke(
		const Types&... values)
	{
		std::scoped_lock lock(invoking_);

		for (size_t i = 0; i != delegates_.size(); ++i) {
			delegates_.at(i)(values...);
		}
	}

	template <typename... Types>
	void invoke_snapshot(
		const Types&... values)
	{
		container_type delegates;
		{
			std::scoped_lock lock(invoking_);
			delegates = delegates_;
		}
		for (size_t i = 0; i != delegates.size(); ++i) {
			delegates.at(i)(values...);
		}
	}

	// `invoke_all_with_last` invokes all delegates and returns the last result.
	template <typename... Types>
	return_type invoke_all_with_last(
		const Types&... values)
	{
		std::scoped_lock lock(invoking_);

		if (delegates_.empty()) {
			if constexpr (std::is_void_v<return_type>) {
				return;
			}
			else {
				static_assert(std::is_default_constructible_v<return_type>,
							  "Cannot return a value when event has no delegates");
				return return_type{};
			}
		}

		for (size_t i = 0; i != delegates_.size(); ++i) {
			if (i + 1 == delegates_.size()) {
				return delegates_.at(i)(values...);
			}
			delegates_.at(i)(values...);
		}
	}

	// `accumulated_invoke` invokes all delegates and call accumulator for all results
	//   from each delegate.
	template <typename TAccumulator, typename... Types>
	void accumulated_invoke(
		TAccumulator&& accumulator, const Types&... values)
		requires(!std::is_void_v<return_type>)
	{
		std::scoped_lock lock(invoking_);

		for (size_t i = 0; i != delegates_.size(); ++i) {
			accumulator(delegates_.at(i)(values...));
		}
	}
};

}
