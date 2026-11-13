// Regression tests for the value/object/array JSON design.
#include "felis/json.hpp"

#include <iostream>
#include <string>

namespace {

using felis::json::dumps;
using felis::json::parse;
using felis::json::value;

int failures = 0;

void expect(bool condition, const std::string& label) {
  if (condition) {
    return;
  }

  std::cerr << "FAIL: " << label << '\n';
  ++failures;
}

void test_parse() {
  const auto document = parse(
      R"({"name":"felis","count":3,"ratio":1.5,"ok":true,"none":null,"items":["a",2]})"
  );

  expect(document.is_object(), "root is an object");

  const auto& object = document.as_object();
  expect(object.exists("name"), "name exists");
  expect(object.at("name").as_string() == "felis", "name value");
  expect(object.at("count").is_int(), "count is an int");
  expect(object.at("count").get_int() == 3, "count value");
  expect(object.at("ratio").is_double(), "ratio is a double");
  expect(object.at("ratio").get_double() == 1.5, "ratio value");
  expect(!object.at("ratio").is_int(), "ratio is not an int");
  expect(object.at("ok").is_bool(), "ok is a bool");
  expect(object.at("ok").get_bool(), "ok value");
  expect(object.at("none").is_null(), "none is null");

  const auto& items = object.at("items").as_array();
  expect(items.size() == 2, "items size");
  expect(items[0].as_string() == "a", "items[0]");
  expect(items[1].get_int() == 2, "items[1]");
}

void test_dumps() {
  const auto document = parse(R"({"b":[true,null],"a":"x"})");

  expect(dumps(document) == R"({"a":"x","b":[true,null]})", "compact output");
  expect(
      dumps(document, 2) ==
          "{\n  \"a\": \"x\",\n  \"b\": [\n    true,\n    null\n  ]\n}",
      "pretty output"
  );
  expect(dumps(value(42)) == "42", "scalar output");
  expect(dumps(value(std::string("a\"b\n"))) == R"("a\"b\n")", "string escaping");
}

void test_build() {
  value document;
  document["name"] = "felis";
  document["count"] = 3;
  document["items"][0] = true;
  document["items"][1] = "second";
  document["nested"]["enabled"] = false;

  expect(document.is_object(), "indexing creates an object");
  expect(document["name"].as_string() == "felis", "string assignment");
  expect(document["count"].get_int() == 3, "int assignment");
  expect(document["items"].is_array(), "indexing creates an array");
  expect(document["items"].as_array().size() == 2, "array grows on demand");
  expect(document["items"][1].as_string() == "second", "array assignment");
  expect(!document["nested"]["enabled"].get_bool(), "nested assignment");

  expect(
      dumps(document) ==
          R"({"count":3,"items":[true,"second"],"name":"felis","nested":{"enabled":false}})",
      "built document output"
  );
}

void test_errors() {
  bool threw = false;

  try {
    (void)parse(R"({"a":1,"a":2})");
  } catch (const std::exception&) {
    threw = true;
  }
  expect(threw, "duplicate keys throw");

  threw = false;
  try {
    (void)parse("[1,2");
  } catch (const std::exception&) {
    threw = true;
  }
  expect(threw, "truncated input throws");

  const auto document = parse(R"({"a":1})");

  threw = false;
  try {
    (void)document.as_object().at("missing");
  } catch (const std::out_of_range&) {
    threw = true;
  }
  expect(threw, "missing key throws");

  threw = false;
  try {
    (void)document.as_object().at("a").as_array();
  } catch (const std::bad_variant_access&) {
    threw = true;
  }
  expect(threw, "wrong accessor throws");
}

} // namespace

int main() {
  test_parse();
  test_dumps();
  test_build();
  test_errors();

  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }

  std::cout << "json tests passed\n";
  return 0;
}
