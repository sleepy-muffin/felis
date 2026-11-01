#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shark::file {
class tlm_node {
  // Touhou Little Marks

public:
  void set_doc(std::string_view content) { doc = std::make_unique<std::string>(content); }

  [[nodiscard]] bool has_doc() const { return doc != nullptr; }

  [[nodiscard]] std::optional<std::string_view> get_doc() const {
    return doc != nullptr ? std::optional<std::string_view>{*doc}
                          : std::optional<std::string_view>{};
  }

  // std::string_view doesn't gaurantee null-terminate
  tlm_node &get_child(std::string_view key) {
    if (children == nullptr) {
      children = std::make_unique<decltype(children)::element_type>();
    }

    auto p = children->try_emplace(std::string(key));
    return p.first->second;
  }

  template<typename Ty>
  tlm_node &operator[](Ty &&key) {
    return get_child(std::forward<Ty>(key));
  }

  [[nodiscard]] std::string print(unsigned short indent = 0) const;

  [[nodiscard]] std::optional<std::reference_wrapper<const std::map<std::string, tlm_node>>>
  c_get_children() const {
    if (children) {
      return *children;
    }
    return {};
  }

private:
  // lit::print("{}  {}", sizeof (std::map<std::string, tlm_node>), sizeof
  // (std::unique_ptr<std::map<std::string, tlm_node>>));
  std::unique_ptr<std::map<std::string, tlm_node>> children = nullptr;
  std::unique_ptr<std::string> doc = nullptr;
};

std::string read(std::string_view path);
std::vector<char> read_binary(const std::string &filename);
tlm_node tlm_node_read(std::string_view path);

// True when the file looks binary (a NUL byte in its first block).
bool is_binary_file(const std::filesystem::path &path);

}; // namespace shark::file
