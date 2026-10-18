// Copyright 2026 scholay
// SPDX-License-Identifier: Apache-2.0
#include "chat_codec.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace rimes::openai;
using Json = nlohmann::json;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void Reject(F operation) { bool rejected = false; try { operation(); } catch (const Failure&) { rejected = true; } Check(rejected, "invalid response accepted"); }
std::string Event(const std::string& text) {
    return "data: " + Json{{"choices", Json::array({{{"index", 0}, {"delta", {{"content", text}}}, {"finish_reason", nullptr}}})}}.dump() + "\r\n\r\n";
}
int main() {
    const auto request = Json::parse(RequestBody("configured-model", "原文🙂", "native contract"));
    Check(request.at("messages")[1].at("content") == "原文🙂" && request.at("stream") == true, "source changed in request");
    Decoder decoder;
    const std::string prefix = R"({"blocks":[{"text":"First 🙂.","title":null},)";
    const auto wire = ": ping\r\n\r\n" + Event(prefix);
    for (const auto byte : wire) decoder.Feed(std::string(1, byte));
    Check(decoder.Preview() == "First 🙂." && !decoder.Done(), "fragmented Unicode or partial envelope lost preview");
    decoder.Feed(Event(R"({"text":"第二段。","title":null}]})") + "data: [DONE]\n\n"); decoder.Finish();
    Check(decoder.Blocks() == std::vector<std::string>({"First 🙂.", "第二段。"}), "logical blocks changed");
    Check(decoder.Preview("\n\n") == "First 🙂.\n\n第二段。", "conversation separators lost");
    Reject([&] { RequestBody("model", std::string(kTextLimit + 1, 'a'), ""); });
    Check(Json::parse(RequestBody("model", std::string(kTextLimit + 1, 'a'), "", kWireLimit)).at("messages")[1].at("content").get<std::string>().size() == kTextLimit + 1, "bounded conversation prompt rejected");
    Reject([&] { RequestBody("model", std::string(kWireLimit + 1, 'a'), "", kWireLimit + 1); });
    Reject([&] { Joined({std::string(kTextLimit, 'a'), "b"}, "\n\n"); });
    Check(LogicalBlocks("```json\n{\"blocks\":[{\"text\":\" a\\r\\nb \"}]}\n```")[0] == "a\nb", "native normalization differs");
    Decoder plain; plain.Feed(Event("Plain fallback.") + "data: [DONE]\n\n"); plain.Finish();
    Check(plain.Preview() == "Plain fallback.", "plain text fallback lost");
    Reject([&] { Decoder d; d.Feed(Event("unfinished")); d.Finish(); });
    Reject([&] { Decoder d; d.Feed("data: {\"error\":{\"message\":\"private\"}}\n\n"); });
    Reject([&] { Decoder d; d.Feed(Event(std::string(kTextLimit + 1, 'a'))); });
    Reject([&] { Decoder d; d.Feed("data: [DONE]\n\n"); });
    Reject([&] { LogicalBlocks("{broken"); });
    Reject([&] { LogicalBlocks("{\"blocks\":[{\"text\":\"\\u0000\"}]}"); });
    Reject([&] { RequestBody("model", std::string("a\0b", 3), ""); });
    Reject([&] { Decoder d; d.Feed(Event("truncated") + "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"); });
    Reject([&] { Decoder d; d.Feed("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[]}}]}\n\n"); });
    std::cout << "OpenAI codec: exact source, fragmented Unicode, semantic blocks, incomplete/oversized/provider/tool rejection passed\n";
}
