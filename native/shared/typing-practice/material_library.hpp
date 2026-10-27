// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace rimes::practice::materials {
using Json = nlohmann::json;
struct TrackRow {
    std::string text;
    std::size_t start = 0, length = 0;
    int indent = 0;
};
std::vector<std::string> Characters(const std::string& text);
std::string ScoredText(const std::string& text);
std::vector<TrackRow> TrackRows(const std::string& text, int columns, int indent = 2);
std::vector<std::string> Passages(const std::vector<std::string>& paragraphs);
// Same local book envelope and stable epub.<SHA-256-prefix>.<number> identities
// as TypingTestLibraryStore. No extracted files, scripts or network requests.
Json ParseEPUB(const std::filesystem::path& path);
std::vector<Json> Articles(const Json& book);
class Library {
 public:
    explicit Library(std::filesystem::path user_root);
    std::vector<Json> Books() const;
    std::vector<Json> Articles() const;
    Json Import(const std::filesystem::path& path);
    void Remove(const std::string& book_id);
 private:
    std::filesystem::path root_;
};
}
