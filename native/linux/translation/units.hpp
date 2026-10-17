#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rimes::translation {
struct SourceBlock { std::string id, text; };
struct SourceSlice {
    std::string block, text;
    std::size_t offset = 0;
    bool operator==(const SourceSlice& other) const { return block == other.block && offset == other.offset && text == other.text; }
};
enum class Phase { Pending, Running, Ready, Failed };
struct Unit {
    std::string id, ticket, source, output;
    std::vector<SourceSlice> slices;
    Phase phase = Phase::Pending;
    std::int64_t first_dirty_ms = 0, edited_ms = 0;
};
// Event-loop owned, independent of GTK/Fcitx and the inference backend. Identity
// belongs to exact source slices, not the smaller display/delivery segments.
class Units {
public:
    static constexpr std::size_t kLimit = 256 * 1024, kMaximumUnits = 200;
    // Match native/macos/AppleTranslationPlugin.swift TranslationRefreshPolicy.
    static constexpr std::int64_t kDebounceMs = 300, kMaximumWaitMs = 900;
    bool Synchronize(const std::vector<SourceBlock>& blocks, std::int64_t now_ms);
    void Clear();
    void Pause();
    void ReturnToEdit();
    void Retry();
    std::optional<Unit> Begin(std::int64_t now_ms, bool force = false);
    bool Finish(std::string_view id, std::string_view ticket, std::string output, bool success, std::string_view target);
    const Unit* Find(std::string_view id) const;
    const std::vector<Unit>& All() const { return units_; }
    bool Complete() const;
    bool Failed() const;
    bool HasPending() const;
    bool Running() const;
    bool Paused() const { return paused_; }
    std::string Preview() const;
    static bool ValidText(std::string_view text);
private:
    std::vector<Unit> units_;
    std::uint64_t sequence_ = 0;
    bool paused_ = false;
};
} // namespace rimes::translation
