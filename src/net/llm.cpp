/*
 * Project: felis
 * Repository: https://github.com/leaked32/felis
 *
 * File: felis/src/llm.cpp
 *
 * License: MIT
 */

#include "felis/net/llm.hpp"

#include "felis/log.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace felis::llm {

namespace {

const boost::json::object* object_at(
    const boost::json::object& object, std::string_view key
) {
  const boost::json::value* value = object.if_contains(key);
  return value != nullptr && value->is_object() ? &value->as_object() : nullptr;
}

const boost::json::array* array_at(
    const boost::json::object& object, std::string_view key
) {
  const boost::json::value* value = object.if_contains(key);
  return value != nullptr && value->is_array() ? &value->as_array() : nullptr;
}

std::string string_at(const boost::json::object& object, std::string_view key) {
  const boost::json::value* value = object.if_contains(key);

  return value != nullptr && value->is_string()
      ? std::string(value->as_string())
      : std::string{};
}

int int_at(const boost::json::object& object, std::string_view key, int fallback) {
  const boost::json::value* value = object.if_contains(key);

  if (value == nullptr || !value->is_number()) {
    return fallback;
  }

  if (value->is_int64()) {
    return static_cast<int>(value->as_int64());
  }

  if (value->is_uint64()) {
    return static_cast<int>(value->as_uint64());
  }

  return static_cast<int>(value->as_double());
}

// Reads a non-negative count, which is how every `usage` field is shaped.
std::size_t size_at(
    const boost::json::object& object, std::string_view key,
    std::size_t fallback = 0
) {
  const boost::json::value* value = object.if_contains(key);

  if (value == nullptr) {
    return fallback;
  }

  if (value->is_int64()) {
    const std::int64_t count = value->as_int64();
    return count > 0 ? static_cast<std::size_t>(count) : 0;
  }

  if (value->is_uint64()) {
    return static_cast<std::size_t>(value->as_uint64());
  }

  if (value->is_double()) {
    const double count = value->as_double();
    return count > 0 ? static_cast<std::size_t>(count) : 0;
  }

  return fallback;
}

// Reads a provider `usage` block. Chat reports `prompt_tokens`/`completion_tokens`,
// Responses `input_tokens`/`output_tokens`; both shapes are accepted.
Usage read_usage(const boost::json::object& usage) {
  Usage out;
  out.input_tokens = size_at(usage, "prompt_tokens", size_at(usage, "input_tokens"));
  out.output_tokens =
      size_at(usage, "completion_tokens", size_at(usage, "output_tokens"));
  out.reported = usage.if_contains("prompt_tokens") != nullptr ||
      usage.if_contains("input_tokens") != nullptr;
  return out;
}

// Running counterpart of prompt_tokens: counts UTF-8 bytes as they arrive so a
// live output estimate costs O(chunk) instead of rescanning the whole reply.
class TokenCounter {
public:
  void add(std::string_view text) {
    for (const unsigned char c: text) {
      if (c < 128) {
        ++ascii_;
      } else if ((c & 0xC0) != 0x80) {
        ++non_ascii_; // count each UTF-8 lead byte once
      }
    }
  }

  [[nodiscard]] std::size_t tokens() const {
    return (ascii_ + 3) / 4 + non_ascii_;
  }

private:
  std::size_t ascii_ = 0;
  std::size_t non_ascii_ = 0;
};

// Incrementally decodes an OpenAI-compatible (Chat Completions) Server-Sent
// Events stream. It never throws: a failure is recorded and reported as
// "finished" so the caller can stop reading and then act on the recorded state.
class SseStream {
public:
  SseStream(const StreamCallbacks& callbacks, std::stop_token stop_token)
      : callbacks_(callbacks), stop_token_(stop_token) {}

  bool feed(std::string_view chunk) {
    buffer_.append(chunk.data(), chunk.size());

    std::size_t newline = std::string::npos;

    while ((newline = buffer_.find('\n')) != std::string::npos) {
      const std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);

      if (!handle_line(line)) {
        return false;
      }
    }

    return true;
  }

  void flush() {
    if (finished_ || buffer_.empty()) {
      return;
    }

    std::string line = std::move(buffer_);
    buffer_.clear();
    handle_line(line);
  }

  [[nodiscard]] bool finished() const { return finished_; }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] const std::string& text() const { return full_reply_; }
  [[nodiscard]] const std::string& finish_reason() const { return finish_reason_; }
  [[nodiscard]] Usage usage() const { return usage_; }
  [[nodiscard]] const std::string& thinking() const { return thinking_; }

  [[nodiscard]] std::vector<ToolCall> tool_calls() const {
    std::vector<ToolCall> calls;
    calls.reserve(tool_calls_.size());

    for (const auto& [index, call]: tool_calls_) {
      calls.push_back(call);
    }

    return calls;
  }

  [[nodiscard]] boost::json::object assistant_message() const {
    boost::json::object message;
    message["role"] = "assistant";
    message["content"] = full_reply_;

    if (!tool_calls_.empty()) {
      boost::json::array calls;

      for (const auto& [index, call]: tool_calls_) {
        boost::json::object function;
        function["name"] = call.name;
        function["arguments"] = call.arguments;

        boost::json::object item;
        item["id"] = call.id;
        item["type"] = "function";
        item["function"] = std::move(function);

        calls.emplace_back(std::move(item));
      }

      message["tool_calls"] = std::move(calls);
    }

    return message;
  }

private:
  bool handle_line(std::string_view raw_line) {
    if (stop_token_.stop_requested()) {
      finished_ = true;
      return false;
    }

    if (raw_line.ends_with('\r')) {
      raw_line.remove_suffix(1);
    }

    // Only `data:` records carry payload; comments and keep-alives are ignored.
    if (!raw_line.starts_with("data:")) {
      return true;
    }

    std::string_view data = raw_line.substr(5);

    if (data.starts_with(' ')) {
      data.remove_prefix(1);
    }

    if (data == "[DONE]") {
      finished_ = true;
      return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value document = boost::json::parse(data, parse_error);

    if (parse_error || !document.is_object()) {
      // A malformed keep-alive must not abort the whole stream.
      felis::log::debug("SSE JSON parse failed: {}", parse_error.message());
      return true;
    }

    const boost::json::object& root = document.as_object();

    // With `stream_options.include_usage`, the final chunk carries the counts and
    // an empty `choices` array, so usage is read before the choice check below.
    if (const boost::json::object* usage = object_at(root, "usage");
        usage != nullptr) {
      usage_ = read_usage(*usage);
    }

    const boost::json::array* choices = array_at(root, "choices");

    if (choices == nullptr || choices->empty() || !choices->front().is_object()) {
      return true;
    }

    const boost::json::object& choice = choices->front().as_object();

    if (const boost::json::value* reason = choice.if_contains("finish_reason");
        reason != nullptr && reason->is_string()) {
      finish_reason_ = std::string(reason->as_string());
    }

    const boost::json::object* delta = object_at(choice, "delta");

    if (delta == nullptr) {
      return true;
    }

    if (const boost::json::value* content = delta->if_contains("content");
        content != nullptr && content->is_string()) {
      const std::string_view token = content->as_string();

      if (!append_text(token)) {
        return false;
      }
    }

    // Hidden reasoning (a reasoning model's "thinking") is kept out of the reply
    // but recorded through the callback, so the caller can show what the model was
    // working through when a turn failed.
    const boost::json::value* reasoning = delta->if_contains("reasoning_content");

    if (reasoning == nullptr) {
      reasoning = delta->if_contains("reasoning");
    }

    if (reasoning != nullptr && reasoning->is_string()) {
      const std::string text = std::string(reasoning->as_string());
      thinking_ += text;

      if (callbacks_.on_reasoning) {
        callbacks_.on_reasoning(text);
      }

      output_estimate_.add(text);
      publish_output_estimate();
    }

    // Tool calls arrive split across chunks; merge them per `index`.
    if (const boost::json::array* calls = array_at(*delta, "tool_calls");
        calls != nullptr) {
      for (const boost::json::value& item: *calls) {
        if (!item.is_object()) {
          continue;
        }

        const boost::json::object& part = item.as_object();
        const int index =
            int_at(part, "index", static_cast<int>(tool_calls_.size()));

        auto& call = tool_calls_[index];
        call.index = index;

        const std::string id = string_at(part, "id");
        if (!id.empty()) {
          call.id = id;
        }

        if (const boost::json::object* function = object_at(part, "function");
            function != nullptr) {
          const std::string name_part = string_at(*function, "name");
          const std::string argument_part = string_at(*function, "arguments");
          call.name += name_part;
          call.arguments += argument_part;
          output_estimate_.add(name_part);
          output_estimate_.add(argument_part);
          publish_output_estimate();
        }
      }
    }

    return true;
  }

  bool append_text(std::string_view token) {
    if (callbacks_.on_text && !callbacks_.on_text(token)) {
      failed_ = true;
      error_ = "Generation is no longer active";
      finished_ = true;
      return false;
    }

    full_reply_.append(token);
    output_estimate_.add(token);
    publish_output_estimate();
    return true;
  }

  void publish_output_estimate() {
    if (callbacks_.on_output_estimate) {
      callbacks_.on_output_estimate(output_estimate_.tokens());
    }
  }

  const StreamCallbacks& callbacks_;
  std::stop_token stop_token_;

  std::string buffer_;
  std::string full_reply_;
  std::string thinking_;
  std::string finish_reason_;
  std::map<int, ToolCall> tool_calls_;
  Usage usage_;
  TokenCounter output_estimate_;

  bool finished_ = false;
  bool failed_ = false;
  std::string error_;
};

// Incrementally decodes the Responses API's Server-Sent Events stream. The
// vocabulary is the `response.*` event family and the terminal event is
// `response.completed` (with `response.failed` / `response.incomplete` as the
// other endings); there is no `[DONE]` sentinel. Like SseStream it never throws:
// a failure is recorded and reported as "finished".
class ResponsesStream {
public:
  ResponsesStream(const StreamCallbacks& callbacks, std::stop_token stop_token)
      : callbacks_(callbacks), stop_token_(stop_token) {}

  bool feed(std::string_view chunk) {
    buffer_.append(chunk.data(), chunk.size());

    std::size_t newline = std::string::npos;

    while ((newline = buffer_.find('\n')) != std::string::npos) {
      const std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);

      if (!handle_line(line)) {
        return false;
      }
    }

    return true;
  }

  void flush() {
    if (finished_ || buffer_.empty()) {
      return;
    }

    std::string line = std::move(buffer_);
    buffer_.clear();
    handle_line(line);
  }

  [[nodiscard]] bool finished() const { return finished_; }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] const std::string& text() const { return full_reply_; }
  [[nodiscard]] const std::string& finish_reason() const { return finish_reason_; }
  [[nodiscard]] Usage usage() const { return usage_; }
  [[nodiscard]] const std::string& thinking() const { return thinking_; }

  // The tool calls the model requested, in output order. `id` is the Responses
  // `call_id`, which is exactly what a function_call_output must quote back.
  [[nodiscard]] std::vector<ToolCall> tool_calls() const {
    std::vector<ToolCall> calls;

    for (const auto& [index, item]: items_) {
      if (string_at(item, "type") != "function_call") {
        continue;
      }

      ToolCall call;
      call.index = index;
      call.id = string_at(item, "call_id");
      if (call.id.empty()) {
        call.id = string_at(item, "id");
      }
      call.name = string_at(item, "name");
      call.arguments = string_at(item, "arguments");
      calls.push_back(std::move(call));
    }

    return calls;
  }

  // Every output item the turn produced (reasoning, function_call, message), in
  // order, so the next round can replay them verbatim — reasoning items must
  // travel back with the tool outputs.
  [[nodiscard]] std::vector<boost::json::object> output_items() const {
    std::vector<boost::json::object> items;

    for (const auto& [index, item]: items_) {
      items.push_back(item);
    }

    return items;
  }

private:
  bool handle_line(std::string_view raw_line) {
    if (stop_token_.stop_requested()) {
      finished_ = true;
      return false;
    }

    if (raw_line.ends_with('\r')) {
      raw_line.remove_suffix(1);
    }

    // Only `data:` records carry payload; comments and keep-alives are ignored.
    if (!raw_line.starts_with("data:")) {
      return true;
    }

    std::string_view data = raw_line.substr(5);

    if (data.starts_with(' ')) {
      data.remove_prefix(1);
    }

    // A gateway may still append the OpenAI sentinel; treat it as an ending.
    if (data == "[DONE]") {
      finish_if_open("stop");
      return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value document = boost::json::parse(data, parse_error);

    if (parse_error || !document.is_object()) {
      felis::log::debug(
          "Responses SSE JSON parse failed: {}", parse_error.message()
      );
      return true;
    }

    return handle_event(document.as_object());
  }

  bool handle_event(const boost::json::object& root) {
    const std::string type = string_at(root, "type");

    if (type == "response.output_text.delta") {
      return append_text(string_at(root, "delta"));
    }

    // Hidden reasoning is only exposed as a summary, and only when one was
    // requested. When present it is recorded through the callback, never the reply.
    if (type == "response.reasoning_summary_text.delta" ||
        type == "response.reasoning_text.delta") {
      const std::string delta = string_at(root, "delta");
      thinking_ += delta;

      if (callbacks_.on_reasoning) {
        callbacks_.on_reasoning(delta);
      }

      output_estimate_.add(delta);
      publish_output_estimate();
      return true;
    }

    if (type == "response.output_item.done") {
      // The finished item is authoritative: it carries the complete argument
      // string (and any encrypted reasoning content) the deltas spelled out.
      const int index = int_at(root, "output_index", static_cast<int>(items_.size()));

      if (const boost::json::object* item = object_at(root, "item");
          item != nullptr && !item->empty()) {
        items_[index] = *item;
      }

      return true;
    }

    if (type == "response.completed") {
      const boost::json::object* response = object_at(root, "response");

      if (response != nullptr) {
        adopt_output(*response);
        finish_if_open(
            string_at(*response, "status") == "incomplete" ? "length" : "stop"
        );
      } else {
        finish_if_open("stop");
      }

      return false;
    }

    if (type == "response.incomplete") {
      if (const boost::json::object* response = object_at(root, "response");
          response != nullptr) {
        adopt_output(*response);
      }

      finish_if_open("length");
      return false;
    }

    if (type == "response.failed") {
      error_ = "The response failed";

      if (const boost::json::object* response = object_at(root, "response");
          response != nullptr) {
        if (const boost::json::object* error = object_at(*response, "error");
            error != nullptr) {
          const std::string message = string_at(*error, "message");
          if (!message.empty()) {
            error_ = message;
          }
        }
      }

      failed_ = true;
      finished_ = true;
      return false;
    }

    return true;
  }

  // The completed event carries the whole response; its `output` array, when
  // present, is the authoritative item list and supersedes what the item events
  // gathered.
  void adopt_output(const boost::json::object& response) {
    // The terminal event carries the whole response, `usage` included. It is the
    // only place the Responses protocol reports the counts.
    if (const boost::json::object* usage = object_at(response, "usage");
        usage != nullptr) {
      usage_ = read_usage(*usage);
    }

    const boost::json::array* output = array_at(response, "output");

    if (output == nullptr || output->empty()) {
      return;
    }

    std::map<int, boost::json::object> authoritative;

    for (std::size_t i = 0; i < output->size(); ++i) {
      if ((*output)[i].is_object()) {
        authoritative[static_cast<int>(i)] = (*output)[i].as_object();
      }
    }

    items_ = std::move(authoritative);

    // Fall back to the output's own message text when no text deltas arrived.
    if (full_reply_.empty()) {
      for (const auto& [index, item]: items_) {
        if (string_at(item, "type") == "message") {
          append_text(message_text(item));
        }
      }
    }
  }

  void publish_output_estimate() {
    if (callbacks_.on_output_estimate) {
      callbacks_.on_output_estimate(output_estimate_.tokens());
    }
  }

  static std::string message_text(const boost::json::object& item) {
    std::string text;

    if (const boost::json::array* content = array_at(item, "content");
        content != nullptr) {
      for (const boost::json::value& part: *content) {
        if (!part.is_object()) {
          continue;
        }

        const boost::json::object& entry = part.as_object();

        if (string_at(entry, "type") == "output_text") {
          text += string_at(entry, "text");
        }
      }
    }

    return text;
  }

  bool append_text(std::string_view delta) {
    if (delta.empty()) {
      return true;
    }

    if (callbacks_.on_text && !callbacks_.on_text(delta)) {
      failed_ = true;
      error_ = "Generation is no longer active";
      finished_ = true;
      return false;
    }

    full_reply_.append(delta);
    output_estimate_.add(delta);
    publish_output_estimate();
    return true;
  }

  void finish_if_open(std::string_view reason) {
    if (finish_reason_.empty()) {
      finish_reason_ = std::string(reason);
    }

    finished_ = true;
  }

  const StreamCallbacks& callbacks_;
  std::stop_token stop_token_;

  std::string buffer_;
  std::string full_reply_;
  std::string thinking_;
  std::string finish_reason_;
  std::map<int, boost::json::object> items_;
  Usage usage_;
  TokenCounter output_estimate_;

  bool finished_ = false;
  bool failed_ = false;
  std::string error_;
};

} // namespace

std::expected<Result, Failure> stream_request(
    Protocol protocol,
    const std::string& url,
    const std::optional<std::string>& proxy,
    const std::vector<net::HttpHeader>& headers,
    const std::string& body,
    std::stop_token stop_token,
    int inactivity_timeout_ms,
    int read_timeout_ms,
    const StreamCallbacks& callbacks
) {
  if (stop_token.stop_requested()) {
    return std::unexpected(Failure{FailureKind::Cancelled, "Generation cancelled"});
  }

  // Both parsers share the same surface; only the protocol decides which event
  // vocabulary is decoded. The lambdas below keep the surrounding code identical
  // for either.
  std::optional<SseStream> chat_stream;
  std::optional<ResponsesStream> responses_stream;

  if (protocol == Protocol::Responses) {
    responses_stream.emplace(callbacks, stop_token);
  } else {
    chat_stream.emplace(callbacks, stop_token);
  }

  const auto feed = [&](std::string_view chunk) -> bool {
    return chat_stream ? chat_stream->feed(chunk) : responses_stream->feed(chunk);
  };
  const auto flush = [&]() {
    if (chat_stream) {
      chat_stream->flush();
    } else {
      responses_stream->flush();
    }
  };
  const auto stream_finished = [&]() {
    return chat_stream ? chat_stream->finished() : responses_stream->finished();
  };
  const auto stream_failed = [&]() {
    return chat_stream ? chat_stream->failed() : responses_stream->failed();
  };
  const auto stream_error = [&]() -> const std::string& {
    return chat_stream ? chat_stream->error() : responses_stream->error();
  };

  // A cancellation after the stream has begun keeps whatever the model had
  // already streamed (text and hidden reasoning), so the caller can persist the
  // interrupted turn instead of discarding it.
  const auto cancelled = [&]() -> std::unexpected<Failure> {
    Failure result{FailureKind::Cancelled, "Generation cancelled"};

    if (chat_stream) {
      result.partial_text = chat_stream->text();
      result.partial_reasoning = chat_stream->thinking();
    } else if (responses_stream) {
      result.partial_text = responses_stream->text();
      result.partial_reasoning = responses_stream->thinking();
    }

    return std::unexpected(std::move(result));
  };

  // Keep the first bytes of the response around so an HTTP error body can be
  // reported; on success the captured prefix is simply ignored.
  std::string captured;
  constexpr std::size_t CAPTURE_LIMIT = 2048;

  const net::ResponseHandler on_response =
      [](int, const std::vector<net::HttpHeader>&) { return true; };

  const net::BodyHandler on_body = [&](std::string_view chunk) {
    if (captured.size() < CAPTURE_LIMIT) {
      captured.append(chunk.substr(0, CAPTURE_LIMIT - captured.size()));
    }

    return feed(chunk);
  };

  int status = 0;

  try {
    status = net::http_request(
        "POST", url, proxy, headers, body, stop_token, on_response, on_body,
        inactivity_timeout_ms, 0, read_timeout_ms
    );
  } catch (const std::exception& error) {
    flush();

    if (stop_token.stop_requested()) {
      return cancelled();
    }

    return std::unexpected(Failure{FailureKind::Transport, error.what()});
  }

  flush();

  if (stop_token.stop_requested()) {
    return cancelled();
  }

  if (stream_failed()) {
    return std::unexpected(Failure{FailureKind::Stream, stream_error()});
  }

  if (status < 200 || status >= 300) {
    std::string message =
        "HTTP request failed (HTTP " + std::to_string(status) + ")";

    if (!captured.empty()) {
      message += ": " + captured;
    }

    return std::unexpected(Failure{FailureKind::HttpStatus, std::move(message)});
  }

  if (!stream_finished()) {
    // The stream closed without its terminal sentinel. The usual cause is a
    // non-streaming response — a plain JSON completion or an error body — which
    // carries no `data:` lines to parse; showing the start of the body makes that
    // immediately obvious.
    std::string message = "SSE stream ended before [DONE]";

    if (!captured.empty()) {
      message += ": ";
      message += captured.substr(0, 400);
    }

    return std::unexpected(Failure{FailureKind::Stream, std::move(message)});
  }

  Result result;

  if (chat_stream) {
    result.text = chat_stream->text();
    result.reasoning = chat_stream->thinking();
    result.finish_reason = chat_stream->finish_reason();
    result.tool_calls = chat_stream->tool_calls();
    result.assistant_message = chat_stream->assistant_message();
    result.usage = chat_stream->usage();
  } else {
    result.text = responses_stream->text();
    result.reasoning = responses_stream->thinking();
    result.finish_reason = responses_stream->finish_reason();
    result.tool_calls = responses_stream->tool_calls();
    result.response_items = responses_stream->output_items();
    result.usage = responses_stream->usage();
  }

  return result;
}

} // namespace felis::llm
