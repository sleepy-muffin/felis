#pragma once

#include <filesystem>
#include <map>
#include <variant>
#include <string>

namespace felis::toml {
class Toml {
public:
  using Table = std::map<std::string, Toml>;
  using Value = std::variant<std::monostate, bool, int64_t, double, std::string, Table>;

  Toml() = default;
  explicit Toml(Value value) : value_(std::move(value)) {}

  [[nodiscard]] bool is_table() const;
  [[nodiscard]] bool is_string() const;
  [[nodiscard]] bool is_integer() const;
  [[nodiscard]] bool is_real() const;
  [[nodiscard]] bool is_boolean() const;

  [[nodiscard]] const Toml& at(const std::string& key) const;
  Toml& at(const std::string& key);
  [[nodiscard]] const Toml* find(const std::string& key) const;
  Toml* find(const std::string& key);

  [[nodiscard]] const Table& table() const;
  Table& table();
  [[nodiscard]] std::string string() const;
  [[nodiscard]] int64_t integer() const;
  [[nodiscard]] double real() const;
  [[nodiscard]] bool boolean() const;

private:
  Value value_;
};

Toml parse(std::string_view text);
Toml parse_toml(const std::filesystem::path& path);
}


