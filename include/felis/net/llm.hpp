/*
 * Project: felis
 * Repository: https://github.com/leaked32/felis
 *
 * File: felis/include/felis/llm.hpp
 *
 * License: MIT
 */

#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json/object.hpp>

#include "felis/net/http.hpp"

// One streaming LLM call, for whichever OpenAI wire protocol a caller speaks.
//
// The request body is assembled by the caller — it is domain-specific — and this
// module owns everything from "POST these bytes" to "the turn is finished": the
// two Server-Sent-Events vocabularies, the transport/HTTP/cancel/truncation
// handling, and the finished-turn result. The protocol is chosen by an enum, so
// a caller makes the same one call whether it talks Chat Completions or
// Responses.
namespace felis::net::llm {

// The wire protocol. Both are OpenAI's; they differ in the SSE vocabulary and in
// the shape of a finished turn (Chat echoes an assistant message with
// `tool_calls`, Responses replays raw output items).
enum class Protocol {
  ChatCompletions = 0,
  Responses = 1,
};

// A single tool call the model requested. During streaming a Chat turn merges the
// call by `index`; a Responses turn reads it whole from a `function_call` item.
struct ToolCall {
  int index = -1;
  std::string id;
  std::string name;
  std::string arguments;
};

// What one request billed, as the service reported it. `reported` is false when
// the service sent no usage block, so a caller can substitute its own estimate.
struct Usage {
  std::size_t input_tokens = 0;
  std::size_t output_tokens = 0;
  bool reported = false;
};

// The hooks the stream drives as it decodes. `on_text` returns false to stop the
// stream (the caller's sink is no longer accepting, e.g. its generation is
// inactive); the others are informational. Any hook may be empty.
struct StreamCallbacks {
  // A visible assistant-text delta. Returning false aborts the stream as a
  // failure ("the sink is no longer active").
  std::function<bool(std::string_view)> on_text;
  // A hidden-reasoning delta (a reasoning model's "thinking").
  std::function<void(std::string_view)> on_reasoning;
  // The running estimate of output tokens produced so far, for a live readout.
  std::function<void(std::size_t)> on_output_estimate;
};

// The finished turn. `finish_reason` is "stop" for a normal end or "length" when
// the response was cut off (the caller decides how to report that).
struct Result {
  std::string text;
  std::string reasoning;
  std::string finish_reason;
  std::vector<ToolCall> tool_calls;
  // Chat only: the exact assistant message to echo back, `tool_calls` included.
  boost::json::object assistant_message;
  // Responses only: the raw output items (reasoning items included) to replay
  // verbatim on the next round.
  std::vector<boost::json::object> response_items;
  Usage usage;
};

enum class FailureKind {
  Cancelled,   // stop requested before or during the stream
  Transport,   // connect/TLS/write failure or a thrown error
  HttpStatus,  // a non-2xx status
  Stream,      // the SSE stream failed or ended before its sentinel
};

struct Failure {
  FailureKind kind = FailureKind::Transport;
  std::string reason;
  // Set on cancellation: the text/reasoning that had already streamed before the
  // stop, so the caller can persist the interrupted turn. Empty otherwise.
  std::string partial_text;
  std::string partial_reasoning;
};

// Performs one streaming request.
//
// `url`/`proxy`/`headers`/`body` are posted as-is (the body must already be valid
// UTF-8 — sanitize it upstream). `inactivity_timeout_ms` bounds connect + TLS +
// write; `read_timeout_ms` bounds each wait for response bytes (0 reuses the
// inactivity window). The stop token is observed throughout, so cancelling stays
// responsive regardless of the read window.
//
// On any failure no result is returned; the reason (including the captured start
// of an error body) is carried in `Failure`. Callers must discard any text they
// already received through `on_text` on failure, except that a cancellation also
// reports the partial text/reasoning back in the `Failure` so it can be kept.
[[nodiscard]] std::expected<Result, Failure> stream_request(
    Protocol protocol,
    const std::string& url,
    const std::optional<std::string>& proxy,
    const std::vector<net::HttpHeader>& headers,
    const std::string& body,
    std::stop_token stop_token,
    int inactivity_timeout_ms,
    int read_timeout_ms,
    const StreamCallbacks& callbacks
);

} // namespace felis::llm
