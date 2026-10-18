#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rimes::practice {
using Json = nlohmann::json;
inline constexpr int kMaximumCharacters = 2048, kMaximumTextBytes = 16384;
inline constexpr int kMaximumCounter = 1000000, kMaximumSamples = 120;
inline constexpr double kMaximumDuration = 86400;
inline constexpr std::size_t kMaximumHistoryBytes = 4 * 1024 * 1024, kMaximumResults = 500;
struct Grapheme {
    std::string text, canonical;
    bool operator==(const Grapheme& other) const { return canonical == other.canonical; }
};
std::vector<Grapheme> Characters(const std::string& text);
// GTK character offsets are Unicode scalars. Expand in the resulting text so
// a separately committed combining mark/ZWJ counts the same attempt as AppKit.
std::pair<int, int> InsertionRange(const std::string& text, int scalar_start, int scalar_count);
Json Rates(const Json& metrics);
bool ValidResult(const Json& value);
Json NormalizeResult(const Json& value);
std::string ComparisonKey(const Json& result);
// Keeps article/editor text in memory. Finish emits only aggregate scores.
class Session {
  public:
    Session(Json article, Json context, std::string id);
    void Start(double time);
    void Key(double time, bool repeat = false, bool backspace = false, bool composing = false);
    void Chord(double time);
    void UpgradeBackspace();
    bool Commit(const std::string& text, double time,
                std::optional<std::pair<int, int>> insertion = std::nullopt);
    void Mark(const std::string& reason);
    void Cancel();
    Json Snapshot(double time);
    std::optional<Json> Finish(double time, double completed_at);

  private:
    struct Alignment {
        std::vector<std::string> states;
        std::vector<bool> correctness;
        int consumed = 0, correct = 0, substitutions = 0, omissions = 0, extras = 0;
    };
    Json article_, context_, metrics_;
    std::string id_;
    std::vector<Grapheme> expected_, committed_;
    Alignment alignment_;
    std::optional<double> started_, last_;
    std::vector<std::string> reasons_;
    std::optional<Json> result_;
    bool cancelled_ = false;
    double sample_interval_ = 1;
    bool Accepts(double time) const;
    void Advance(double time);
    void Increment(const char* key, int amount = 1);
    void Refresh();
    void Sample(bool force = false);
    static Alignment Align(const std::vector<Grapheme>& expected,
                           const std::vector<Grapheme>& typed, bool final);
};
} // namespace rimes::practice
