// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#include "metrics.hpp"

#include <icu.h>
#include <sddl.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <numeric>
#include <stdexcept>

#include "../core/buffer_keys.hpp"
#include "provider.hpp"

namespace rimes::windows::metrics {
namespace {
constexpr std::size_t kBytes = 4 * 1024 * 1024;
void Add(Json& j, const char* key, unsigned n = 1) {
  j[key] = (std::min)(j.contains("expected") ? 1000000ULL : 1000000000ULL,
                      j.value(key, 0ULL) + n);
}
Json Empty() {
  return {{"version", 1},
          {"recordKeys", true},
          {"recordActivity", true},
          {"days", Json::object()},
          {"sessions", Json::array()},
          {"results", Json::array()}};
}
void Safe(const std::filesystem::path& path) {
  for (auto p = path; !p.empty(); p = p.parent_path()) {
    auto flags = GetFileAttributesW(p.c_str());
    if (flags != INVALID_FILE_ATTRIBUTES &&
        (flags & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error(
          "统计路径包含链接，已停止写入；请恢复普通目录。");
    if (p == p.parent_path()) break;
  }
}
void PrivateDirectory(const std::filesystem::path& path) {
  Safe(path);
  std::filesystem::create_directories(path);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  // Owner and SYSTEM only; descendants inherit the protected DACL.
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor,
          nullptr))
    throw std::runtime_error("无法设置统计目录权限。");
  const auto ok = SetFileSecurityW(
      path.c_str(),
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      descriptor);
  LocalFree(descriptor);
  if (!ok) throw std::runtime_error("无法保护统计目录。");
}
void ValidateNumberTree(const Json& value) {
  if (value.is_number()) {
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0 || number > 1e12)
      throw std::runtime_error("统计数值无效。");
  } else if (value.is_structured())
    for (const auto& child : value) ValidateNumberTree(child);
}
void Validate(const Json& j) {
  if (j.size() != 6) throw std::runtime_error("统计文件含未知字段。");
  if (j.at("version") != 1 || !j.at("recordKeys").is_boolean() ||
      !j.at("recordActivity").is_boolean() || !j.at("days").is_object() ||
      j.at("days").size() > 36600 || !j.at("sessions").is_array() ||
      j.at("sessions").size() > 500 || !j.at("results").is_array() ||
      j.at("results").size() > 500)
    throw std::runtime_error("统计文件格式无效。");
  ValidateNumberTree(j);
  for (const auto& [day, value] : j.at("days").items()) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-' ||
        !value.at("keys").is_object())
      throw std::runtime_error("统计日期无效。");
    int year = 0, month = 0, date = 0;
    if (sscanf_s(day.c_str(), "%d-%d-%d", &year, &month, &date) != 3 ||
        year < 1970 || year > 3000 || month < 1 || month > 12 || date < 1 ||
        date > 31 || value.size() != 6)
      throw std::runtime_error("统计日期无效。");
    tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = date;
    parts.tm_hour = 12;
    parts.tm_isdst = -1;
    if (Day(static_cast<double>(mktime(&parts))) != day)
      throw std::runtime_error("统计日期无效。");
    for (const auto& [key, count] : value.at("keys").items()) {
      if (key.empty() || key.size() > 3 ||
          key.find_first_not_of("0123456789") != std::string::npos ||
          std::stoi(key) < 1 || std::stoi(key) > 255 ||
          !count.is_number_integer() || count.get<double>() > 1e9)
        throw std::runtime_error("按键统计无效。");
    }
    for (const auto key :
         {"keyCount", "characters", "chords", "active", "sessionCount"})
      if (!value.at(key).is_number() || value.at(key).get<double>() > 1e9)
        throw std::runtime_error("统计计数无效。");
  }
  for (const auto& session : j.at("sessions")) {
    if (!session.is_object() || session.size() != 7 ||
        !session.at("day").is_string() || !session.at("start").is_number() ||
        !session.at("end").is_number() ||
        session.at("end") < session.at("start"))
      throw std::runtime_error("输入会话无效。");
    for (const auto key : {"keys", "characters", "chords", "active"})
      if (!session.at(key).is_number() || session.at(key).get<double>() > 1e9)
        throw std::runtime_error("输入会话计数无效。");
  }
  for (const auto& result : j.at("results")) {
    if (!result.at("articleID").is_string() ||
        !result.at("context").is_object() ||
        !result.at("metrics").is_object() || !result.at("reasons").is_array() ||
        !result.at("complete").is_boolean() ||
        !result.at("completedAt").is_number())
      throw std::runtime_error("测速成绩无效。");
    const auto& m = result.at("metrics");
    if (m.size() != 22) throw std::runtime_error("测速指标含未知字段。");
    if (result.size() != 8 ||
        result.at("articleID").get<std::string>().size() > 256 ||
        !result.at("articleVersion").is_number_integer() ||
        (result.at("language") != "chinese" &&
         result.at("language") != "english"))
      throw std::runtime_error("测速成绩标识无效。");
    for (const auto key :
         {"keys", "repeats", "backspaces", "compositionBackspaces",
          "committedBackspaces", "deleted", "corrections", "attempts",
          "correctAttempts"})
      if (!m.at(key).is_number_integer() || m.at(key).get<double>() > 1000000)
        throw std::runtime_error("测速成绩计数无效。");
    for (const auto key : {"expected", "committed", "correct", "substitutions",
                           "omissions", "extras"})
      if (!m.at(key).is_number_integer() || m.at(key).get<double>() > 2048)
        throw std::runtime_error("测速成绩字数无效。");
    if (m.at("expected") == 0 || m.at("committed") == 0 ||
        m.at("attempts") < m.at("committed") ||
        m.at("correctAttempts") > m.at("attempts") ||
        m.at("correct").get<unsigned>() +
                m.at("substitutions").get<unsigned>() +
                m.at("extras").get<unsigned>() !=
            m.at("committed").get<unsigned>() ||
        m.at("correct").get<unsigned>() +
                m.at("substitutions").get<unsigned>() +
                m.at("omissions").get<unsigned>() !=
            m.at("expected").get<unsigned>())
      throw std::runtime_error("测速成绩计数不一致。");
    for (const auto key : {"processAccuracy", "finalAccuracy"})
      if (!m.at(key).is_null() &&
          (!m.at(key).is_number() || m.at(key).get<double>() > 1))
        throw std::runtime_error("测速正确率无效。");
    for (const auto& [key, value] : result.at("context").items()) {
      if (key != "inputSource" && key != "schema" && key != "keymapRevision" &&
          key != "comparison" && key != "chordAvailable" &&
          key != "repeatArticle")
        throw std::runtime_error("测速上下文含未知字段。");
      if ((key == "chordAvailable" || key == "repeatArticle")
              ? !value.is_boolean()
              : (!value.is_string() || value.get<std::string>().size() > 512))
        throw std::runtime_error("测速上下文无效。");
    }
    if (m.at("elapsed").get<double>() <= 0 ||
        m.at("elapsed").get<double>() > 86400 || !m.at("samples").is_array() ||
        m.at("samples").size() > 120 ||
        m.at("committed").get<unsigned>() > 2048)
      throw std::runtime_error("测速成绩超出范围。");
    for (const auto key : {"effectiveCPM", "rawCPM"})
      if (!m.at(key).is_number()) throw std::runtime_error("测速速度无效。");
    if (!m.at("chords").is_null() && (!m.at("chords").is_number_integer() ||
                                      m.at("chords").get<double>() > 1000000))
      throw std::runtime_error("并击计数无效。");
    double lastSample = -1;
    for (const auto& sample : m.at("samples")) {
      if (!sample.is_array() || sample.size() != 2 || !sample[0].is_number() ||
          !sample[1].is_number() || sample[0].get<double>() < lastSample ||
          sample[0].get<double>() > m.at("elapsed").get<double>())
        throw std::runtime_error("测速曲线无效。");
      lastSample = sample[0].get<double>();
    }
    for (const auto& reason : result.at("reasons"))
      if (reason != "incomplete" && reason != "focusLost" &&
          reason != "interrupted" && reason != "assistedInput" &&
          reason != "configurationChanged" && reason != "inputSourceChanged")
        throw std::runtime_error("测速练习状态无效。");
  }
}
}  // namespace
double Now() {
  LARGE_INTEGER counter{}, frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(counter.QuadPart) /
         static_cast<double>(frequency.QuadPart);
}
double Wall() {
  FILETIME ft{};
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER v{};
  v.LowPart = ft.dwLowDateTime;
  v.HighPart = ft.dwHighDateTime;
  return static_cast<double>(v.QuadPart - 116444736000000000ULL) / 1e7;
}
std::string Day(double wall) {
  const auto t = static_cast<time_t>(wall);
  tm local{};
  localtime_s(&local, &t);
  char out[16]{};
  strftime(out, sizeof(out), "%Y-%m-%d", &local);
  return out;
}
std::string Normalize(std::string text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      out += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else
      out += text[i];
  }
  return out;
}
std::vector<std::wstring> Characters(const std::string& text) {
  const auto wide = workbench::Wide(Normalize(text));
  if (!text.empty() && wide.empty()) throw std::runtime_error("Invalid UTF-8");
  UErrorCode error = U_ZERO_ERROR;
  auto* iterator =
      ubrk_open(UBRK_CHARACTER, "", reinterpret_cast<const UChar*>(wide.data()),
                static_cast<int>(wide.size()), &error);
  if (U_FAILURE(error) || !iterator)
    throw std::runtime_error("Unicode character iterator unavailable");
  std::vector<std::wstring> out;
  auto begin = ubrk_first(iterator);
  for (auto end = ubrk_next(iterator); end != UBRK_DONE;
       begin = end, end = ubrk_next(iterator)) {
    auto part = wide.substr(begin, end - begin);
    // Swift Character compares canonical equivalents; keep UTF-16 offsets in
    // the UI separately, normalizing only comparison tokens here.
    int size = NormalizeString(NormalizationC, part.data(),
                               static_cast<int>(part.size()), nullptr, 0);
    if (size > 0) {
      std::wstring normalized(size, L'\0');
      size = NormalizeString(NormalizationC, part.data(),
                             static_cast<int>(part.size()), normalized.data(),
                             size);
      if (size > 0) {
        normalized.resize(size);
        part = std::move(normalized);
      }
    }
    out.push_back(std::move(part));
  }
  ubrk_close(iterator);
  return out;
}
bool TypingKey(unsigned key, unsigned modifiers) {
  if (core::HasHostCommandModifier(modifiers)) return false;
  return (key >= '0' && key <= 'Z') ||
         (key >= VK_NUMPAD0 && key <= VK_DIVIDE) ||
         (key >= VK_OEM_1 && key <= VK_OEM_3) ||
         (key >= VK_OEM_4 && key <= VK_OEM_8) || key == VK_OEM_102 ||
         key == VK_BACK || key == VK_DELETE || key == VK_RETURN ||
         key == VK_TAB || key == VK_SPACE;
}
Session::Session(Article article, Json context)
    : article_(std::move(article)),
      context_(std::move(context)),
      expected_(Characters(article_.text)) {
  if (expected_.empty() || expected_.size() > 2048 ||
      article_.text.size() > 16384)
    throw std::runtime_error("Invalid practice article");
  metrics_ = {
      {"elapsed", 0.0},
      {"keys", 0},
      {"repeats", 0},
      {"backspaces", 0},
      {"compositionBackspaces", 0},
      {"committedBackspaces", 0},
      {"deleted", 0},
      {"corrections", 0},
      {"attempts", 0},
      {"correctAttempts", 0},
      {"expected", expected_.size()},
      {"chords",
       context_.value("chordAvailable", false) ? Json(0) : Json(nullptr)},
      {"samples", Json::array()}};
  alignment_ = Align(expected_, committed_, false);
  Refresh();
}
void Session::Start(double now) {
  if (!finished_ && std::isfinite(now) && now >= 0 && start_ < 0)
    start_ = last_ = now;
}
void Session::Tick(double now) {
  if (finished_ || start_ < 0 || !std::isfinite(now) || now < last_) return;
  last_ = now;
  metrics_["elapsed"] = (std::min)(86400.0, now - start_);
  if (now - start_ > 86400) Mark("interrupted");
  Sample(false);
}
void Session::Key(double now, bool repeat, bool backspace, bool composing) {
  if (finished_ || !std::isfinite(now) || now < last_) return;
  Start(now);
  Tick(now);
  Add(metrics_, repeat ? "repeats" : "keys");
  if (backspace) {
    Add(metrics_, "backspaces");
    Add(metrics_, composing ? "compositionBackspaces" : "committedBackspaces");
  }
}
void Session::UpgradeBackspace() {
  if (metrics_["committedBackspaces"].get<unsigned>()) {
    metrics_["committedBackspaces"] =
        metrics_["committedBackspaces"].get<unsigned>() - 1;
    Add(metrics_, "compositionBackspaces");
  }
}
void Session::Chord(double now) {
  if (Started() && !finished_ && !metrics_["chords"].is_null()) {
    Tick(now);
    Add(metrics_, "chords");
  }
}
void Session::Mark(const std::string& reason) {
  if (!finished_ &&
      std::find(reasons_.begin(), reasons_.end(), reason) == reasons_.end())
    reasons_.push_back(reason);
}
Session::Alignment Session::Align(const std::vector<std::wstring>& expected,
                                  const std::vector<std::wstring>& typed,
                                  bool final) {
  const auto width = expected.size() + 1;
  std::vector<int> previous(width), pm(width, 0);
  std::iota(previous.begin(), previous.end(), 0);
  std::vector<unsigned char> dirs(width * (typed.size() + 1), 0);
  for (std::size_t c = 1; c < width; ++c) dirs[c] = 2;
  for (std::size_t r = 1; r <= typed.size(); ++r) {
    std::vector<int> costs(width), matches(width);
    costs[0] = static_cast<int>(r);
    dirs[r * width] = 3;
    for (std::size_t c = 1; c < width; ++c) {
      bool equal = typed[r - 1] == expected[c - 1];
      int cost = previous[c - 1] + !equal, count = pm[c - 1] + equal;
      unsigned char direction = 1;
      if (costs[c - 1] + 1 < cost ||
          (costs[c - 1] + 1 == cost && matches[c - 1] > count)) {
        cost = costs[c - 1] + 1;
        count = matches[c - 1];
        direction = 2;
      }
      if (previous[c] + 1 < cost ||
          (previous[c] + 1 == cost && pm[c] > count)) {
        cost = previous[c] + 1;
        count = pm[c];
        direction = 3;
      }
      costs[c] = cost;
      matches[c] = count;
      dirs[r * width + c] = direction;
    }
    previous = std::move(costs);
    pm = std::move(matches);
  }
  std::size_t endpoint = expected.size();
  if (!final) {
    endpoint = 0;
    for (std::size_t c = 1; c < width; ++c)
      if (previous[c] < previous[endpoint] ||
          (previous[c] == previous[endpoint] && pm[c] >= pm[endpoint]))
        endpoint = c;
  }
  Alignment out;
  out.states.resize(expected.size());
  out.correct.resize(typed.size());
  out.consumed = static_cast<int>(endpoint);
  auto r = typed.size(), c = endpoint;
  while (r || c) {
    switch (dirs[r * width + c]) {
      case 1:
        --r;
        --c;
        if (typed[r] == expected[c]) {
          out.states[c] = 1;
          out.correct[r] = true;
          ++out.matches;
        } else {
          out.states[c] = 2;
          ++out.substitutions;
        }
        break;
      case 2:
        --c;
        out.states[c] = 3;
        ++out.omissions;
        break;
      default:
        --r;
        ++out.extras;
        break;
    }
  }
  return out;
}
bool Session::Reconcile(
    const std::string& text, double now,
    std::optional<std::pair<std::size_t, std::size_t>> insertion) {
  if (finished_ || !std::isfinite(now) || now < last_ || text.size() > 16384 ||
      text.find('\0') != std::string::npos)
    return false;
  auto next = Characters(text);
  if (next.size() > 2048) return false;
  if (next == committed_ && !insertion) return true;
  // Minimal insert/delete script, preserving unchanged islands across edits.
  const auto width = committed_.size() + 1;
  std::vector<unsigned short> dp(width * (next.size() + 1));
  for (std::size_t r = 1; r <= next.size(); ++r)
    for (std::size_t c = 1; c < width; ++c)
      dp[r * width + c] =
          next[r - 1] == committed_[c - 1]
              ? dp[(r - 1) * width + c - 1] + 1
              : (std::max)(dp[(r - 1) * width + c], dp[r * width + c - 1]);
  std::vector<std::size_t> inserted;
  unsigned removed = 0;
  auto r = next.size(), c = committed_.size();
  while (r || c) {
    if (r && c && next[r - 1] == committed_[c - 1]) {
      --r;
      --c;
    } else if (r && (!c || dp[(r - 1) * width + c] >= dp[r * width + c - 1]))
      inserted.push_back(--r);
    else {
      --c;
      ++removed;
    }
  }
  if (insertion) {
    const auto [begin, end] = *insertion;
    if (begin > end || end > next.size() || begin > committed_.size() ||
        committed_.size() + end - begin < next.size())
      return false;
    const auto erased = committed_.size() + end - begin - next.size();
    if (begin + erased > committed_.size() ||
        !std::equal(committed_.begin(), committed_.begin() + begin,
                    next.begin()) ||
        !std::equal(committed_.begin() + begin + erased, committed_.end(),
                    next.begin() + end))
      return false;
    inserted.clear();
    for (auto i = begin; i < end; ++i) inserted.push_back(i);
    removed = static_cast<unsigned>(erased);
  }
  Start(now);
  Tick(now);
  committed_ = std::move(next);
  alignment_ = Align(expected_, committed_, false);
  Add(metrics_, "deleted", removed);
  if (removed) Add(metrics_, "corrections");
  Add(metrics_, "attempts", static_cast<unsigned>(inserted.size()));
  for (auto index : inserted)
    if (alignment_.correct[index]) Add(metrics_, "correctAttempts");
  Refresh();
  return true;
}
void Session::Refresh() {
  metrics_["committed"] = committed_.size();
  metrics_["correct"] = alignment_.matches;
  metrics_["substitutions"] = alignment_.substitutions;
  metrics_["omissions"] = alignment_.omissions;
  metrics_["extras"] = alignment_.extras;
  const double seconds = metrics_["elapsed"].get<double>();
  metrics_["effectiveCPM"] =
      seconds > 0 ? alignment_.matches * 60 / seconds : 0;
  metrics_["rawCPM"] = seconds > 0 ? committed_.size() * 60 / seconds : 0;
  const double attempts = metrics_["attempts"].get<double>();
  metrics_["processAccuracy"] =
      attempts > 0 ? Json(metrics_["correctAttempts"].get<double>() / attempts)
                   : Json(nullptr);
  const double total = alignment_.matches + alignment_.substitutions +
                       alignment_.omissions + alignment_.extras;
  metrics_["finalAccuracy"] =
      total > 0 ? Json(alignment_.matches / total) : Json(nullptr);
}
void Session::Sample(bool force) {
  Refresh();
  auto& samples = metrics_["samples"];
  const auto time = metrics_["elapsed"].get<double>();
  if (time <= 0 ||
      (!force &&
       time - (samples.empty() ? 0 : samples.back()[0].get<double>()) <
           interval_))
    return;
  if (!samples.empty() && samples.back()[0] == time)
    samples.erase(samples.size() - 1);
  samples.push_back({time, metrics_["effectiveCPM"]});
  if (samples.size() > 120) {
    Json thin = Json::array();
    for (std::size_t i = 0; i < samples.size(); i += 2)
      thin.push_back(samples[i]);
    if (thin.back() != samples.back()) thin.push_back(samples.back());
    samples = std::move(thin);
    interval_ *= 2;
  }
}
bool Session::Complete() const {
  if (result_) return result_->value("complete", false);
  return !committed_.empty() &&
         alignment_.consumed == static_cast<int>(expected_.size());
}
std::optional<Json> Session::Finish(double now, double wall) {
  if (result_) return result_;
  if (finished_ || !Started() || committed_.empty() || now < last_ ||
      !std::isfinite(now) || !std::isfinite(wall) || wall < 0 || now <= start_)
    return {};
  Tick(now);
  const bool complete = Complete();
  if (!complete) Mark("incomplete");
  alignment_ = Align(expected_, committed_, true);
  Refresh();
  Sample(true);
  finished_ = true;
  result_ = Json{
      {"articleID", article_.id},      {"articleVersion", article_.version},
      {"language", article_.language}, {"context", context_},
      {"completedAt", wall},           {"metrics", metrics_},
      {"complete", complete},          {"reasons", reasons_}};
  return result_;
}
Json Session::Snapshot() const {
  return {{"metrics", metrics_},    {"states", alignment_.states},
          {"started", Started()},   {"finished", finished_},
          {"complete", Complete()}, {"reasons", reasons_},
          {"context", context_}};
}

Service::Service(std::filesystem::path root)
    : root_(std::move(root)), data_(Empty()) {
  if (root_.empty()) {
    wchar_t local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    root_ = std::filesystem::path(local) / L"RIMES" / L"metrics-v1";
  }
  Load();
}
Service::~Service() { Save(); }
void Service::Load() {
  try {
    Safe(root_ / L"metrics.json");
    auto path = root_ / L"metrics.json";
    if (!std::filesystem::exists(path)) return;
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > kBytes)
      throw std::runtime_error("统计文件过大或类型无效。");
    std::ifstream file(path, std::ios::binary);
    auto value = Json::parse(file);
    Validate(value);
    data_ = std::move(value);
  } catch (const std::exception& e) {
    issue_ = std::string("统计存储只读，原文件已保留。 ") + e.what();
  }
}
void Service::Enable(bool statistics, bool practice) {
  std::lock_guard lock(mutex_);
  if (statistics_ != statistics) last_daily_ = -1;
  statistics_ = statistics;
  practice_ = practice;
  if (!practice) FinishLocked("configurationChanged");
}
bool Service::PracticeFocused(unsigned process) const {
  GUITHREADINFO info{sizeof(info)};
  return practice_editor_ && process == GetCurrentProcessId() &&
         GetGUIThreadInfo(editor_thread_, &info) &&
         info.hwndFocus == practice_editor_;
}
void Service::PracticeKey(unsigned key, bool repeat, DWORD stamp,
                          bool composing) {
  if (!editor_ || !session_ || session_->Finished()) return;
  for (auto& seen : ledger_)
    if (seen.key == key && seen.stamp == stamp && seen.repeat == repeat) {
      if (key == VK_BACK && composing && !seen.composing) {
        session_->UpgradeBackspace();
        seen.composing = true;
      }
      return;
    }
  ledger_.push_back({key, stamp, repeat, composing});
  if (ledger_.size() > 64) ledger_.pop_front();
  session_->Key(Now(), repeat, key == VK_BACK, composing);
}
void Service::ObserveKey(unsigned process, const core::KeyEvent& key,
                         bool composing) {
  if (!(key.event_flags &
        static_cast<unsigned>(core::KeyEventFlags::kKeyDown)) ||
      (key.event_flags & static_cast<unsigned>(core::KeyEventFlags::kTestOnly)))
    return;
  std::lock_guard lock(mutex_);
  const bool repeat =
      (key.event_flags & static_cast<unsigned>(core::KeyEventFlags::kRepeat)) !=
      0;
  if (PracticeFocused(process)) {
    if (TypingKey(key.virtual_key, key.modifiers))
      PracticeKey(key.virtual_key, repeat,
                  static_cast<DWORD>(key.timestamp_millis), composing);
    return;
  }
  Daily(key.virtual_key, repeat, key.modifiers, 0, 0, Wall());
}
void Service::ObserveChord(unsigned process) {
  std::lock_guard lock(mutex_);
  if (PracticeFocused(process)) {
    if (session_) session_->Chord(Now());
    return;
  }
  Daily(0, false, 0, 0, 1, Wall());
}
void Service::ObserveCommit(unsigned process, unsigned count) {
  std::lock_guard lock(mutex_);
  if (PracticeFocused(process)) return;
  Daily(0, false, 0, count, 0, Wall());
}
void Service::LocalKey(unsigned key, unsigned modifiers, bool repeat,
                       DWORD stamp, bool composing) {
  std::lock_guard lock(mutex_);
  if (PracticeFocused(GetCurrentProcessId()) && TypingKey(key, modifiers))
    PracticeKey(key, repeat, stamp, composing);
}
void Service::Arm(HWND editor, Article article, Json context) {
  std::lock_guard lock(mutex_);
  if (!practice_) return;
  FinishLocked("interrupted");
  last_daily_ = -1;
  editor_ = editor;
  practice_editor_ = editor;
  editor_thread_ = GetWindowThreadProcessId(editor, nullptr);
  ledger_.clear();
  bool prior = false;
  for (const auto& result : data_["results"])
    if (result["articleID"] == article.id &&
        result["articleVersion"] == article.version &&
        result["context"].value("comparison", "") ==
            context.value("comparison", ""))
      prior = true;
  context["repeatArticle"] = prior;
  session_ = std::make_unique<Session>(std::move(article), std::move(context));
}
void Service::Mark(const std::string& reason) {
  std::lock_guard lock(mutex_);
  if (session_) session_->Mark(reason);
}
void Service::Composition() {
  std::lock_guard lock(mutex_);
  if (editor_ && session_) session_->Start(Now());
}
bool Service::Reconcile(
    const std::string& text,
    std::optional<std::pair<std::size_t, std::size_t>> insertion) {
  std::unique_lock lock(mutex_);
  if (!editor_ || !session_) return false;
  const bool ok = session_->Reconcile(text, Now(), insertion);
  if (!ok)
    FinishLocked("assistedInput");
  else if (session_->Complete())
    FinishLocked("");
  const bool ended = !editor_;
  lock.unlock();
  if (ended) Save();
  return ok;
}
void Service::FinishLocked(const std::string& reason) {
  if (!editor_ || !session_) return;
  if (!reason.empty()) session_->Mark(reason);
  if (auto result = session_->Finish(Now(), Wall())) {
    if (issue_.empty()) {
      data_["results"].push_back(*result);
      while (data_["results"].size() > 500) data_["results"].erase(0);
      dirty_ = true;
    }
  }
  editor_ = nullptr;
  last_daily_ = -1;
}
void Service::Finish(const std::string& reason) {
  std::unique_lock lock(mutex_);
  FinishLocked(reason);
  lock.unlock();
  Save();
}
void Service::ResetPractice() {
  std::lock_guard lock(mutex_);
  if (!editor_) {
    session_.reset();
    ledger_.clear();
  }
}
Json Service::Practice() {
  std::lock_guard lock(mutex_);
  if (session_) {
    if (editor_) session_->Tick(Now());
    return session_->Snapshot();
  }
  return Json::object();
}
void Service::ForgetEditor(HWND editor) {
  std::lock_guard lock(mutex_);
  if (practice_editor_ == editor) {
    practice_editor_ = nullptr;
    editor_thread_ = 0;
  }
}
bool Service::Armed() {
  std::lock_guard lock(mutex_);
  return editor_ != nullptr;
}
void Service::Daily(unsigned key, bool repeat, unsigned modifiers,
                    unsigned commits, unsigned chords, double time) {
  // Caller owns mutex (test callers are single-threaded). No text enters here.
  if (!statistics_ || !issue_.empty() || !std::isfinite(time) || time < 0)
    return;
  const bool frequency =
      key > 0 && key < 256 && !repeat && data_["recordKeys"].get<bool>();
  const bool activity =
      data_["recordActivity"].get<bool>() &&
      ((!repeat && TypingKey(key, modifiers)) || commits || chords);
  if (!frequency && !activity) return;
  const auto day = Day(time);
  if (!data_["days"].contains(day))
    data_["days"][day] = {{"keys", Json::object()}, {"keyCount", 0},
                          {"characters", 0},        {"chords", 0},
                          {"active", 0.0},          {"sessionCount", 0}};
  if (frequency) Add(data_["days"][day]["keys"], std::to_string(key).c_str());
  if (activity)
    Activity(day, (!repeat && TypingKey(key, modifiers)) ? 1 : 0, commits,
             chords, time);
  while (data_["days"].size() > 36600)
    data_["days"].erase(data_["days"].begin());
  dirty_ = true;
}
void Service::Activity(const std::string& day, unsigned key, unsigned commits,
                       unsigned chords, double time) {
  auto& sessions = data_["sessions"];
  auto& d = data_["days"][day];
  if (last_daily_ < 0 || time < last_daily_ || time - last_daily_ > 10 ||
      sessions.empty() || sessions.back()["day"] != day) {
    sessions.push_back({{"day", day},
                        {"start", time},
                        {"end", time},
                        {"keys", 0},
                        {"characters", 0},
                        {"chords", 0},
                        {"active", 0.0}});
    Add(d, "sessionCount");
    last_daily_ = -1;
  }
  const double delta = last_daily_ < 0 ? 0 : time - last_daily_;
  auto& s = sessions.back();
  s["end"] = time;
  d["active"] = (std::min)(1e9, d["active"].get<double>() + delta);
  s["active"] = (std::min)(1e9, s["active"].get<double>() + delta);
  Add(d, "keyCount", key);
  Add(d, "characters", commits);
  Add(d, "chords", chords);
  Add(s, "keys", key);
  Add(s, "characters", commits);
  Add(s, "chords", chords);
  last_daily_ = time;
  while (sessions.size() > 500) sessions.erase(0);
}
Json Service::Data() {
  std::lock_guard lock(mutex_);
  return data_;
}
std::string Service::Issue() {
  std::lock_guard lock(mutex_);
  return issue_;
}
void Service::Preferences(bool keys, bool activity) {
  std::unique_lock lock(mutex_);
  if (!issue_.empty()) return;
  data_["recordKeys"] = keys;
  data_["recordActivity"] = activity;
  last_daily_ = -1;
  dirty_ = true;
  lock.unlock();
  Save();
}
bool Service::Clear(const std::string& kind, const std::string& day) {
  std::unique_lock lock(mutex_);
  if (!issue_.empty()) return false;
  if (kind == "results")
    data_["results"] = Json::array();
  else if (kind == "activity") {
    data_["sessions"] = Json::array();
    for (auto& d : data_["days"]) {
      d["keyCount"] = d["characters"] = d["chords"] = d["sessionCount"] = 0;
      d["active"] = 0.0;
    }
  } else if (kind == "keys") {
    for (auto& [date, d] : data_["days"].items())
      if (day.empty() || date == day) d["keys"] = Json::object();
  } else
    return false;
  last_daily_ = -1;
  dirty_ = true;
  lock.unlock();
  Save();
  return Issue().empty();
}
void Service::WriteSnapshot(Json value) {
  PrivateDirectory(root_);
  const auto path = root_ / L"metrics.json";
  Safe(path);
  while (value.dump().size() > kBytes) {
    if (!value["results"].empty())
      value["results"].erase(0);
    else if (!value["sessions"].empty())
      value["sessions"].erase(0);
    else if (!value["days"].empty())
      value["days"].erase(value["days"].begin());
    else
      throw std::runtime_error("统计文件超出大小限制。");
  }
  const auto bytes = value.dump();
  const auto temp =
      root_ / (L"metrics-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
  Safe(temp);
  HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入统计文件。");
  DWORD written = 0;
  const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                            &written, nullptr) &&
                  written == bytes.size() && FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || !MoveFileExW(temp.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("无法保存统计文件。");
}
void Service::Save() {
  Json snapshot;
  std::uint64_t sequence;
  {
    std::lock_guard lock(mutex_);
    if (!dirty_ || !issue_.empty()) return;
    snapshot = data_;
    sequence = ++save_sequence_;
    dirty_ = false;
  }
  // Disk and JSON encoding never hold the input-observation mutex. A later
  // snapshot wins even if two UI/runtime saves reach the writer out of order.
  std::lock_guard writer(save_mutex_);
  if (sequence <= written_sequence_) return;
  try {
    WriteSnapshot(std::move(snapshot));
    written_sequence_ = sequence;
  } catch (const std::exception& e) {
    std::lock_guard lock(mutex_);
    issue_ = e.what();
    dirty_ = true;
  }
}
bool Service::Repair() {
  std::unique_lock writer(save_mutex_);
  std::unique_lock lock(mutex_);
  if (issue_.empty()) return true;
  try {
    Safe(root_);
    const auto file = root_ / L"metrics.json";
    auto backup =
        root_ /
        (L"metrics-backup-" +
         std::to_wstring(static_cast<unsigned long long>(Wall())) + L".json");
    // Rename the entry, never read or follow a damaged file/symlink.
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !MoveFileExW(file.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH))
      return false;
    data_ = Empty();
    issue_.clear();
    dirty_ = true;
    last_daily_ = -1;
    lock.unlock();
    writer.unlock();
    Save();
    return Issue().empty();
  } catch (...) {
    return false;
  }
}
}  // namespace rimes::windows::metrics
