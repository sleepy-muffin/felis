/*
 * Project: shark
 * Repository: https://github.com/leaked32/shark
 *
 * File: shark/src/str.cpp
 *
 * License: MIT
 */

#include "shark/str.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "shark/log.hpp"

#include <fstream>
#include <sstream>

std::string
shark::str::replace(std::string_view input, std::string_view from, std::string_view to) {
  using T = decltype(input);

  const size_t len_from = from.size();
  const size_t len_to = to.size();
  const size_t len_input = input.size();

  size_t pre_count = 0;
  for (size_t occur = 0; occur = input.find(from, occur), occur != T::npos;) {
    occur += len_from;
    pre_count += 1ul;
  }

  if (pre_count == 0) {
    throw pinch("expression not fonud");
  }

  size_t output_capacity = len_input + (len_to - len_from) * pre_count;
  std::string output;
  output.resize(output_capacity);

  // std::println("{} {} {} {}", input, from, to, pre_count);

  size_t it_last = 0;
  size_t it_count = 0;
  for (size_t it = 0; it = input.find(from, it), it != T::npos;) {
    // std::println("{} ", output);

    // 1. Copy the buffer before the found substring to the output, the
    // begin of the buffer should be last time, last_it = it + len_from
    std::copy(
        input.begin() + it_last, input.begin() + it,
        output.begin() + it_last + it_count * (len_to - len_from));
    // 2. Copy the new substring to the output
    std::copy(to.begin(), to.end(), output.begin() + it + it_count * (len_to - len_from));

    it += len_from;
    it_last = it;

    it_count += 1;
  }
  // std::println("{} {}", output.size(), output.length(), it_last + it_count
  // * (len_to - len_from)); size() does not count the ending \0 by
  // std::string, but \0 I put is counted, so size() and length() can't be
  // used.
  // 3. Copy the tail buffer
  std::copy(
      input.begin() + it_last, input.end(),
      output.begin() + it_last + it_count * (len_to - len_from));

  return output;
}

bool shark::str::is_empty_or_whitespace(const std::string &s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

// std::optional<std::string> shark::str::trim(const std::string &s) {
//   if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); })) {
//     return {};
//   }
//
//   auto start =
//       std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
//
//   auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) {
//                return std::isspace(c);
//              }).base();
//
//   if (start >= end)
//     return "";
//
//
//   return std::string(start, end);
// }
std::string trim(const std::string &text) {
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  const auto begin = std::find_if(text.begin(), text.end(), not_space);
  const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();

  if (begin >= end) {
    return {};
  }

  return std::string(begin, end);
}

unsigned short shark::str::leading_space_count(const std::string &s) {
  unsigned short count = 0;
  auto start =
      std::find_if_not(s.begin(), s.end(), [&count](unsigned char c) { return std::isspace(c); });

  return count;
}

void shark::str::remove_whitespace(std::string &s) {
  s.erase(
      std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
}


size_t shark::str::utf8_codepoints(std::string_view s) {
  size_t count = 0;

  for (unsigned char c: s) {
    if ((c & 0xC0) != 0x80)
      ++count;
  }

  return count;
}

// ASCII lower-case, used by the smart-case search. A locale-independent fold
// keeps the matching behaviour stable across platforms.
std::string shark::str::to_lower(const std::string &s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
  return result;
}

size_t shark::str::estimate_tokens(std::string_view s) {
  size_t ascii = 0;
  size_t non_ascii_chars = 0;

  for (unsigned char c: s) {
    if (c < 128)
      ++ascii;
    else if ((c & 0xC0) != 0x80)
      ++non_ascii_chars;
  }

  return (ascii + 3) / 4 + non_ascii_chars;
}


void shark::str::forcibly_print_vector(const std::vector<std::string> &input) {
  // std::cout << "\tdebug forcely_print_vector: ";
  for (size_t i = 0; i < input.size(); ++i) {
    // std::cout << i << ":";
    forcibly_print_string(input.at(i));
    std::cout << ", ";
  }
  // std::cout << std::endl;
}

void shark::str::forcibly_print_string(const std::string &input) {
  // std::cout << "";
  for (size_t i = 0; i < input.size(); ++i) {
    std::cout << (char) (*(input.data() + i));
  }
  // std::cout << std::endl;
}


void shark::str::indented_print(std::string_view s, char c, int n) {}

std::string shark::str::indent(std::string_view s, char c, int n) {
  if (s.empty() || n <= 0)
    return std::string(s);
  std::string prefix(n, c);
  std::string result;
  result.reserve(s.size() + prefix.size() * 32); // safe overestimate

  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string_view::npos) {
      result += prefix;
      result += s.substr(pos);
      break;
    }
    result += prefix;
    result += s.substr(pos, nl - pos + 1);
    pos = nl + 1;
  }
  return result;
}


std::string shark::str::make_uuid() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  std::uniform_int_distribution<std::uint32_t> distribution(0, 0xFFFFFFFFu);

  std::uint32_t parts[4];
  for (auto &part: parts) {
    part = distribution(engine);
  }

  // Set the version (4) and variant (RFC 4122) bits so this is a valid v4 UUID.
  parts[1] = (parts[1] & 0xFFFF0FFFu) | 0x00004000u;
  parts[2] = (parts[2] & 0x3FFFFFFFu) | 0x80000000u;

  char buffer[37];
  std::snprintf(
      buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%04x%08x", parts[0], (parts[1] >> 16) & 0xFFFFu,
      parts[1] & 0xFFFFu, (parts[2] >> 16) & 0xFFFFu, parts[2] & 0xFFFFu, parts[3]);
  return buffer;
}


std::string shark::str::now_iso8601_utc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);

  std::tm broken_down{};
#if defined(_WIN32)
  gmtime_s(&broken_down, &seconds);
#else
  gmtime_r(&seconds, &broken_down);
#endif

  char buffer[32];
  const std::size_t written =
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken_down);
  return std::string(buffer, written);
}
