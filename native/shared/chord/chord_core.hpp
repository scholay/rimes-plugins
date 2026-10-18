#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rimes::chord {
using Json = nlohmann::json;
inline constexpr std::size_t kMaximumProfileBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kMaximumProfiles = 128;
inline constexpr char kPluginID[] = "builtin.fly-chord-learning";
const Json& Contract();
bool IsUUID(std::string_view value);
bool IsChordSchema(std::string_view value);
enum class Half { Left, Right };
enum class Shape { Left, Right, Both };
struct Entry {
    std::string keys, output;
    bool syllable = false;
};
struct Profile {
    std::string id, name, left, right, native_schema;
    bool legacy = false, ziranma = false;
    std::vector<Entry> mappings;
    static Profile Parse(const Json& value, bool require_mappings = true);
    static Profile Decode(std::string_view bytes, bool require_mappings = true);
    static Profile BuiltIn();
    static std::vector<Profile> NativeProfiles();
    Json Value() const;
    bool IsPreset() const;
    std::string SchemaID() const;
    std::string Canonical(std::string_view keys) const;
    const Entry* Find(std::string_view keys) const;
    std::optional<Half> KeyHalf(std::int32_t key) const;
    std::optional<std::string> Encoded(const Entry& entry) const;
    bool InsertBoundary(std::string_view keys) const;
    bool MayCombine(std::string_view keys) const;
    bool MayAwaitComplement(std::string_view keys) const;
    std::vector<std::string> Algebra() const;
    std::string SchemaYAML() const;
};

struct Context {
    std::string input;
    std::size_t raw_caret = 0, cursor = 0, selection_start = 0, selection_end = 0;
    bool operator==(const Context& other) const;
};
struct Boundary {
    bool before = false, after = false;
};
Boundary Plan(const Context& context, const Profile& profile);
std::optional<std::size_t> InsertedScalarCount(std::string_view before, std::string_view after);
std::optional<Shape> BatchShape(std::string_view keys, const Profile& profile);

// Press order is retained. Repeats never enter the replay list twice. Timers
// and exact host authority stay with the platform adapter.
class Batch {
  public:
    bool Stage(std::int32_t key, const Profile& profile);
    bool Pending() const { return !keys_.empty(); }
    const std::string& Keys() const { return keys_; }
    std::string Take();
    void Reset() { keys_.clear(); }

  private:
    std::string keys_;
};
struct SettledLeft {
    std::string keys, base_input;
    Context settled;
    Boundary boundary;
    std::size_t inserted_scalars = 0;
};
class Pairing {
  public:
    void Record(std::string keys, std::string base, const Context& settled, Boundary boundary,
                const Profile& profile);
    std::optional<SettledLeft> Take(std::string_view keys, const Context& current,
                                    const Profile& profile);
    void Reset() { left_.reset(); }

  private:
    std::optional<SettledLeft> left_;
};

struct Exercise {
    std::string id, keys, output;
};
struct Course {
    std::string id, title;
    std::size_t key_count;
    std::vector<Exercise> mappings;
};
std::vector<Course> Curriculum(const Profile& profile);
std::vector<Exercise> Sample(const Course& course, std::size_t limit, const Json& items,
                             std::uint64_t seed);
bool Matches(std::string_view captured, std::string_view expected);
} // namespace rimes::chord
