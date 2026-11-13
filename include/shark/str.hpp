#pragma once
#include <charconv>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <variant>

#ifndef __cplusplus
#error "This header requires C++"
#endif

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "shark/common.hpp"

namespace shark::str {
std::string replace(std::string_view input, std::string_view from, std::string_view to);

inline std::vector<std::string> split(const std::string& text) {
	std::vector<std::string> result;

	size_t i = 0;

	while (i < text.size()) {
		// Skip whitespace
		while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
			++i;
		}

		if (i >= text.size())
			break;

		size_t start = i;

		// Read token
		while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
			++i;
		}

		result.emplace_back(text.substr(start, i - start));
	}

	return result;
}

inline std::vector<std::string> split(const std::string& text, char delimiter) {
	std::vector<std::string> result;

	size_t start = 0;

	while (true) {
		size_t pos = text.find(delimiter, start);

		if (pos == std::string::npos) {
			result.emplace_back(text.substr(start));
			break;
		}

		result.emplace_back(text.substr(start, pos - start));
		start = pos + 1;
	}

	return result;
}

[[nodiscard]] std::string trim(const std::string &text);
[[nodiscard]] unsigned short leading_space_count(const std::string& s);
[[nodiscard]] bool is_empty_or_whitespace(const std::string& s);
void remove_whitespace(std::string& s);
size_t utf8_codepoints(std::string_view s);
size_t estimate_tokens(std::string_view s);

// --- UTF-8 guarding -----------------------------------------------------
//
// Byte-safe helpers used wherever text reaches a strict JSON wire. A fetched
// page or a file read can carry bytes from another encoding, and cutting text at
// a byte limit can split a multi-byte code point. Either way the request body
// stops being valid UTF-8 and a strict server rejects the whole turn, so
// everything that reaches the wire is passed through sanitize_utf8() and cut with
// truncate_utf8(). Invalid bytes are treated as one byte each.

// Byte length of the longest prefix of `text` that is at most `max_bytes` long
// and ends on a code-point boundary.
std::size_t utf8_cut_length(std::string_view text, std::size_t max_bytes);

// `text` with every byte that is not part of a well-formed UTF-8 sequence
// replaced by U+FFFD.
std::string sanitize_utf8(std::string_view text);

// The longest prefix of `text` that is at most `max_bytes` long and ends on a
// code-point boundary. Unlike sanitize_utf8() it introduces no replacement
// characters; use it to truncate text that is already valid UTF-8.
std::string truncate_utf8(std::string_view text, std::size_t max_bytes);
[[nodiscard]] std::string to_lower(const std::string& s);

[[nodiscard]] std::string make_uuid();
[[nodiscard]] std::string now_iso8601_utc();

template<typename T>
std::tuple<T, T> from_interval_text(std::string_view interval) {
	std::string intr = str::replace(interval, " ", "");
}

void forcibly_print_string(const std::string& input);
void forcibly_print_vector(const std::vector<std::string>& input);

void indented_print(std::string_view s, char c = ' ', int n = 4);
std::string indent(std::string_view s, char c = ' ', int n = 4);

} // namespace shark::str
