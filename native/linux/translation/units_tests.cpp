#include "units.hpp"
#include <iostream>
#include <stdexcept>
using namespace rimes::translation;
void Check(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
int main() {
    Units units;
    Check(units.Synchronize({{"a", "第一句。"}, {"b", "第二句。尾"}}, 0), "valid source");
    Check(units.All().size() == 3 && units.All()[1].slices[0].offset == 0 && units.All()[2].slices[0].offset == 12, "exact UTF-8 slices");
    Check(!units.Begin(299), "debounce");
    auto first = units.Begin(300).value();
    Check(units.Finish(first.id, first.ticket, "First.", true, "en"), "first translation");
    units.Synchronize({{"a", "第一句。"}, {"b", "第二句。尾部编辑"}}, 400);
    Check(units.All()[0].id == first.id && units.Preview() == "First. ", "stable prefix survives tail editing");
    auto second = units.Begin(401).value();
    units.Synchronize({{"a", "第一句。"}, {"b", "第二句。尾部继续编辑"}}, 600);
    Check(units.Finish(second.id, second.ticket, "Second.", true, "en"), "inflight stable sentence survives unrelated edits");
    units.Synchronize({{"a", "修改了前段。"}, {"b", "第二句。尾部继续编辑"}}, 700);
    Check(units.All()[1].id == second.id && units.Preview().empty(), "edited prefix invalidates only its own result");
    Check(!units.Finish(first.id, first.ticket, "stale", true, "en"), "removed source rejects stale result");
    auto retry = units.Begin(1000, true).value();
    units.Pause();
    Check(!units.Finish(retry.id, retry.ticket, "cancelled", true, "en"), "cancel rejects late callback");
    Check(!units.Begin(5000), "cancel remains paused");
    units.Retry();
    auto replacement = units.Begin(5000).value();
    Check(replacement.id == retry.id && replacement.ticket != retry.ticket, "retry retains unit but replaces request");
    Check(!units.Finish(retry.id, retry.ticket, "old retry", true, "en"), "cancelled request cannot finish new retry");
    Check(!units.Finish(replacement.id, replacement.ticket, "", false, "en") && units.Failed(), "failed result is explicit");
    units.Retry();
    replacement = units.Begin(5000).value();
    Check(units.Finish(replacement.id, replacement.ticket, "  修改。  ", true, "zh-Hans"), "retry succeeds");
    Units continuous;
    for (int i = 0; i <= 9; ++i) {
        continuous.Synchronize({{"tail", std::string(i + 1, 'x')}}, i * 100);
        Check(bool(continuous.Begin(i * 100)) == (i == 9), "maximum wait under continuous typing");
    }
    Units whitespace;
    const std::string text = "  \"Hello!\"  \n第二句。\n\n3.14 is a number.🙂";
    whitespace.Synchronize({{"left", text.substr(0, 9)}, {"right", text.substr(9)}}, 0);
    std::string reconstructed;
    for (const auto& unit : whitespace.All()) {
        std::string slices; for (const auto& slice : unit.slices) slices += slice.text;
        Check(slices == unit.source, "cross-block ownership"); reconstructed += unit.source;
    }
    Check(reconstructed == text, "punctuation, paragraphs and emoji are byte exact");
    Units bounded;
    std::string long_input; for (int i = 0; i < 300; ++i) long_input += "句。";
    bounded.Synchronize({{"large", long_input}}, 0);
    Check(bounded.All().size() == Units::kMaximumUnits, "bounded fan-out");
    reconstructed.clear(); for (const auto& u : bounded.All()) reconstructed += u.source;
    Check(reconstructed == long_input, "large paste is never truncated");
    Check(!bounded.Synchronize({{"bad", "\xed\xa0\x80"}}, 0) && bounded.All().empty(), "invalid UTF-8 refuses source");
    Check(!bounded.Synchronize({{"same", "a"}, {"same", "b"}}, 0), "duplicate source identity refused");
    Units delivery;
    delivery.SetSegmenter([](std::string_view text) { return std::vector<std::string>{std::string(text.substr(0, 6)), std::string(text.substr(6))}; });
    delivery.Synchronize({{"shared", "前句。后句。"}}, 0);
    auto front = delivery.Begin(300).value();
    Check(delivery.Finish(front.id, front.ticket, "First translated sentence", true, "en"), "segment translated output");
    auto following = delivery.Begin(300).value();
    auto head = delivery.Head().value();
    Check(!head.retired && head.source.size() == 1 && head.source[0].text == "前句。", "first child owns exact full source sentence");
    Check(delivery.Consume(head), "native success retires source once");
    auto suffix = delivery.Head().value();
    Check(suffix.retired && suffix.source.empty() && suffix.child != head.child, "unsent child becomes independent immutable output");
    Check(!delivery.Consume(head), "duplicate callback cannot consume twice");
    delivery.Synchronize({{"shared", "后句。"}}, 400);
    Check(delivery.All()[0].id == following.id && delivery.All()[0].slices[0].offset == 0, "consumption rebases untouched sentence without retranslating");
    Check(delivery.Finish(following.id, following.ticket, "Second translated sentence", true, "en"), "inflight following sentence retains ownership");
    delivery.Pause(); delivery.ResetMutable();
    Check(delivery.Head()->child == suffix.child && delivery.Preview() == suffix.text, "owner/configuration pause preserves immutable suffix");
    delivery.Synchronize({{"new", "继续。"}}, 500);
    Check(delivery.Head()->child == suffix.child, "fresh source follows frozen output");
    Check(delivery.Consume(suffix), "remaining child consumes no source");
    Check(delivery.Retired().empty() && !delivery.Head(), "source waits for its own result");
    delivery.Clear();
    Check(delivery.Preview().empty(), "privacy discard removes pending output");
    std::cout << "Translation units: slices, debounce/deadline, incremental edits, cancellation, retry and bounds passed\n";
}
