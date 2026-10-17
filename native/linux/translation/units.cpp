#include "units.hpp"
#include <algorithm>
#include <set>

namespace rimes::translation {
namespace {
struct Rune { std::uint32_t value; std::size_t start, end; };
std::vector<Rune> Runes(std::string_view text) {
    std::vector<Rune> runes;
    for (std::size_t at = 0; at < text.size();) {
        const auto start = at;
        const auto lead = static_cast<unsigned char>(text[at++]);
        std::size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead < 0xE0 ? 2 : lead < 0xF0 && lead >= 0xE0 ? 3 : lead <= 0xF4 && lead >= 0xF0 ? 4 : 0;
        if (!length || start + length > text.size() || !lead) return {};
        std::uint32_t value = lead & (length == 1 ? 0x7f : (1 << (7 - length)) - 1);
        for (std::size_t i = 1; i < length; ++i) {
            const auto byte = static_cast<unsigned char>(text[at++]);
            if ((byte & 0xC0) != 0x80) return {};
            value = (value << 6) | (byte & 0x3F);
        }
        if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000) || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return {};
        runes.push_back({value, start, at});
    }
    return runes;
}
bool Space(std::uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000 || c == 0xa0; }
bool Closer(std::uint32_t c) { return c == '"' || c == '\'' || c == ')' || c == ']' || c == 0x201d || c == 0x2019 || c == 0xff09 || c == 0x3011 || c == 0x300b || c == 0x300d || c == 0x300f; }
std::string Format(std::string output, std::string_view source, std::string_view target) {
    const auto translated = Runes(output), original = Runes(source);
    auto first = std::find_if_not(translated.begin(), translated.end(), [](const auto& c) { return Space(c.value); });
    auto last = std::find_if_not(translated.rbegin(), translated.rend(), [](const auto& c) { return Space(c.value); });
    if (first == translated.end()) return {};
    auto start = std::find_if_not(original.begin(), original.end(), [](const auto& c) { return Space(c.value); });
    auto end = std::find_if_not(original.rbegin(), original.rend(), [](const auto& c) { return Space(c.value); });
    const auto prefix = start == original.end() ? source.size() : start->start;
    const auto suffix = end == original.rend() ? source.size() : end->end;
    std::string value(source.substr(0, prefix));
    value += output.substr(first->start, last->end - first->start);
    if (suffix < source.size()) value += source.substr(suffix);
    else if (target != "zh" && target != "zh-Hans") value += ' ';
    return value;
}
}
bool Units::ValidText(std::string_view text) { return text.size() <= kLimit && (text.empty() || !Runes(text).empty()); }
bool Units::Synchronize(const std::vector<SourceBlock>& blocks, std::int64_t now) {
    std::string source;
    std::set<std::string> identities;
    for (const auto& block : blocks) {
        if (block.id.empty() || !identities.insert(block.id).second || !ValidText(block.text) || block.text.size() > kLimit - source.size()) { ResetMutable(); return false; }
        source += block.text;
    }
    const auto runes = Runes(source);
    if (std::all_of(runes.begin(), runes.end(), [](const auto& c) { return Space(c.value); })) { ResetMutable(); return true; }
    std::vector<std::size_t> ends;
    for (std::size_t at = 0; at < runes.size() && ends.size() < kMaximumUnits - 1; ++at) {
        const auto c = runes[at].value;
        bool boundary = c == '\n' || c == '\r' || c == 0x3002 || c == 0xff01 || c == 0xff1f || c == '!' || c == '?';
        if (c == '.') boundary = !(at && at + 1 < runes.size() && runes[at-1].value >= '0' && runes[at-1].value <= '9' && runes[at+1].value >= '0' && runes[at+1].value <= '9');
        if (!boundary) continue;
        while (at + 1 < runes.size() && (Space(runes[at+1].value) || Closer(runes[at+1].value))) ++at;
        ends.push_back(runes[at].end);
    }
    if (!source.empty() && (ends.empty() || ends.back() != source.size())) ends.push_back(source.size());
    std::vector<Unit> next;
    std::size_t begin = 0;
    bool changed = false;
    // Continuous tail edits inherit their first dirty deadline. Ready units do
    // not age or regenerate merely because another sentence changed.
    auto first_dirty = now;
    for (const auto& unit : units_) if (unit.phase == Phase::Pending || unit.phase == Phase::Running) first_dirty = std::min(first_dirty, unit.first_dirty_ms);
    for (const auto end : ends) {
        Unit unit;
        unit.source = source.substr(begin, end - begin);
        std::size_t offset = 0;
        for (const auto& block : blocks) {
            const auto low = std::max(begin, offset), high = std::min(end, offset + block.text.size());
            if (low < high) unit.slices.push_back({block.id, block.text.substr(low - offset, high - low), low - offset});
            offset += block.text.size();
        }
        const auto previous = std::find_if(units_.begin(), units_.end(), [&](const auto& old) { return old.slices == unit.slices; });
        if (previous != units_.end()) unit = *previous;
        else {
            changed = true;
            unit.id = "translation-unit-" + std::to_string(++sequence_);
            unit.first_dirty_ms = first_dirty;
            unit.edited_ms = now;
            const auto content = Runes(unit.source);
            if (std::all_of(content.begin(), content.end(), [](const auto& c) { return Space(c.value); })) { unit.phase = Phase::Ready; unit.output = unit.source; }
        }
        next.push_back(std::move(unit));
        begin = end;
    }
    changed |= next.size() != units_.size();
    units_ = std::move(next);
    if (changed) paused_ = false;
    return true;
}
void Units::Clear() { ResetMutable(); retired_.clear(); }
void Units::ResetMutable() { units_.clear(); paused_ = false; }
void Units::Pause() {
    paused_ = true;
    for (auto& unit : units_) if (unit.phase == Phase::Running) { unit.phase = Phase::Pending; unit.ticket.clear(); }
}
void Units::Retry() {
    paused_ = false;
    for (auto& unit : units_) if (unit.phase == Phase::Failed) unit.phase = Phase::Pending;
}
void Units::ReturnToEdit() {
    Pause();
    for (auto& unit : units_) { unit.output.clear(); unit.children.clear(); unit.phase = Phase::Pending; unit.ticket.clear(); }
}
std::optional<Unit> Units::Begin(std::int64_t now, bool force) {
    if ((paused_ && !force) || Running()) return {};
    for (auto& unit : units_) {
        if (unit.phase != Phase::Pending) continue;
        if (!force && now < std::min(unit.edited_ms + kDebounceMs, unit.first_dirty_ms + kMaximumWaitMs)) return {};
        unit.phase = Phase::Running;
        unit.ticket = "translation-request-" + std::to_string(++sequence_);
        return unit;
    }
    return {};
}
bool Units::Finish(std::string_view id, std::string_view ticket, std::string output, bool success, std::string_view target) {
    auto found = std::find_if(units_.begin(), units_.end(), [&](const auto& unit) { return unit.id == id && unit.ticket == ticket && unit.phase == Phase::Running; });
    if (found == units_.end()) return false;
    success &= ValidText(output) && !output.empty();
    if (success) output = Format(std::move(output), found->source, target);
    std::size_t total = 0;
    for (const auto& unit : units_) total += unit.output.size();
    for (const auto& unit : retired_) total += unit.output.size();
    success &= !output.empty() && output.size() <= kLimit - std::min(total, kLimit);
    found->output = success ? std::move(output) : "";
    found->children.clear();
    if (success) {
        auto segments = split_ ? split_(found->output) : std::vector<std::string>{found->output};
        std::string joined; for (const auto& segment : segments) joined += segment;
        success = joined == found->output && !segments.empty() && std::none_of(segments.begin(), segments.end(), [](const auto& s) { return s.empty(); });
        if (success) {
            // Bound display fan-out without truncating translated text.
            if (segments.size() > kMaximumUnits) {
                for (std::size_t i = kMaximumUnits; i < segments.size(); ++i) segments[kMaximumUnits - 1] += segments[i];
                segments.resize(kMaximumUnits);
            }
            for (auto& segment : segments) found->children.push_back({"translation-output-" + std::to_string(++sequence_), std::move(segment)});
        } else found->output.clear();
    }
    found->phase = success ? Phase::Ready : Phase::Failed;
    return success;
}
const Unit* Units::Find(std::string_view id) const { const auto at = std::find_if(units_.begin(), units_.end(), [&](const auto& unit) { return unit.id == id; }); return at == units_.end() ? nullptr : &*at; }
bool Units::Complete() const { return (!units_.empty() || !retired_.empty()) && std::all_of(units_.begin(), units_.end(), [](const auto& u) { return u.phase == Phase::Ready; }); }
bool Units::Failed() const { return std::any_of(units_.begin(), units_.end(), [](const auto& u) { return u.phase == Phase::Failed; }); }
bool Units::HasPending() const { return std::any_of(units_.begin(), units_.end(), [](const auto& u) { return u.phase == Phase::Pending; }); }
bool Units::Running() const { return std::any_of(units_.begin(), units_.end(), [](const auto& u) { return u.phase == Phase::Running; }); }
std::string Units::Preview() const { std::string text; for (const auto& unit : retired_) text += unit.output; for (const auto& unit : units_) { if (unit.phase != Phase::Ready) break; text += unit.output; } return text; }
std::optional<Delivery> Units::Head() const {
    const auto* unit = !retired_.empty() ? &retired_.front() : units_.empty() ? nullptr : &units_.front();
    if (!unit || unit->phase != Phase::Ready || unit->children.empty()) return {};
    return Delivery{unit->id, unit->children.front().id, unit->children.front().text, unit->slices, !retired_.empty()};
}
bool Units::Consume(const Delivery& delivery) {
    const auto current = Head();
    if (!current || current->unit != delivery.unit || current->child != delivery.child || current->text != delivery.text || current->source != delivery.source || current->retired != delivery.retired) return false;
    auto& owner = delivery.retired ? retired_.front() : units_.front();
    owner.children.erase(owner.children.begin());
    owner.output.clear(); for (const auto& child : owner.children) owner.output += child.text;
    if (delivery.retired) {
        if (owner.children.empty()) retired_.erase(retired_.begin());
        return true;
    }
    auto frozen = std::move(owner);
    units_.erase(units_.begin());
    // Source consumption shifts offsets only; later untouched sentences keep
    // their IDs, in-flight tickets and output clocks.
    for (auto& unit : units_) for (auto& slice : unit.slices) {
        std::size_t shift = 0;
        for (const auto& consumed : delivery.source)
            if (consumed.block == slice.block && consumed.offset + consumed.text.size() <= slice.offset) shift += consumed.text.size();
        slice.offset -= shift;
    }
    frozen.source.clear(); frozen.slices.clear(); frozen.ticket.clear();
    if (!frozen.children.empty()) retired_.push_back(std::move(frozen));
    return true;
}
} // namespace rimes::translation
