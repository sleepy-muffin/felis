#pragma once

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <random>

#include "felis/str.hpp"

namespace felis::math
{

namespace
{
struct _internal
{
	std::random_device rd;
	std::mt19937 gen;
	std::uniform_real_distribution<float> dis;

	_internal() : gen(rd()) {}
};

static std::unique_ptr<_internal> static_random_ = nullptr;
static _internal& instance()
{
	if (!static_random_) {
		static_random_ = std::make_unique<_internal>();
	}
	return *static_random_;
}
} // namespace

inline float random_float(
	const float begin = 0.0f, const float end = 1.0f)
{
	auto& ins = instance();
	ins.dis.param(typename decltype(ins.dis)::param_type{begin, end});
	return ins.dis(ins.gen);
}

inline int random_int(
	const int begin, const int end)
{
	float r = random_float();
	return begin + (int)(r * (end - begin));
}

} // namespace felis::math
