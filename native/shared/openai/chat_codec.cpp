// Copyright 2026 scholay
// SPDX-License-Identifier: Apache-2.0
#include "chat_codec.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
namespace rimes::openai {
namespace {
using Json = nlohmann::json;
Json Parse(std::string_view bytes) {
    try {
        return Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32) throw Failure(Error::Invalid);
            return true;
        });
    } catch (const std::exception&) { throw Failure(Error::Invalid); }
}
std::string Trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \r\n\t");
    if (start == text.npos) return {};
    return std::string(text.substr(start, text.find_last_not_of(" \r\n\t") - start + 1));
}
void ValidText(std::string_view text) {
    if (text.size() > kTextLimit) throw Failure(Error::TooLarge);
    if (text.find('\0') != text.npos) throw Failure(Error::Invalid);
    try { (void)Json(std::string(text)).dump(); }
    catch (const std::exception&) { throw Failure(Error::Invalid); }
}
std::string Candidate(std::string_view raw) {
    auto text = Trim(raw);
    if (text.compare(0, 3, "```") == 0) {
        const auto first = text.find('\n');
        if (first == text.npos) return {};
        text.erase(0, first + 1);
        if (text.size() >= 3 && text.compare(text.size() - 3, 3, "```") == 0) text.resize(text.size() - 3);
        text = Trim(text);
    }
    return text;
}
std::string BlockText(const Json& block) {
    if (!block.is_object() || !block.contains("text") || !block["text"].is_string()) throw Failure(Error::Invalid);
    if (block.contains("title") && !block["title"].is_null() &&
        (!block["title"].is_string() || block["title"].get<std::string>().size() > 200)) throw Failure(Error::Invalid);
    auto text = block["text"].get<std::string>();
    for (std::size_t at = 0; (at = text.find("\r\n", at)) != std::string::npos;) text.replace(at, 2, "\n");
    text = Trim(text); ValidText(text);
    if (text.empty()) throw Failure(Error::Invalid);
    return text;
}
std::string Content(const Json& value) {
    if (value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_array()) throw Failure(Error::Invalid);
    std::string result;
    for (const auto& part : value) {
        if (!part.is_object()) throw Failure(Error::Invalid);
        if (part.value("type", "") == "text") {
            if (!part.contains("text") || !part["text"].is_string()) throw Failure(Error::Invalid);
            result += part["text"].get<std::string>();
        }
    }
    return result;
}
}
std::string RequestBody(std::string_view model, std::string_view source, std::string_view system_prompt) {
    ValidText(source); ValidText(model); ValidText(system_prompt);
    if (Trim(source).empty() || Trim(model).empty() || model.size() > 256) throw Failure(Error::Invalid);
    return Json{{"model", model}, {"stream", true}, {"messages", Json::array({
        {{"role", "system"}, {"content", system_prompt}}, {{"role", "user"}, {"content", source}}})}}.dump();
}
std::string Joined(const std::vector<std::string>& blocks) {
    std::string text;
    for (const auto& block : blocks) {
        if (block.size() > kTextLimit - text.size()) throw Failure(Error::TooLarge);
        text += block;
    }
    return text;
}
std::vector<std::string> LogicalBlocks(std::string_view raw) {
    ValidText(raw);
    const auto text = Candidate(raw);
    if (text.empty()) throw Failure(Error::Invalid);
    if (text.front() != '{' && text.front() != '[') return {text};
    const auto parsed = Parse(text);
    if (!parsed.is_object() || !parsed.contains("blocks") || !parsed["blocks"].is_array() ||
        parsed["blocks"].empty() || parsed["blocks"].size() > 20) throw Failure(Error::Invalid);
    std::vector<std::string> blocks;
    for (const auto& block : parsed["blocks"]) blocks.push_back(BlockText(block));
    (void)Joined(blocks);
    return blocks;
}
void Decoder::Feed(std::string_view bytes) {
    if (bytes.size() > kWireLimit - wire_) throw Failure(Error::TooLarge);
    wire_ += bytes.size();
    for (const auto ch : bytes) {
        if (done_) return;
        if (ch == '\n') Line();
        else {
            if (line_.size() >= kLineLimit) throw Failure(Error::TooLarge);
            line_ += ch;
        }
    }
}
void Decoder::Line() {
    auto line = std::move(line_); line_.clear();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) { Event(); return; }
    if (line.compare(0, 5, "data:") != 0) return;
    line.erase(0, 5); if (!line.empty() && line.front() == ' ') line.erase(0, 1);
    if (line.size() + event_.size() + 1 > kLineLimit) throw Failure(Error::TooLarge);
    if (!event_.empty()) event_ += '\n';
    event_ += line;
}
void Decoder::Event() {
    if (event_.empty()) return;
    auto event = std::move(event_); event_.clear();
    if (event == "[DONE]") { (void)Blocks(); done_ = true; return; }
    try {
        const auto value = Parse(event);
        if (!value.is_object()) throw Failure(Error::Invalid);
        if (value.contains("error")) throw Failure(Error::Provider);
        if (!value.contains("choices")) return;
        const auto& choices = value.at("choices");
        if (!choices.is_array() || choices.size() > 1) throw Failure(Error::Invalid);
        for (const auto& choice : choices) {
            if (!choice.is_object() || !choice.contains("index") || !choice["index"].is_number_integer() || choice["index"] != 0)
                throw Failure(Error::Invalid);
            std::string content;
            if (choice.contains("delta")) {
                const auto& delta = choice.at("delta");
                if (!delta.is_object()) throw Failure(Error::Invalid);
                if (delta.contains("tool_calls") || delta.contains("function_call")) throw Failure(Error::Invalid);
                content = Content(delta.value("content", Json()));
            } else if (choice.contains("text")) content = Content(choice.at("text"));
            if (!content.empty()) {
                if (stopped_) throw Failure(Error::Invalid);
                ValidText(content);
                if (content.size() > kTextLimit - text_.size()) throw Failure(Error::TooLarge);
                text_ += content;
            }
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                if (stopped_ || choice["finish_reason"] != "stop") throw Failure(Error::Incomplete);
                stopped_ = true;
            }
        }
    } catch (const Failure&) { throw; }
    catch (const std::exception&) { throw Failure(Error::Invalid); }
}
void Decoder::Finish() {
    if (!done_) { if (!line_.empty()) Line(); Event(); }
    if (!done_) throw Failure(Error::Incomplete);
    (void)Blocks();
}
std::string Decoder::Preview() const {
    const auto text = Candidate(text_);
    if (text.empty()) return {};
    if (text.front() != '{' && text.front() != '[') return text;
    // Emit only complete logical objects while their JSON envelope is growing.
    // Unfinished JSON syntax is never shown as generated prose.
    const auto key = text.find("\"blocks\"");
    const auto array = key == text.npos ? text.npos : text.find('[', key + 8);
    if (array == text.npos) return {};
    std::vector<std::string> blocks;
    bool quoted = false, escaped = false;
    int depth = 0; std::size_t start = 0;
    for (std::size_t at = array + 1; at < text.size(); ++at) {
        const auto ch = text[at];
        if (quoted) { if (escaped) escaped = false; else if (ch == '\\') escaped = true; else if (ch == '"') quoted = false; continue; }
        if (ch == '"') quoted = true;
        else if (ch == '{') { if (!depth) start = at; ++depth; }
        else if (ch == '}' && depth && !--depth) {
            blocks.push_back(BlockText(Parse(std::string_view(text).substr(start, at - start + 1))));
            if (blocks.size() > 20) throw Failure(Error::Invalid);
        }
    }
    return Joined(blocks);
}
} // namespace rimes::openai
