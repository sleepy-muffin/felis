#include "felis/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "felis/file/file.hpp"

namespace felis::json {
namespace {

class parser {
public:
  explicit parser(std::string_view input)
    : input_(input) {
  }

  value parse() {
    auto result = parse_value();
    ws();
    if (pos_ != input_.size())
      fail("trailing data");
    return result;
  }

private:
  std::string_view input_;
  std::size_t pos_ = 0;

  [[noreturn]] void fail(const char* what) const {
    throw std::runtime_error("JSON byte " + std::to_string(pos_) + ": " + what);
  }

  void ws() {
    while (pos_ < input_.size() && std::string_view(" \t\r\n").find(input_[pos_]) !=
                                       std::string_view::npos)
      ++pos_;
  }

  bool take(char c) {
    ws();
    if (pos_ < input_.size() && input_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void literal(std::string_view text) {
    if (input_.substr(pos_, text.size()) != text)
      fail("invalid literal");
    pos_ += text.size();
  }

  value parse_value() {
    ws();
    if (pos_ == input_.size())
      fail("expected value");

    switch (input_[pos_]) {
      case 'n': literal("null");
        return value();
      case 't': literal("true");
        return value(true);
      case 'f': literal("false");
        return value(false);
      case '"': return value(parse_string());
      case '[': return value(parse_array());
      case '{': return value(parse_object());
      default: return value(parse_number());
    }
  }

  static void utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7f) {
      out += static_cast<char>(code_point);
    } else if (code_point <= 0x7ff) {
      out += static_cast<char>(0xc0 | code_point >> 6);
      out += static_cast<char>(0x80 | (code_point & 63));
    } else if (code_point <= 0xffff) {
      out += static_cast<char>(0xe0 | code_point >> 12);
      out += static_cast<char>(0x80 | ((code_point >> 6) & 63));
      out += static_cast<char>(0x80 | (code_point & 63));
    } else {
      out += static_cast<char>(0xf0 | code_point >> 18);
      out += static_cast<char>(0x80 | ((code_point >> 12) & 63));
      out += static_cast<char>(0x80 | ((code_point >> 6) & 63));
      out += static_cast<char>(0x80 | (code_point & 63));
    }
  }

  std::uint32_t hex4() {
    std::uint32_t result = 0;
    for (int n = 0; n < 4; ++n) {
      if (pos_ >= input_.size())
        fail("short unicode escape");

      const char c = input_[pos_++];
      result <<= 4;

      if (c >= '0' && c <= '9')
        result += static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        result += static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        result += static_cast<std::uint32_t>(c - 'A' + 10);
      else
        fail("bad unicode escape");
    }
    return result;
  }

  std::string parse_string() {
    if (!take('"'))
      fail("expected string");

    std::string result;

    while (pos_ < input_.size()) {
      char c = input_[pos_++];

      if (c == '"')
        return result;
      if (static_cast<unsigned char>(c) < 0x20)
        fail("control in string");
      if (c != '\\') {
        result += c;
        continue;
      }
      if (pos_ == input_.size())
        fail("short escape");

      c = input_[pos_++];

      switch (c) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          auto code_point = hex4();

          if (code_point >= 0xd800 && code_point <= 0xdbff) {
            if (input_.substr(pos_, 2) != "\\u")
              fail("missing low surrogate");

            pos_ += 2;
            const auto low = hex4();

            if (low < 0xdc00 || low > 0xdfff)
              fail("bad low surrogate");

            code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
          }

          utf8(result, code_point);
          break;
        }
        default: fail("bad escape");
      }
    }

    fail("unterminated string");
  }

  double parse_number() {
    ws();

    const char* first = input_.data() + pos_;
    const char* last = input_.data() + input_.size();

    double result = 0.0;
    const auto [ptr, error] = std::from_chars(first, last, result);

    if (error != std::errc() || ptr == first)
      fail("expected number");

    pos_ += static_cast<std::size_t>(ptr - first);

    if (!std::isfinite(result))
      fail("non-finite number");

    return result;
  }

  array parse_array() {
    take('[');

    array result;
    if (take(']'))
      return result;

    do {
      result.push_back(parse_value());
    } while (take(','));

    if (!take(']'))
      fail("expected ]");

    return result;
  }

  object parse_object() {
    take('{');

    object result;
    if (take('}'))
      return result;

    do {
      ws();
      auto key = parse_string();

      if (!take(':'))
        fail("expected :");

      if (!result.insert(std::move(key), parse_value()))
        fail("duplicate key");
    } while (take(','));

    if (!take('}'))
      fail("expected }");

    return result;
  }
};

} // namespace

namespace detail {

struct serializer {
  std::string out;
  int indentation = 0;

  explicit serializer(int indent)
    : indentation(indent) {
  }

  [[nodiscard]] bool pretty() const {
    return indentation > 0;
  }

  void newline(int level) {
    out += '\n';
    out.append(static_cast<std::size_t>(level * indentation), ' ');
  }

  void append_string(const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";

    out += '"';

    for (const unsigned char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0x0f];
          } else {
            out += static_cast<char>(c);
          }
          break;
      }
    }

    out += '"';
  }

  void dump(const value& item, int level) {
    std::visit(
        [&]<typename T0>(const T0& alternative) {
          using type = std::decay_t<T0>;

          if constexpr (std::is_same_v<type, std::nullptr_t>) {
            out += "null";
          } else if constexpr (std::is_same_v<type, bool>) {
            out += alternative ? "true" : "false";
          } else if constexpr (std::is_same_v<type, double>) {
            if (!std::isfinite(alternative))
              throw std::runtime_error("cannot serialize non-finite JSON number");

            char buffer[64];
            const auto [ptr, error] = std::to_chars(
                std::begin(buffer), std::end(buffer), alternative
            );

            if (error != std::errc())
              throw std::runtime_error("cannot serialize JSON number");

            out.append(buffer, ptr);
          } else if constexpr (std::is_same_v<type, std::string>) {
            append_string(alternative);
          } else if constexpr (std::is_same_v<type, array>) {
            dump_array(alternative, level);
          } else if constexpr (std::is_same_v<type, object>) {
            dump_object(alternative, level);
          }
        },
        item.value_
    );
  }

  void dump_array(const array& item, int level) {
    out += '[';

    bool first = true;
    for (const auto& element : item.values_) {
      if (first)
        first = false;
      else
        out += ',';

      if (pretty())
        newline(level + 1);

      dump(element, level + 1);
    }

    if (!first && pretty())
      newline(level);

    out += ']';
  }

  void dump_object(const object& item, int level) {
    out += '{';

    bool first = true;
    for (const auto& [key, element] : item.values_) {
      if (first)
        first = false;
      else
        out += ',';

      if (pretty())
        newline(level + 1);

      append_string(key);
      out += pretty() ? ": " : ":";
      dump(element, level + 1);
    }

    if (!first && pretty())
      newline(level);

    out += '}';
  }
};

} // namespace detail

value parse(std::string_view text) {
  return parser(text).parse();
}

value loads(std::string_view path) {
  return parse(felis::file::read(path));
}

std::string dumps(const value& item, int indentation) {
  detail::serializer serializer(indentation);
  serializer.dump(item, 0);
  return std::move(serializer.out);
}

std::string dumps(const object& item, int indentation) {
  detail::serializer serializer(indentation);
  serializer.dump_object(item, 0);
  return std::move(serializer.out);
}

bool value::is_null() const {
  return std::holds_alternative<std::nullptr_t>(value_);
}

bool value::is_bool() const {
  return std::holds_alternative<bool>(value_);
}

bool value::get_bool() const {
  return std::get<bool>(value_);
}

bool value::is_int() const {
  const auto* number = std::get_if<double>(&value_);

  if (number == nullptr || !std::isfinite(*number) || std::trunc(*number) != *number)
    return false;

  return *number >= static_cast<double>(std::numeric_limits<int>::min()) &&
         *number <= static_cast<double>(std::numeric_limits<int>::max());
}

int value::get_int() const {
  if (!is_int())
    throw std::runtime_error("JSON value is not an integer");

  return static_cast<int>(std::get<double>(value_));
}

bool value::is_double() const {
  return std::holds_alternative<double>(value_);
}

double value::get_double() const {
  return std::get<double>(value_);
}

bool value::is_string() const {
  return std::holds_alternative<std::string>(value_);
}

std::string& value::as_string() {
  return std::get<std::string>(value_);
}

const std::string& value::as_string() const {
  return std::get<std::string>(value_);
}

bool value::is_array() const {
  return std::holds_alternative<array>(value_);
}

array& value::as_array() {
  return std::get<array>(value_);
}

const array& value::as_array() const {
  return std::get<array>(value_);
}

bool value::is_object() const {
  return std::holds_alternative<object>(value_);
}

object& value::as_object() {
  return std::get<object>(value_);
}

const object& value::as_object() const {
  return std::get<object>(value_);
}

value& value::operator[](const std::string& key) {
  if (is_null())
    value_ = object{};

  return as_object()[key];
}

const value& value::operator[](const std::string& key) const {
  return as_object().at(key);
}

value& value::operator[](std::size_t index) {
  if (is_null())
    value_ = array{};

  auto& items = as_array();

  while (items.size() <= index)
    items.push_back(value{});

  return items[index];
}

const value& value::operator[](std::size_t index) const {
  return as_array().at(index);
}

value& object::at(const std::string& key) {
  const auto found = values_.find(key);

  if (found == values_.end())
    throw std::out_of_range("missing JSON key: " + key);

  return found->second;
}

const value& object::at(const std::string& key) const {
  const auto found = values_.find(key);

  if (found == values_.end())
    throw std::out_of_range("missing JSON key: " + key);

  return found->second;
}

bool object::exists(const std::string& key) const {
  return values_.find(key) != values_.end();
}

value& object::operator[](const std::string& key) {
  return values_.try_emplace(key).first->second;
}

const value& object::operator[](const std::string& key) const {
  return at(key);
}

bool object::insert(std::string key, value item) {
  return values_.emplace(std::move(key), std::move(item)).second;
}

std::size_t object::size() const {
  return values_.size();
}

bool object::empty() const {
  return values_.empty();
}

value& array::at(std::size_t index) {
  if (index >= values_.size())
    throw std::out_of_range("JSON array index out of range");

  return values_[index];
}

const value& array::at(std::size_t index) const {
  if (index >= values_.size())
    throw std::out_of_range("JSON array index out of range");

  return values_[index];
}

value& array::operator[](std::size_t index) {
  return values_[index];
}

const value& array::operator[](std::size_t index) const {
  return values_[index];
}

void array::push_back(value item) {
  values_.push_back(std::move(item));
}

std::size_t array::size() const {
  return values_.size();
}

bool array::empty() const {
  return values_.empty();
}

} // namespace felis::json
