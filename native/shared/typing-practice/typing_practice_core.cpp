#include "typing_practice_core.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <unicode/brkiter.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>

namespace rimes::practice {
namespace {
const std::vector<std::string> counters = {
    "physicalKeyCount",          "repeatKeyCount",          "backspaceCount",
    "compositionBackspaceCount", "committedBackspaceCount", "deletedCharacterCount",
    "correctionCount",           "attemptedCharacterCount", "correctAttemptCount",
    "correctCharacterCount",     "substitutionCount",       "omissionCount",
    "extraCharacterCount",       "committedCharacterCount", "expectedCharacterCount"};
const std::set<std::string> reasons = {"focusLost",   "inputSourceChanged", "configurationChanged",
                                       "interrupted", "incomplete",         "assistedInput"};
bool Number(const Json& v, double low, double high) {
    return v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() >= low &&
           v.get<double>() <= high;
}
bool Count(const Json& v, int low = 0, int high = kMaximumCounter) {
    return Number(v, low, high) && std::floor(v.get<double>()) == v.get<double>();
}
bool Identifier(const Json& v, bool empty = false) {
    if (!v.is_string())
        return false;
    const auto& value = v.get_ref<const std::string&>();
    if ((!empty && value.empty()) || value.size() > 256 || value.find('\0') != value.npos)
        return false;
    UErrorCode error = U_ZERO_ERROR;
    int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, value.data(), value.size(), &error);
    if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error))
        return false;
    auto unicode = icu::UnicodeString::fromUTF8(value);
    for (int32_t i = 0; i < unicode.length();) {
        auto ch = unicode.char32At(i);
        i += U16_LENGTH(ch);
        if (u_iscntrl(ch))
            return false;
    }
    return true;
}
// Linear-space Myers decomposition follows Swift's edit ordering so repeated
// graphemes receive the same attempt classification as Collection.difference.
// Adapted from Swift Diffing.swift, copyright 2015-2019 Apple Inc. and the
// Swift project authors, Apache 2.0 with Runtime Library Exception.
// See LICENSES/Swift-Apache-2.0.txt and THIRD_PARTY_NOTICES.md.
std::pair<std::vector<int>, int> Difference(const std::vector<Grapheme>& before,
                                            const std::vector<Grapheme>& after) {
    struct Box {
        int l, t, r, b;
    };
    auto shrink = [&](Box box) {
        while (box.l < box.r && box.t < box.b && before[box.l] == after[box.t]) {
            ++box.l;
            ++box.t;
        }
        while (box.r > box.l && box.b > box.t && before[box.r - 1] == after[box.b - 1]) {
            --box.r;
            --box.b;
        }
        return box;
    };
    const int size = before.size() + after.size() + 2;
    std::vector<int> f(size * 2 + 1), b(size * 2 + 1);
    auto forward = [&](int k) -> int& { return f[size + k]; };
    auto backward = [&](int k) -> int& { return b[size + k]; };
    std::vector<Box> pending{shrink({0, 0, int(before.size()), int(after.size())})};
    std::vector<int> inserted;
    int removed = 0;
    while (!pending.empty()) {
        const Box box = pending.back();
        pending.pop_back();
        const int width = box.r - box.l, height = box.b - box.t;
        if (width + height <= 1) {
            if (width)
                ++removed;
            else if (height)
                inserted.push_back(box.t);
            continue;
        }
        const int delta = width - height;
        const bool odd = delta % 2 != 0;
        forward(1) = box.l;
        backward(1) = box.b;
        std::optional<Box> middle;
        bool add = false;
        int edit = -1;
        for (int depth = 0; depth <= (width + height + 1) / 2 && !middle; ++depth) {
            for (int k = depth; k >= -depth; k -= 2) {
                int right, bottom, left, top;
                bool insertion;
                if (k == -depth || (k != depth && forward(k - 1) < forward(k + 1))) {
                    right = left = forward(k + 1);
                    bottom = box.t + right - box.l - k;
                    insertion = true;
                    edit = bottom - 1;
                } else {
                    left = forward(k - 1);
                    right = left + 1;
                    bottom = box.t + right - box.l - k;
                    insertion = false;
                    edit = left;
                }
                top = depth == 0 || right != left ? bottom : bottom - 1;
                while (right < box.r && bottom < box.b && before[right] == after[bottom]) {
                    ++right;
                    ++bottom;
                }
                forward(k) = right;
                const int c = k - delta;
                if (odd && c >= -(depth - 1) && c <= depth - 1 && bottom >= backward(c)) {
                    middle = Box{left, top, right, bottom};
                    add = insertion;
                    break;
                }
            }
            if (middle)
                break;
            for (int c = depth; c >= -depth; c -= 2) {
                const int k = c + delta;
                int left, top, right, bottom;
                bool insertion;
                if (c == -depth || (c != depth && backward(c - 1) > backward(c + 1))) {
                    top = bottom = backward(c + 1);
                    left = box.l + top - box.t + k;
                    insertion = false;
                    edit = left;
                } else {
                    bottom = backward(c - 1);
                    top = bottom - 1;
                    left = box.l + top - box.t + k;
                    insertion = true;
                    edit = top;
                }
                right = depth == 0 || top != bottom ? left : left + 1;
                while (left > box.l && top > box.t && before[left - 1] == after[top - 1]) {
                    --left;
                    --top;
                }
                backward(c) = top;
                if (!odd && k >= -depth && k <= depth && left <= forward(k)) {
                    middle = Box{left, top, right, bottom};
                    add = insertion;
                    break;
                }
            }
        }
        if (!middle)
            throw std::runtime_error("Unable to align committed edit");
        if (add)
            inserted.push_back(edit);
        else
            ++removed;
        pending.push_back(shrink({middle->r, middle->b, box.r, box.b}));
        pending.push_back(shrink({box.l, box.t, middle->l, middle->t}));
    }
    return {inserted, removed};
}
} // namespace
std::vector<Grapheme> Characters(const std::string& text) {
    if (text.size() > kMaximumTextBytes || text.find('\0') != text.npos)
        throw std::runtime_error("跟打正文超出范围");
    UErrorCode error = U_ZERO_ERROR;
    int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, text.data(), text.size(), &error);
    if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error))
        throw std::runtime_error("无效 UTF-8");
    auto unicode = icu::UnicodeString::fromUTF8(text);
    unicode.findAndReplace(icu::UnicodeString("\r\n"), icu::UnicodeString("\n"));
    unicode.findAndReplace(icu::UnicodeString("\r"), icu::UnicodeString("\n"));
    error = U_ZERO_ERROR;
    const auto* normalizer = icu::Normalizer2::getNFCInstance(error);
    std::unique_ptr<icu::BreakIterator> iterator(
        icu::BreakIterator::createCharacterInstance(icu::Locale::getRoot(), error));
    if (U_FAILURE(error) || !iterator || !normalizer)
        throw std::runtime_error("Unicode 规则不可用");
    iterator->setText(unicode);
    std::vector<Grapheme> output;
    for (int32_t from = iterator->first(), to; (to = iterator->next()) != icu::BreakIterator::DONE;
         from = to) {
        Grapheme value;
        auto part = unicode.tempSubStringBetween(from, to);
        part.toUTF8String(value.text);
        icu::UnicodeString canonical;
        normalizer->normalize(part, canonical, error);
        if (U_FAILURE(error))
            throw std::runtime_error("字符无法归一");
        canonical.toUTF8String(value.canonical);
        output.push_back(std::move(value));
        if (output.size() > kMaximumCharacters)
            throw std::runtime_error("跟打正文超出范围");
    }
    return output;
}
std::pair<int, int> InsertionRange(const std::string& text, int start, int count) {
    (void)Characters(text);
    const auto unicode = icu::UnicodeString::fromUTF8(text);
    if (start < 0 || count < 0 || start > unicode.countChar32() ||
        count > unicode.countChar32() - start)
        throw std::runtime_error("无效的插入范围");
    auto lower = unicode.moveIndex32(0, start);
    auto upper = unicode.moveIndex32(lower, count);
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> iterator(
        icu::BreakIterator::createCharacterInstance(icu::Locale::getRoot(), error));
    if (U_FAILURE(error) || !iterator)
        throw std::runtime_error("Unicode 规则不可用");
    iterator->setText(unicode);
    if (!iterator->isBoundary(lower))
        lower = iterator->preceding(lower);
    if (!iterator->isBoundary(upper))
        upper = iterator->following(upper);
    std::string before, inserted;
    unicode.tempSubStringBetween(0, lower).toUTF8String(before);
    unicode.tempSubStringBetween(lower, upper).toUTF8String(inserted);
    const int first = Characters(before).size();
    return {first, first + int(Characters(inserted).size())};
}
Json Rates(const Json& m) {
    const double elapsed = m.at("elapsedSeconds");
    const double correct = m.at("correctCharacterCount"),
                 committed = m.at("committedCharacterCount");
    const double attempts = m.at("attemptedCharacterCount"),
                 correct_attempts = m.at("correctAttemptCount");
    const double expected = m.at("expectedCharacterCount"),
                 substitutions = m.at("substitutionCount"), omissions = m.at("omissionCount"),
                 extras = m.at("extraCharacterCount");
    auto rate = [elapsed](double n, double scale) {
        return elapsed > 0 ? n * scale / elapsed : 0.;
    };
    auto ratio = [](double a, double b) -> Json { return b > 0 ? Json(a / b) : Json(); };
    return {
        {"effectiveCPM", rate(correct, 60)},
        {"rawCPM", rate(committed, 60)},
        {"wordsPerMinute", rate(correct, 60) / 5},
        {"keysPerSecond", rate(m.at("physicalKeyCount"), 1)},
        {"chordsPerSecond", m.contains("chordCount") ? Json(rate(m.at("chordCount"), 1)) : Json()},
        {"processAccuracy", ratio(correct_attempts, attempts)},
        {"finalAccuracy", ratio(correct, correct + substitutions + omissions + extras)},
        {"progress",
         expected > 0 ? std::min(1., (correct + substitutions + omissions) / expected) : 0.},
        {"codeLength", ratio(m.at("physicalKeyCount"), committed)}};
}
Session::Session(Json article, Json context, std::string id)
    : article_(std::move(article)), context_(std::move(context)), id_(std::move(id)) {
    for (const auto& name : counters)
        metrics_[name] = 0;
    metrics_["elapsedSeconds"] = 0.;
    metrics_["speedSamples"] = Json::array();
    if (context_.value("chordCountingAvailable", false))
        metrics_["chordCount"] = 0;
    try {
        expected_ = Characters(article_.at("text"));
    } catch (...) {
        cancelled_ = true;
    }
    if (expected_.empty())
        cancelled_ = true;
    metrics_["expectedCharacterCount"] = expected_.size();
    alignment_.states.resize(expected_.size(), "pending");
}
bool Session::Accepts(double time) const {
    return !cancelled_ && !result_ && std::isfinite(time) && time >= 0 &&
           (!last_ || time >= *last_);
}
void Session::Start(double time) {
    if (cancelled_ || result_ || started_ || !std::isfinite(time) || time < 0)
        return;
    started_ = last_ = time;
}
void Session::Advance(double time) {
    if (!started_)
        return;
    last_ = std::max(last_.value_or(*started_), time);
    metrics_["elapsedSeconds"] = std::min(kMaximumDuration, std::max(0., time - *started_));
    if (time - *started_ > kMaximumDuration)
        Mark("interrupted");
}
void Session::Increment(const char* key, int amount) {
    metrics_[key] = std::min(kMaximumCounter, metrics_.at(key).get<int>() +
                                                  std::min(kMaximumCounter, std::max(0, amount)));
}
void Session::Key(double time, bool repeat, bool backspace, bool composing) {
    if (!Accepts(time))
        return;
    Start(time);
    Advance(time);
    Increment(repeat ? "repeatKeyCount" : "physicalKeyCount");
    if (backspace) {
        Increment("backspaceCount");
        Increment(composing ? "compositionBackspaceCount" : "committedBackspaceCount");
    }
}
void Session::Chord(double time) {
    if (!Accepts(time) || !started_ || !context_.value("chordCountingAvailable", false))
        return;
    Advance(time);
    Increment("chordCount");
}
void Session::UpgradeBackspace() {
    if (cancelled_ || result_ || metrics_.at("committedBackspaceCount") == 0)
        return;
    metrics_["committedBackspaceCount"] = metrics_.at("committedBackspaceCount").get<int>() - 1;
    metrics_["compositionBackspaceCount"] = metrics_.at("compositionBackspaceCount").get<int>() + 1;
}
bool Session::Commit(const std::string& text, double time,
                     std::optional<std::pair<int, int>> insertion) {
    if (!Accepts(time))
        return false;
    std::vector<Grapheme> next;
    try {
        next = Characters(text);
    } catch (...) {
        return false;
    }
    std::vector<int> inserted;
    int removed = 0;
    if (insertion) {
        const auto [lo, hi] = *insertion;
        if (lo < 0 || hi < lo || hi > int(next.size()) || lo > int(committed_.size()))
            return false;
        removed = int(committed_.size()) + hi - lo - int(next.size());
        if (removed < 0 || lo + removed > int(committed_.size()) ||
            !std::equal(committed_.begin(), committed_.begin() + lo, next.begin()) ||
            !std::equal(committed_.begin() + lo + removed, committed_.end(), next.begin() + hi,
                        next.end()))
            return false;
        for (int i = lo; i < hi; ++i)
            inserted.push_back(i);
    } else {
        if (next == committed_)
            return true;
        std::tie(inserted, removed) = Difference(committed_, next);
    }
    if (next == committed_ && inserted.empty() && !removed)
        return true;
    Start(time);
    Advance(time);
    committed_ = std::move(next);
    alignment_ = Align(expected_, committed_, false);
    Increment("deletedCharacterCount", removed);
    if (removed)
        Increment("correctionCount");
    Increment("attemptedCharacterCount", inserted.size());
    int correct = 0;
    for (int index : inserted)
        if (alignment_.correctness[index])
            ++correct;
    Increment("correctAttemptCount", correct);
    Refresh();
    return true;
}
void Session::Mark(const std::string& reason) {
    if (!cancelled_ && !result_ && reasons.count(reason) &&
        std::find(reasons_.begin(), reasons_.end(), reason) == reasons_.end())
        reasons_.push_back(reason);
}
void Session::Cancel() {
    if (!result_)
        cancelled_ = true;
}
void Session::Refresh() {
    metrics_["committedCharacterCount"] = committed_.size();
    metrics_["correctCharacterCount"] = alignment_.correct;
    metrics_["substitutionCount"] = alignment_.substitutions;
    metrics_["omissionCount"] = alignment_.omissions;
    metrics_["extraCharacterCount"] = alignment_.extras;
}
void Session::Sample(bool force) {
    const double elapsed = metrics_.at("elapsedSeconds");
    if (!started_ || elapsed <= 0)
        return;
    auto& samples = metrics_["speedSamples"];
    const double previous =
        samples.empty() ? 0. : samples.back().at("elapsedSeconds").get<double>();
    if (!force && elapsed - previous < sample_interval_)
        return;
    if (!samples.empty() && previous == elapsed)
        samples.erase(samples.size() - 1);
    samples.push_back(
        {{"elapsedSeconds", elapsed}, {"effectiveCPM", Rates(metrics_).at("effectiveCPM")}});
    if (samples.size() > kMaximumSamples) {
        Json kept = Json::array();
        for (std::size_t i = 0; i < samples.size(); i += 2)
            kept.push_back(samples[i]);
        if (kept.back() != samples.back())
            kept.push_back(samples.back());
        samples = std::move(kept);
        sample_interval_ *= 2;
    }
}
Json Session::Snapshot(double time) {
    if (Accepts(time)) {
        Advance(time);
        Sample();
    }
    std::string text;
    for (const auto& value : committed_)
        text += value.text;
    return {{"metrics", metrics_},
            {"committedText", text},
            {"targetStates", alignment_.states},
            {"typedCorrectness", alignment_.correctness},
            {"canComplete", !committed_.empty() && alignment_.consumed == int(expected_.size())},
            {"isStarted", started_.has_value()},
            {"isFinished", result_.has_value()},
            {"isCancelled", cancelled_},
            {"rates", Rates(metrics_)}};
}
std::optional<Json> Session::Finish(double time, double completed_at) {
    if (result_)
        return result_;
    if (!Accepts(time) || !started_ || committed_.empty() || !std::isfinite(completed_at) ||
        completed_at < 0)
        return {};
    Advance(time);
    if (metrics_.at("elapsedSeconds").get<double>() <= 0)
        return {};
    const bool complete = alignment_.consumed == int(expected_.size());
    if (!complete)
        Mark("incomplete");
    alignment_ = Align(expected_, committed_, true);
    Refresh();
    Sample(true);
    result_ = Json{{"id", id_},
                   {"articleID", article_.at("id")},
                   {"articleVersion", article_.at("version")},
                   {"language", article_.at("language")},
                   {"context", context_},
                   {"completedAt", completed_at},
                   {"metrics", metrics_},
                   {"isComplete", complete},
                   {"practiceReasons", reasons_}};
    return result_;
}
Session::Alignment Session::Align(const std::vector<Grapheme>& expected,
                                  const std::vector<Grapheme>& typed, bool final) {
    const int width = int(expected.size()) + 1;
    std::vector<int> prior_cost(width), prior_matches(width);
    std::vector<unsigned char> directions(std::size_t(width) * (typed.size() + 1));
    for (int x = 0; x < width; ++x) {
        prior_cost[x] = x;
        directions[x] = 2;
    }
    for (int y = 1; y <= int(typed.size()); ++y) {
        std::vector<int> cost(width), matches(width);
        cost[0] = y;
        directions[std::size_t(y) * width] = 3;
        for (int x = 1; x < width; ++x) {
            const bool equal = typed[y - 1] == expected[x - 1];
            int c = prior_cost[x - 1] + (equal ? 0 : 1), m = prior_matches[x - 1] + (equal ? 1 : 0);
            unsigned char direction = 1;
            if (cost[x - 1] + 1 < c || (cost[x - 1] + 1 == c && matches[x - 1] > m)) {
                c = cost[x - 1] + 1;
                m = matches[x - 1];
                direction = 2;
            }
            if (prior_cost[x] + 1 < c || (prior_cost[x] + 1 == c && prior_matches[x] > m)) {
                c = prior_cost[x] + 1;
                m = prior_matches[x];
                direction = 3;
            }
            cost[x] = c;
            matches[x] = m;
            directions[std::size_t(y) * width + x] = direction;
        }
        prior_cost = std::move(cost);
        prior_matches = std::move(matches);
    }
    int endpoint = int(expected.size());
    if (!final) {
        endpoint = 0;
        for (int x = 1; x < width; ++x)
            if (prior_cost[x] < prior_cost[endpoint] ||
                (prior_cost[x] == prior_cost[endpoint] &&
                 prior_matches[x] >= prior_matches[endpoint]))
                endpoint = x;
    }
    Alignment value;
    value.states.resize(expected.size(), "pending");
    value.correctness.resize(typed.size());
    value.consumed = endpoint;
    int x = endpoint, y = typed.size();
    while (x || y) {
        switch (directions[std::size_t(y) * width + x]) {
        case 1:
            --x;
            --y;
            if (typed[y] == expected[x]) {
                value.states[x] = "correct";
                value.correctness[y] = true;
                ++value.correct;
            } else {
                value.states[x] = "incorrect";
                ++value.substitutions;
            }
            break;
        case 2:
            value.states[--x] = "omitted";
            ++value.omissions;
            break;
        default:
            --y;
            ++value.extras;
            break;
        }
    }
    return value;
}
bool ValidResult(const Json& value) {
    try {
        if (!value.is_object() || !value.at("id").is_string())
            return false;
        const auto id = value.at("id").get<std::string>();
        if (id.size() != 36)
            return false;
        for (std::size_t i = 0; i < id.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (id[i] != '-')
                    return false;
            } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') ||
                         (id[i] >= 'A' && id[i] <= 'F')))
                return false;
        }
        const auto& context = value.at("context");
        const auto& m = value.at("metrics");
        if (!Identifier(value.at("articleID")) || !Count(value.at("articleVersion"), 1) ||
            (value.at("language") != "chinese" && value.at("language") != "english") ||
            !Identifier(context.at("schemaID"), true) ||
            (context.at("mode") != "firstAttempt" && context.at("mode") != "practice") ||
            !context.at("chordCountingAvailable").is_boolean() ||
            !value.at("isComplete").is_boolean() || !value.at("completedAt").is_number() ||
            !std::isfinite(value.at("completedAt").get<double>()) ||
            value.at("completedAt").get<double>() < 0 ||
            !Number(m.at("elapsedSeconds"), 0, kMaximumDuration) ||
            m.at("elapsedSeconds").get<double>() <= 0)
            return false;
        for (const char* key : {"keymapID", "inputSourceID", "keymapRevision"})
            if (context.contains(key) && !context.at(key).is_null() && !Identifier(context.at(key)))
                return false;
        if (context.contains("keymapVersion") && !context.at("keymapVersion").is_null() &&
            !Count(context.at("keymapVersion"), 1))
            return false;
        for (const auto& key : counters)
            if (!Count(m.at(key)))
                return false;
        auto count = [&](const char* key) { return m.at(key).get<int>(); };
        const int expected = count("expectedCharacterCount"),
                  committed = count("committedCharacterCount");
        const bool chords = m.contains("chordCount") && !m.at("chordCount").is_null();
        if (expected < 1 || expected > kMaximumCharacters || committed < 1 ||
            committed > kMaximumCharacters || count("attemptedCharacterCount") < committed ||
            count("correctAttemptCount") > count("attemptedCharacterCount") ||
            count("deletedCharacterCount") > count("attemptedCharacterCount") ||
            count("correctionCount") > count("deletedCharacterCount") ||
            count("compositionBackspaceCount") + count("committedBackspaceCount") !=
                count("backspaceCount") ||
            count("backspaceCount") > count("physicalKeyCount") + count("repeatKeyCount") ||
            committed != count("correctCharacterCount") + count("substitutionCount") +
                             count("extraCharacterCount") ||
            expected != count("correctCharacterCount") + count("substitutionCount") +
                            count("omissionCount") ||
            chords != context.at("chordCountingAvailable").get<bool>() ||
            (chords && !Count(m.at("chordCount"))))
            return false;
        const auto& flags = value.at("practiceReasons");
        if (!flags.is_array() || flags.size() > reasons.size())
            return false;
        std::set<std::string> seen;
        for (const auto& flag : flags)
            if (!flag.is_string() || !reasons.count(flag.get<std::string>()) ||
                !seen.insert(flag.get<std::string>()).second)
                return false;
        if (value.at("isComplete") == false && !seen.count("incomplete"))
            return false;
        Json metrics = m;
        if (!chords)
            metrics.erase("chordCount");
        const auto rates = Rates(metrics);
        for (const char* key : {"effectiveCPM", "rawCPM", "keysPerSecond"})
            if (!std::isfinite(rates.at(key).get<double>()))
                return false;
        const auto& samples = m.at("speedSamples");
        if (!samples.is_array() || samples.empty() || samples.size() > kMaximumSamples ||
            samples.back().at("elapsedSeconds") != m.at("elapsedSeconds") ||
            samples.back().at("effectiveCPM") != rates.at("effectiveCPM"))
            return false;
        double previous = 0;
        for (const auto& sample : samples) {
            const double elapsed = sample.at("elapsedSeconds").get<double>();
            if (!Number(sample.at("elapsedSeconds"), 0, m.at("elapsedSeconds")) ||
                elapsed <= previous ||
                !Number(sample.at("effectiveCPM"), 0, double(expected) * 60 / elapsed))
                return false;
            previous = elapsed;
        }
        return true;
    } catch (...) {
        return false;
    }
}
Json NormalizeResult(const Json& value) {
    if (!ValidResult(value))
        throw std::runtime_error("跟打成绩结构或数值超出范围");
    Json out;
    for (const char* key : {"id", "articleID", "articleVersion", "language", "completedAt",
                            "isComplete", "practiceReasons"})
        out[key] = value.at(key);
    auto id = out.at("id").get<std::string>();
    for (char& c : id)
        if (c >= 'a' && c <= 'f')
            c += 'A' - 'a';
    out["id"] = id;
    out["articleVersion"] = value.at("articleVersion").get<int>();
    const auto& context = value.at("context");
    for (const char* key : {"schemaID", "keymapID", "keymapVersion", "mode",
                            "chordCountingAvailable", "inputSourceID", "keymapRevision"})
        if (context.contains(key) && !context.at(key).is_null())
            out["context"][key] = context.at(key);
    if (out["context"].contains("keymapVersion"))
        out["context"]["keymapVersion"] = context.at("keymapVersion").get<int>();
    const auto& m = value.at("metrics");
    for (const auto& key : counters)
        out["metrics"][key] = m.at(key).get<int>();
    out["metrics"]["elapsedSeconds"] = m.at("elapsedSeconds");
    if (m.contains("chordCount") && !m.at("chordCount").is_null())
        out["metrics"]["chordCount"] = m.at("chordCount").get<int>();
    auto& samples = out["metrics"]["speedSamples"];
    samples = Json::array();
    for (const auto& sample : m.at("speedSamples"))
        samples.push_back({{"elapsedSeconds", sample.at("elapsedSeconds")},
                           {"effectiveCPM", sample.at("effectiveCPM")}});
    return out;
}
std::string ComparisonKey(const Json& result) {
    const auto& c = result.at("context");
    // Structured identities keep separator characters in IDs unambiguous.
    return Json::array({result.at("articleVersion"), result.at("language"), c.at("schemaID"),
                        c.value("keymapID", Json()), c.value("keymapVersion", Json()),
                        c.value("keymapRevision", Json()), c.value("inputSourceID", Json())})
        .dump();
}
} // namespace rimes::practice
