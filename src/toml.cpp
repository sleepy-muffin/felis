#include "felis/toml.hpp"

#include <charconv>
#include <fstream>
#include <sstream>


namespace {
std::string toml_trim(const std::string& s) {
  auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::string toml_unquote(const std::string& value, size_t line) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    throw std::runtime_error("TOML line " + std::to_string(line) + ": expected string");
  std::string out;
  for (size_t i = 1; i + 1 < value.size(); ++i) {
    if (value[i] != '\\') {
      out += value[i];
      continue;
    }
    if (++i + 1 >= value.size())
      throw std::runtime_error("invalid TOML escape");
    switch (value[i]) {
      case 'n': out += '\n';
        break;
      case 'r': out += '\r';
        break;
      case 't': out += '\t';
        break;
      case '"': out += '"';
        break;
      case '\\': out += '\\';
        break;
      default: throw std::runtime_error("invalid TOML escape");
    }
  }
  return out;
}

felis::toml::Toml parse_toml_scalar(const std::string& value, size_t line) {
  using Toml = felis::toml::Toml;
  if (value.empty())
    throw std::runtime_error("TOML line " + std::to_string(line) + ": empty value");
  if (value.front() == '"')
    return Toml(toml_unquote(value, line));
  if (value == "true")
    return Toml(true);
  if (value == "false")
    return Toml(false);

  int64_t integer_value;
  auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), integer_value);
  if (ec == std::errc{} && p == value.data() + value.size())
    return Toml(integer_value);

  try {
    size_t n;
    double real_value = std::stod(value, &n);
    if (n == value.size())
      return Toml(real_value);
  } catch (...) {
  }

  throw std::runtime_error("TOML line " + std::to_string(line) + ": unsupported value");
}

felis::toml::Toml::Table& ensure_toml_table(felis::toml::Toml& node) {
  if (!node.is_table())
    node = felis::toml::Toml(felis::toml::Toml::Table{});
  return node.table();
}
} // namespace

bool felis::toml::Toml::is_table() const { return std::holds_alternative<Table>(value_); }
bool felis::toml::Toml::is_string() const { return std::holds_alternative<std::string>(value_); }
bool felis::toml::Toml::is_integer() const { return std::holds_alternative<int64_t>(value_); }
bool felis::toml::Toml::is_real() const { return std::holds_alternative<double>(value_); }
bool felis::toml::Toml::is_boolean() const { return std::holds_alternative<bool>(value_); }

const felis::toml::Toml& felis::toml::Toml::at(const std::string& key) const {
  const auto& t = table();
  auto it = t.find(key);
  if (it == t.end())
    throw std::runtime_error("missing TOML key: " + key);
  return it->second;
}

felis::toml::Toml& felis::toml::Toml::at(const std::string& key) {
  auto& t = table();
  auto it = t.find(key);
  if (it == t.end())
    throw std::runtime_error("missing TOML key: " + key);
  return it->second;
}

const felis::toml::Toml* felis::toml::Toml::find(const std::string& key) const {
  if (!is_table())
    return nullptr;
  const auto& t = std::get<Table>(value_);
  auto it = t.find(key);
  return it == t.end() ? nullptr : &it->second;
}

felis::toml::Toml* felis::toml::Toml::find(const std::string& key) {
  if (!is_table())
    return nullptr;
  auto& t = std::get<Table>(value_);
  auto it = t.find(key);
  return it == t.end() ? nullptr : &it->second;
}

const felis::toml::Toml::Table& felis::toml::Toml::table() const {
  if (!is_table())
    throw std::runtime_error("expected TOML table");
  return std::get<Table>(value_);
}

felis::toml::Toml::Table& felis::toml::Toml::table() {
  if (!is_table())
    throw std::runtime_error("expected TOML table");
  return std::get<Table>(value_);
}

std::string felis::toml::Toml::string() const {
  if (!is_string())
    throw std::runtime_error("expected TOML string");
  return std::get<std::string>(value_);
}

int64_t felis::toml::Toml::integer() const {
  if (!is_integer())
    throw std::runtime_error("expected TOML integer");
  return std::get<int64_t>(value_);
}

double felis::toml::Toml::real() const {
  if (is_real())
    return std::get<double>(value_);
  if (is_integer())
    return static_cast<double>(std::get<int64_t>(value_));
  throw std::runtime_error("expected TOML number");
}

bool felis::toml::Toml::boolean() const {
  if (!is_boolean())
    throw std::runtime_error("expected TOML boolean");
  return std::get<bool>(value_);
}

felis::toml::Toml felis::toml::parse(std::string_view text) {
  Toml root(Toml::Table{});
  Toml* current = &root;
  std::istringstream in{std::string(text)};
  std::string line;
  for (size_t line_no = 1; std::getline(in, line); ++line_no) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
        quoted = !quoted;
      if (line[i] == '#' && !quoted) {
        line.resize(i);
        break;
      }
    }
    line = toml_trim(line);
    if (line.empty())
      continue;

    if (line.front() == '[' && line.back() == ']') {
      auto section = toml_trim(line.substr(1, line.size() - 2));
      if (section.empty())
        throw std::runtime_error("empty TOML section");
      current = &root;
      size_t begin = 0;
      while (begin <= section.size()) {
        auto end = section.find('.', begin);
        auto key = toml_trim(section.substr(begin, end - begin));
        if (key.empty())
          throw std::runtime_error(
            "TOML line " + std::to_string(line_no) + ": empty section key"
          );
        auto& table = ensure_toml_table(*current);
        auto [it, inserted] = table.try_emplace(key, Toml(Toml::Table{}));
        if (!it->second.is_table())
          throw std::runtime_error(
            "TOML line " + std::to_string(line_no) + ": section conflicts with value"
          );
        current = &it->second;
        if (end == std::string::npos)
          break;
        begin = end + 1;
      }
      continue;
    }

    auto eq = line.find('=');
    if (eq == std::string::npos)
      throw std::runtime_error(
        "TOML line " + std::to_string(line_no) + ": expected key=value"
      );
    auto key = toml_trim(line.substr(0, eq));
    auto value = toml_trim(line.substr(eq + 1));
    if (key.empty())
      throw std::runtime_error("TOML line " + std::to_string(line_no) + ": empty key");

    auto& table = ensure_toml_table(*current);
    if (!table.emplace(key, parse_toml_scalar(value, line_no)).second)
      throw std::runtime_error("duplicate TOML key: " + key);
  }
  return root;
}

felis::toml::Toml felis::toml::parse_toml(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open config: " + path.string());
  return parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}
