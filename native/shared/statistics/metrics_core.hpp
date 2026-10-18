#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace rimes::metrics {
using Json = nlohmann::json;
inline constexpr std::int64_t kMaximumCounter = 1000000000;
inline constexpr std::size_t kMaximumFileBytes = 4 * 1048576;
inline constexpr std::size_t kMaximumDays = 36600;

// Only sanitized measurements cross this boundary. There is deliberately no
// input text, schema, application identity, focus token or delivery callback.
struct Event {
    enum class Kind { Key, Commit, Chord } kind = Kind::Key;
    double timestamp = 0;
    std::string key;
    bool repeat = false, shortcut = false;
    std::int64_t characters = 0;
    double chord_duration = 0;
    int handled_releases = 0;
};
bool CountsForActivity(const Event& event);
std::optional<int> DaySerial(const std::string& day);
std::string DayFromSerial(int day);
std::string LocalDay(double timestamp);
std::vector<std::string> DayRange(const std::string& end, int count);
bool ValidKeysFile(const Json& value);
bool ValidActivityFile(const Json& value);
double CharactersPerMinute(const Json& record);

class KeyFrequency {
  public:
    explicit KeyFrequency(Json file = Json());
    bool Record(const Event& event, const std::string& day);
    Json Snapshot(const std::string& day) const;
    void Clear(const std::optional<std::string>& day, double now);
    const Json& File() const { return file_; }

  private:
    Json file_;
};

class TypingActivity {
  public:
    explicit TypingActivity(Json file = Json());
    bool Consume(const Event& event, const std::string& day);
    void EndSession();
    void Clear(double now);
    const Json& File() const { return file_; }

  private:
    Json file_;
    std::optional<double> last_at_;
};
} // namespace rimes::metrics
