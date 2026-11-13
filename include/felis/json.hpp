#pragma once

#include <concepts>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace felis::json {

class value;

namespace detail {
struct serializer;
}

// A JSON object: string keys mapped to values. Keys are kept sorted, as in
// std::map. Accessing a missing key through at() throws, while operator[]
// inserts a null value.
class object {
public:
  using object_container = std::map<std::string, value>;

  object() = default;

  [[nodiscard]] value& at(const std::string& key);
  [[nodiscard]] const value& at(const std::string& key) const;

  [[nodiscard]] bool exists(const std::string& key) const;

  [[nodiscard]] value& operator[](const std::string& key);
  [[nodiscard]] const value& operator[](const std::string& key) const;

  // Inserts only when the key is absent; returns false when it already exists.
  bool insert(std::string key, value item);

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const;

private:
  friend struct detail::serializer;

  object_container values_;
};

// A JSON array: an ordered sequence of values.
class array {
public:
  using array_container = std::vector<value>;

  array() = default;

  [[nodiscard]] value& at(std::size_t index);
  [[nodiscard]] const value& at(std::size_t index) const;

  [[nodiscard]] value& operator[](std::size_t index);
  [[nodiscard]] const value& operator[](std::size_t index) const;

  void push_back(value item);

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const;

private:
  friend struct detail::serializer;

  array_container values_;
};

// A JSON value. Numbers are stored as doubles; is_int()/get_int() are
// convenience accessors for doubles that hold an integral value.
class value {
public:
  using value_container =
      std::variant<std::nullptr_t, bool, double, std::string, array, object>;

  value() : value_(nullptr) {}
  value(std::nullptr_t) : value_(nullptr) {}
  value(bool item) : value_(item) {}
  value(double item) : value_(item) {}
  value(std::string item) : value_(std::move(item)) {}
  value(const char* item) : value_(std::string(item)) {}
  value(array item) : value_(std::move(item)) {}
  value(object item) : value_(std::move(item)) {}

  // Integers are stored as doubles, the only numeric representation.
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  value(T item) : value_(static_cast<double>(item)) {}

  [[nodiscard]] bool is_null() const;

  [[nodiscard]] bool is_bool() const;
  [[nodiscard]] bool get_bool() const;

  [[nodiscard]] bool is_int() const;
  [[nodiscard]] int get_int() const;

  [[nodiscard]] bool is_double() const;
  [[nodiscard]] double get_double() const;

  [[nodiscard]] bool is_string() const;
  [[nodiscard]] std::string& as_string();
  [[nodiscard]] const std::string& as_string() const;

  [[nodiscard]] bool is_array() const;
  [[nodiscard]] array& as_array();
  [[nodiscard]] const array& as_array() const;

  [[nodiscard]] bool is_object() const;
  [[nodiscard]] object& as_object();
  [[nodiscard]] const object& as_object() const;

  // Indexing forwards to as_object()/as_array(). A null value is promoted to
  // the matching container, so `doc["key"] = "value"` also works on a fresh
  // value. A value of the wrong kind throws std::bad_variant_access.
  [[nodiscard]] value& operator[](const std::string& key);
  [[nodiscard]] const value& operator[](const std::string& key) const;
  [[nodiscard]] value& operator[](std::size_t index);
  [[nodiscard]] const value& operator[](std::size_t index) const;

private:
  friend struct detail::serializer;

  value_container value_;
};

[[nodiscard]] value parse(std::string_view text);
[[nodiscard]] value loads(std::string_view path);

// indentation == 0 writes compact JSON; any positive value pretty-prints with
// that many spaces per level.
[[nodiscard]] std::string dumps(const value& item, int indentation = 0);
[[nodiscard]] std::string dumps(const object& item, int indentation = 0);

} // namespace felis::json
