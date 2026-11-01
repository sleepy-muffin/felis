

#include <boost/json.hpp>

#include "shark/log.hpp"
#include "shark/file/file.hpp"

std::string shark::file::read(
  std::string_view path
) {
  // Warning std::string_view is not null-terminated guaranteed.
  std::ifstream file(std::string(path), std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    shark::raise("Cannot open file: {}", path);
  }

  std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return str;
}

std::vector<char> shark::file::read_binary(
  const std::string& filename
) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("Failed to open file: " + filename);
  size_t size = file.tellg();
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), size);
  return buffer;
}

shark::file::tlm_node shark::file::tlm_node_read(
  std::string_view file_path
) {
  const std::string identifier = "=^..^= ";
  std::string ctx = shark::file::read(file_path);

  decltype(ctx)::iterator ctx_begin = ctx.begin();

  using ty_indexes = std::vector<std::string>;
  ty_indexes indexes;

  tlm_node node;
  auto mk_doc = [](tlm_node& node, std::string_view doc, const ty_indexes& indexes) {
    tlm_node* idx = &node;
    for (const auto& index: indexes) {
      idx = &(*idx)[index];
    }
    idx->set_doc(doc);
  };

  auto decribe_id = [](std::string_view id, ty_indexes& indexes) {
    short inherit_from = -1;

    std::string_view::iterator id_begin = id.end();
    for (auto id_i = id.begin(); id_i != id.end(); ++id_i) {
      if (std::isspace(static_cast<unsigned char>(*id_i))) {
        continue;
      }
      if (inherit_from == -1) {
        if (*id_i == ':') {
          inherit_from = 1;
          continue;
        } else {
          inherit_from = 0;
          indexes.clear();
        }
      }
      if (id_begin == id.end()) {
        id_begin = id_i;
      }

      if (*id_i == ':') {
        indexes.emplace_back(id_begin, id_i);
        id_begin = id_i + 1;
      }
    }
  };

  for (size_t pos = 0; pos < ctx.size(); ++pos) {
    if (pos == 0 ? true : ctx.at(pos - 1) == '\n') {
      if (std::string_view(ctx.data() + pos, identifier.size()).compare(identifier) == 0) {
        // line of the key
        if (!indexes.empty()) {
          auto tmp_begin = ctx_begin + 1;
          auto tmp_end = ctx.begin() + pos - 1;
          mk_doc(
            node, tmp_end > tmp_begin ? std::string_view{tmp_begin, tmp_end} : "",
            indexes
          );
        }

        size_t pos_next = ctx.find_first_of('\n', pos); // Move to the next new line
        size_t ps_id_begin = pos + identifier.size();

        std::string_view id(ctx.data() + ps_id_begin, pos_next - ps_id_begin);
        decribe_id(id, indexes);

        // lit::log::info("id:{}", id);

        ctx_begin = ctx.begin() + pos_next;
      }
      // Not identifier line, don't care about it.'
    }
  }

  // node.print();
  return node;
}

bool shark::file::is_binary_file(const std::filesystem::path& path) {
  // A file counts as binary when a NUL byte appears in its first block. This is
  // the same convention git and ripgrep use, and it lets search_code cover
  // every text language without maintaining an extension whitelist.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return true;
  }

  std::array<char, 8192> buffer{};
  in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  const std::streamsize count = in.gcount();

  return std::find(buffer.begin(), buffer.begin() + count, '\0') !=
      buffer.begin() + count;
}


std::string shark::file::tlm_node::print(
  unsigned short indent
) const {
  std::ostringstream ss;
  std::string indent_str(indent, ' ');

  // --- children ---
  if (children) {
    for (const auto& [key, child]: *children) {
      ss << indent_str << "child:" << key << ":\n";
      ss << child.print(indent + 2);
    }
  }

  // --- doc (word wrapped, aligned) ---
  if (doc) {
    std::string_view text = *doc;

    std::string prefix = indent_str + "doc: ";
    std::string continuation(prefix.size(), ' ');

    bool first_line_global = true;

    size_t start = 0;
    while (start <= text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string_view::npos)
        end = text.size();

      std::string_view line = text.substr(start, end - start);

      // --- empty line (preserve it) ---
      if (line.empty()) {
        if (first_line_global) {
          ss << prefix << '\n';
          first_line_global = false;
        } else {
          ss << continuation << '\n';
        }
      } else {
        // --- word wrap THIS line only ---
        size_t pos = 0;
        bool first_line_local = true;

        while (pos < line.size()) {
          size_t line_start = pos;
          size_t len = 0;
          size_t last_space = std::string_view::npos;

          size_t limit = 128 - prefix.size();

          while (pos < line.size() && len < limit) {
            if (std::isspace(static_cast<unsigned char>(line[pos]))) {
              last_space = pos;
            }
            ++pos;
            ++len;
          }

          if (pos < line.size() && last_space != std::string_view::npos &&
              last_space > line_start) {
            pos = last_space + 1;
            len = last_space - line_start;
          }

          while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
          }

          if (first_line_global) {
            ss << prefix;
            first_line_global = false;
          } else if (first_line_local) {
            ss << continuation;
          } else {
            ss << continuation;
          }

          ss << line.substr(line_start, len) << '\n';
          first_line_local = false;
        }
      }

      start = end + 1;
    }
  }

  return ss.str();
}

boost::json::object shark::file::read_json(
	std::string_view path)
{
	namespace json = boost::json;
	auto raw = boost::json::parse(read(path));
	return raw.as_object();
}

void shark::file::dump_json(
	std::string_view path, const boost::json::object& js)
{
	namespace json = boost::json;
	std::string pretty = boost::json::serialize(js);

	std::ofstream(std::string(path)) << pretty;
}
