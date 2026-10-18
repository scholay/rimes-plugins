// Copyright 2026 scholay
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rimes::openai {
enum class Error { Invalid, TooLarge, Incomplete, Provider };
struct Failure : std::runtime_error {
    Error code;
    explicit Failure(Error value) : std::runtime_error("Invalid AI response"), code(value) {}
};
inline constexpr std::size_t kWireLimit = 1024 * 1024, kLineLimit = 512 * 1024, kTextLimit = 256 * 1024;
std::string RequestBody(std::string_view model, std::string_view source, std::string_view system_prompt,
                        std::size_t maximum_source = kTextLimit);
std::vector<std::string> LogicalBlocks(std::string_view text);
std::string Joined(const std::vector<std::string>& blocks, std::string_view separator = {});

// Transport-independent SSE codec. Input fragments may split UTF-8 or CRLF;
// only complete events are decoded. No endpoint, credential or target lives here.
class Decoder {
public:
    void Feed(std::string_view bytes);
    void Finish();
    bool Done() const { return done_; }
    std::string Preview(std::string_view separator = {}) const;
    std::vector<std::string> Blocks() const { return LogicalBlocks(text_); }
private:
    void Line();
    void Event();
    std::string line_, event_, text_;
    std::size_t wire_ = 0;
    bool stopped_ = false, done_ = false;
};
} // namespace rimes::openai
