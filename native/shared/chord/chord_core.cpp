#include "chord_core.hpp"
#include "native_chord_contract.hpp"
#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <unicode/brkiter.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/utf8.h>

namespace rimes::chord {
namespace {
[[noreturn]] void Invalid(const char* reason) { throw std::runtime_error(reason); }
std::string String(const Json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string())
        Invalid("键位字段类型不正确");
    return value[key].get<std::string>();
}
std::string Scalar(std::string_view value) {
    std::string result = "'";
    for (const char byte : value) {
        result += byte;
        if (byte == '\'')
            result += '\'';
    }
    return result + "'";
}
std::string Regex(std::string_view value) {
    std::string result;
    for (const char byte : value) {
        if (byte == '.')
            result += '\\';
        result += byte;
    }
    return result;
}
std::vector<UChar32> Scalars(std::string_view text) {
    std::vector<UChar32> result;
    if (text.size() > kMaximumProfileBytes)
        Invalid("键位文本过长");
    for (int32_t index = 0; index < static_cast<int32_t>(text.size());) {
        UChar32 code;
        U8_NEXT(text.data(), index, static_cast<int32_t>(text.size()), code);
        if (code < 0)
            Invalid("键位文本不是有效 UTF-8");
        result.push_back(code);
    }
    return result;
}
void ValidateName(const std::string& value) {
    if (value.empty() || value.size() > 240)
        Invalid("名称须为 1–80 个字符");
    bool nonspace = false;
    for (const auto code : Scalars(value)) {
        const auto type = u_charType(code);
        if (type == U_CONTROL_CHAR || type == U_FORMAT_CHAR || code == 0x2028 || code == 0x2029)
            Invalid("名称不能包含换行或控制字符");
        nonspace |= !u_isUWhiteSpace(code);
    }
    if (!nonspace)
        Invalid("名称不能为空");
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> iterator(
        icu::BreakIterator::createCharacterInstance(icu::Locale::getRoot(), error));
    if (U_FAILURE(error) || !iterator)
        Invalid("无法校验名称");
    const auto unicode = icu::UnicodeString::fromUTF8(value);
    iterator->setText(unicode);
    std::size_t count = 0;
    for (iterator->first(); iterator->next() != icu::BreakIterator::DONE;)
        ++count;
    if (count > 80)
        Invalid("名称须为 1–80 个字符");
}
void ReplaceOne(std::string& text, std::string_view key, std::string_view value) {
    auto offset = text.find(key);
    if (offset == std::string::npos || text.find(key, offset + key.size()) != std::string::npos)
        Invalid("原生并击模板不匹配");
    text.replace(offset, key.size(), value);
}
} // namespace
const Json& Contract() {
    static const auto value = Json::parse(native::kChordContract);
    return value;
}
bool IsUUID(std::string_view value) {
    if (value.size() != 36)
        return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-')
                return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f') ||
                     (value[index] >= 'A' && value[index] <= 'F')))
            return false;
    }
    return true;
}
bool IsChordSchema(std::string_view value) {
    if (value == "my_combo" || value.substr(0, 12) == "rimes_chord_")
        return true;
    for (const auto& item : Contract().at("nativeProfiles"))
        if (item.at("nativeSchemeID") == value)
            return true;
    return false;
}
Profile Profile::Decode(std::string_view bytes, bool require_mappings) {
    if (bytes.size() > kMaximumProfileBytes)
        Invalid("键位文件超过 2 MiB 限制");
    return Parse(Json::parse(bytes), require_mappings);
}
Profile Profile::Parse(const Json& value, bool require_mappings) {
    if (!value.is_object() || !value.contains("formatVersion") ||
        !value["formatVersion"].is_number_integer() || value["formatVersion"] != 1)
        Invalid("不支持的键位格式版本");
    Profile result;
    result.id = String(value, "id");
    result.name = String(value, "name");
    result.left = String(value, "leftKeys");
    result.right = String(value, "rightKeys");
    if (!value.contains("mappings") || !value["mappings"].is_array())
        Invalid("键位映射须为列表");
    const auto boundary = value.value("boundaryPolicy", std::string("explicitSyllables"));
    const auto encoding = value.value("outputEncoding", std::string("fullPinyin"));
    if (boundary != "legacyBatches" && boundary != "explicitSyllables")
        Invalid("未知的音节边界类型");
    if (encoding != "fullPinyin" && encoding != "ziranma")
        Invalid("未知的输出编码");
    result.legacy = boundary == "legacyBatches";
    result.ziranma = encoding == "ziranma";
    if (value.contains("nativeSchemeID") && !value["nativeSchemeID"].is_null())
        result.native_schema = String(value, "nativeSchemeID");
    // Match Swift decoding before validation, including read-only presets.
    for (const auto& entry : value["mappings"]) {
        const auto kind = String(entry, "kind");
        if (kind != "fragment" && kind != "syllable")
            Invalid("未知的键位类型");
        result.mappings.push_back(
            {String(entry, "keys"), String(entry, "output"), kind == "syllable"});
    }
    for (const auto& preset : Contract().at("nativeProfiles"))
        if (preset.at("id") == result.id) {
            if (!result.native_schema.empty() &&
                result.native_schema != preset.at("nativeSchemeID"))
                Invalid("原生方案身份不匹配");
            result.name = preset.at("name");
            result.left = preset.at("leftKeys");
            result.right = preset.at("rightKeys");
            result.native_schema = preset.at("nativeSchemeID");
            result.legacy = true;
            result.ziranma = false;
            result.mappings.clear();
            return result;
        }
    if (!result.native_schema.empty())
        Invalid("不能导入或复制原生并击方案");
    if (result.id != "builtin.flyyao" && !IsUUID(result.id))
        Invalid("方案 ID 必须是 UUID");
    ValidateName(result.name);
    const auto alphabet = result.left + result.right;
    std::set<char> seen;
    for (char byte : alphabet)
        if (std::string_view("abcdefghijklmnopqrstuvwxyz,.").find(byte) == std::string_view::npos ||
            !seen.insert(byte).second)
            Invalid("左右键区必须由不重复的 a–z、逗号或句号组成");
    if (alphabet.empty())
        Invalid("键区不能为空");
    if ((require_mappings && result.mappings.empty()) || result.mappings.size() > 4096)
        Invalid("应用方案需要 1–4096 条映射");
    if (result.id == "builtin.flyyao" && result.ziranma)
        Invalid("请先复制内置键位，再切换输出编码");
    std::transform(result.id.begin(), result.id.end(), result.id.begin(), [](unsigned char byte) {
        return byte >= 'A' && byte <= 'Z' ? byte + 32 : byte;
    });
    std::set<std::string> mappings;
    for (auto& entry : result.mappings) {
        std::set<char> keys(entry.keys.begin(), entry.keys.end());
        if (keys.size() < 2 || keys.size() != entry.keys.size() ||
            !std::includes(seen.begin(), seen.end(), keys.begin(), keys.end()))
            Invalid("映射须含至少两个不重复的已分区键");
        entry.keys = result.Canonical(entry.keys);
        if (!mappings.insert(entry.keys).second)
            Invalid("键位组合重复（顺序无关）");
        if (entry.output.empty() || entry.output.size() > 32 ||
            !std::all_of(entry.output.begin(), entry.output.end(),
                         [](char byte) { return byte >= 'a' && byte <= 'z'; }))
            Invalid("映射输出须为 1–32 个小写字母");
        if (!result.Encoded(entry))
            Invalid("拼音不能转为自然码，请检查映射类型");
    }
    return result;
}
Profile Profile::BuiltIn() { return Parse(Contract().at("builtin")); }
std::vector<Profile> Profile::NativeProfiles() {
    std::vector<Profile> result;
    for (const auto& profile : Contract().at("nativeProfiles"))
        result.push_back(Parse(profile));
    return result;
}
Json Profile::Value() const {
    Json result = {{"formatVersion", 1},
                   {"id", id},
                   {"name", name},
                   {"leftKeys", left},
                   {"rightKeys", right},
                   {"boundaryPolicy", legacy ? "legacyBatches" : "explicitSyllables"},
                   {"outputEncoding", ziranma ? "ziranma" : "fullPinyin"},
                   {"mappings", Json::array()}};
    for (const auto& entry : mappings)
        result["mappings"].push_back({{"keys", entry.keys},
                                      {"output", entry.output},
                                      {"kind", entry.syllable ? "syllable" : "fragment"}});
    if (!native_schema.empty())
        result["nativeSchemeID"] = native_schema;
    return result;
}
bool Profile::IsPreset() const { return id == "builtin.flyyao" || !native_schema.empty(); }
std::string Profile::SchemaID() const {
    if (!native_schema.empty())
        return native_schema;
    if (id == "builtin.flyyao")
        return "my_combo";
    std::string result = "rimes_chord_";
    for (char byte : id)
        if (byte != '-')
            result += byte;
    return result;
}
std::string Profile::Canonical(std::string_view keys) const {
    std::string result;
    std::set<char> unknown(keys.begin(), keys.end());
    for (char byte : left + right)
        if (unknown.erase(byte))
            result += byte;
    for (char byte : unknown)
        result += byte;
    return result;
}
const Entry* Profile::Find(std::string_view keys) const {
    const auto canonical = Canonical(keys);
    if (canonical.size() < 2)
        return nullptr;
    for (const auto& entry : mappings)
        if (entry.keys == canonical)
            return &entry;
    return nullptr;
}
std::optional<Half> Profile::KeyHalf(std::int32_t key) const {
    if (key < 0 || key > 127)
        return std::nullopt;
    if (left.find(static_cast<char>(key)) != std::string::npos)
        return Half::Left;
    if (right.find(static_cast<char>(key)) != std::string::npos)
        return Half::Right;
    return std::nullopt;
}
std::optional<std::string> Profile::Encoded(const Entry& entry) const {
    if (!ziranma)
        return entry.output;
    const auto& values = Contract().at(entry.syllable ? "syllables" : "fragments");
    if (values.contains(entry.output))
        return values.at(entry.output).get<std::string>();
    return std::nullopt;
}
bool Profile::InsertBoundary(std::string_view keys) const {
    const auto* entry = Find(keys);
    return legacy ? keys.size() > 1 : entry && entry->syllable;
}
bool Profile::MayCombine(std::string_view keys) const {
    const auto* entry = Find(keys);
    return legacy || (entry && entry->syllable);
}
bool Profile::MayAwaitComplement(std::string_view keys) const {
    const auto* entry = Find(keys);
    return legacy || !(entry && entry->syllable);
}
std::vector<std::string> Profile::Algebra() const {
    const auto profile = Parse(Value());
    std::vector<std::string> result;
    const auto alphabet = profile.left + profile.right;
    for (auto it = alphabet.rbegin(); it != alphabet.rend(); ++it)
        result.push_back("xform/^(.*)(" + Regex(std::string(1, *it)) + ")(.*)$/$2$1$3/");
    for (std::size_t i = 0; i < mappings.size(); ++i)
        result.push_back("xform/^" + Regex(mappings[i].keys) + "$/~" + std::to_string(i) + "~/");
    result.insert(result.end(), {"xform/^([a-z,.]{2,})$/!$1!/", "xform/^!(.*)[,.](.*)!$/!$1$2!/",
                                 "xform/^!(.*)[,.](.*)!$/!$1$2!/", "xform/^!(.*)!$/$1/"});
    for (std::size_t i = 0; i < mappings.size(); ++i)
        result.push_back("xform/^~" + std::to_string(i) + "~$/" + *Encoded(mappings[i]) + "/");
    return result;
}
std::string Profile::SchemaYAML() const {
    const auto profile = Parse(Value());
    if (!native_schema.empty())
        Invalid("原生预设使用内置 Rime 方案");
    std::string result = Contract().at("templates").at(ziranma ? "ziranma" : "fullPinyin");
    std::string algebra;
    for (const auto& rule : profile.Algebra()) {
        if (!algebra.empty())
            algebra += '\n';
        algebra += "    - " + Scalar(rule);
    }
    ReplaceOne(result, "@SCHEMA_ID@", profile.SchemaID());
    ReplaceOne(result, "@NAME@", Scalar(profile.name));
    ReplaceOne(result, "@ALPHABET@", Scalar(profile.left + profile.right));
    ReplaceOne(result, "@ALGEBRA@", algebra);
    return result;
}
bool Context::operator==(const Context& other) const {
    return input == other.input && raw_caret == other.raw_caret && cursor == other.cursor &&
           selection_start == other.selection_start && selection_end == other.selection_end;
}
Boundary Plan(const Context& context, const Profile& profile) {
    const auto cursor = std::min(context.raw_caret, context.input.size());
    return {cursor > 0 && context.input[cursor - 1] != '\'',
            !profile.legacy || (cursor < context.input.size() && context.input[cursor] != '\'')};
}
std::optional<std::size_t> InsertedScalarCount(std::string_view before, std::string_view after) {
    const auto old = Scalars(before), next = Scalars(after);
    if (next.size() < old.size())
        return std::nullopt;
    const auto inserted = next.size() - old.size();
    for (std::size_t index = 0; index <= old.size(); ++index)
        if (std::equal(old.begin(), old.begin() + index, next.begin()) &&
            std::equal(old.begin() + index, old.end(), next.begin() + index + inserted))
            return inserted;
    return std::nullopt;
}
std::optional<Shape> BatchShape(std::string_view keys, const Profile& profile) {
    bool left = false, right = false;
    for (char key : keys) {
        const auto half = profile.KeyHalf(key);
        left |= half == Half::Left;
        right |= half == Half::Right;
    }
    if (left && right)
        return Shape::Both;
    if (left)
        return Shape::Left;
    if (right)
        return Shape::Right;
    return std::nullopt;
}
bool Batch::Stage(std::int32_t key, const Profile& profile) {
    if (!profile.KeyHalf(key) || keys_.size() >= 50 ||
        keys_.find(static_cast<char>(key)) != std::string::npos)
        return false;
    keys_ += static_cast<char>(key);
    return true;
}
std::string Batch::Take() {
    auto result = std::move(keys_);
    keys_.clear();
    return result;
}
void Pairing::Record(std::string keys, std::string base, const Context& settled, Boundary boundary,
                     const Profile& profile) {
    Reset();
    if (BatchShape(keys, profile) != Shape::Left || !profile.MayAwaitComplement(keys))
        return;
    const auto inserted = InsertedScalarCount(base, settled.input);
    if (inserted && *inserted)
        left_ = SettledLeft{std::move(keys), std::move(base), settled, boundary, *inserted};
}
std::optional<SettledLeft> Pairing::Take(std::string_view keys, const Context& current,
                                         const Profile& profile) {
    auto previous = std::move(left_);
    Reset();
    if (!previous || BatchShape(keys, profile) != Shape::Right ||
        (previous->keys.size() <= 1 && keys.size() <= 1) || !(previous->settled == current) ||
        !profile.MayCombine(previous->keys + std::string(keys)))
        return std::nullopt;
    return previous;
}
} // namespace rimes::chord
