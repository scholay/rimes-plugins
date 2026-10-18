#include "metrics_core.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <set>
#include <stdexcept>

namespace rimes::metrics {
namespace {
bool Number(const Json& value, double maximum = std::numeric_limits<double>::max()) {
    return value.is_number() && std::isfinite(value.get<double>()) && value >= 0 &&
           value.get<double>() <= maximum;
}
bool Counter(const Json& value) {
    return value.is_number_integer() && value >= 0 && value <= kMaximumCounter;
}
bool Days(const Json& days) {
    if (!days.is_object() || days.size() > kMaximumDays)
        return false;
    std::optional<int> first, last;
    for (const auto& [key, unused] : days.items()) {
        (void)unused;
        const auto day = DaySerial(key);
        if (!day)
            return false;
        first = first ? std::min(*first, *day) : *day;
        last = last ? std::max(*last, *day) : *day;
    }
    return !first || *last - *first < static_cast<int>(kMaximumDays);
}
bool CanAddDay(const Json& days, const std::string& day) {
    const auto serial = DaySerial(day);
    if (!serial)
        return false;
    if (days.contains(day))
        return true;
    if (days.size() >= kMaximumDays)
        return false;
    if (days.empty())
        return true;
    const auto first = std::min(*serial, *DaySerial(days.begin().key()));
    const auto last = std::max(*serial, *DaySerial(std::prev(days.end()).key()));
    return last - first < static_cast<int>(kMaximumDays);
}
bool Record(const Json& value, bool session) {
    if (!value.is_object() || !Counter(value.at("keyCount")) ||
        !Counter(value.at("committedCharacterCount")) || !Counter(value.at("chordCount")) ||
        !Number(value.at("activeSeconds"), kMaximumCounter))
        return false;
    if (!session)
        return Counter(value.at("sessionCount"));
    return Number(value.at("startedAt")) && Number(value.at("endedAt")) &&
           value.at("endedAt") >= value.at("startedAt") && value.at("dayKey").is_string() &&
           DaySerial(value.at("dayKey")).has_value();
}
Json Empty(bool activity) {
    Json result{{"version", 1}, {"days", Json::object()}, {"updatedAt", 0}};
    if (activity)
        result["sessions"] = Json::array();
    return result;
}
Json EmptyDay() {
    return {{"keyCount", 0},
            {"committedCharacterCount", 0},
            {"chordCount", 0},
            {"activeSeconds", 0.0},
            {"sessionCount", 0}};
}
void Add(Json& value, std::int64_t amount) {
    value = std::min(kMaximumCounter, value.get<std::int64_t>() + amount);
}
bool Time(double value) { return std::isfinite(value) && value >= 0; }
} // namespace

std::optional<int> DaySerial(const std::string& day) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-')
        return {};
    for (std::size_t i = 0; i < day.size(); ++i)
        if (i != 4 && i != 7 && (day[i] < '0' || day[i] > '9'))
            return {};
    int year = std::stoi(day.substr(0, 4));
    const int month = std::stoi(day.substr(5, 2)), date = std::stoi(day.substr(8, 2));
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int lengths[] = {31, 28 + leap, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || month < 1 || month > 12 || date < 1 || date > lengths[month - 1])
        return {};
    // Gregorian civil day, relative to 1970-01-01. The inverse below is used
    // for chart ranges; calendar days must not be stepped by 86400 across DST.
    year -= month <= 2;
    const int era = year / 400, yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + date - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
std::string DayFromSerial(int day) {
    if (day < -719162 || day > 2932896)
        return {};
    day += 719468;
    const int era = (day >= 0 ? day : day - 146096) / 146097;
    const int doe = day - era * 146097;
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = yoe + era * 400;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    const int date = doy - (153 * mp + 2) / 5 + 1, month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    if (year < 1 || year > 9999)
        return {};
    char output[32];
    std::snprintf(output, sizeof(output), "%04d-%02d-%02d", year, month, date);
    return output;
}
std::string LocalDay(double timestamp) {
    if (!Time(timestamp) || timestamp > 253402300799.0)
        return {};
    const auto time = static_cast<std::time_t>(timestamp);
    std::tm value{};
    if (!localtime_r(&time, &value))
        return {};
    char output[16];
    if (!std::strftime(output, sizeof(output), "%Y-%m-%d", &value))
        return {};
    return DaySerial(output) ? std::string(output) : std::string();
}
std::vector<std::string> DayRange(const std::string& end, int count) {
    const auto last = DaySerial(end);
    std::vector<std::string> result;
    if (!last || count < 1 || count > static_cast<int>(kMaximumDays))
        return result;
    for (int i = count - 1; i >= 0; --i) {
        const auto day = DayFromSerial(*last - i);
        if (!day.empty())
            result.push_back(day);
    }
    return result;
}
bool CountsForActivity(const Event& event) {
    static const std::set<std::string> navigation = {
        "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp", "PageDown"};
    if (!Time(event.timestamp))
        return false;
    switch (event.kind) {
    case Event::Kind::Key:
        return !event.repeat && !event.shortcut && !navigation.count(event.key);
    case Event::Kind::Commit:
        return event.characters > 0 && event.characters <= kMaximumCounter;
    case Event::Kind::Chord:
        return Time(event.chord_duration) && event.handled_releases >= 0;
    }
    return false;
}
bool ValidKeysFile(const Json& value) {
    try {
        if (value.at("version") != 1 || !Number(value.at("updatedAt")) || !Days(value.at("days")))
            return false;
        for (const auto& day : value.at("days")) {
            const auto& keys = day.at("keys");
            if (!keys.is_object() || keys.size() > 512)
                return false;
            for (const auto& [id, count] : keys.items())
                if (id.empty() || id.size() > 80 || !Counter(count))
                    return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
bool ValidActivityFile(const Json& value) {
    try {
        if (value.at("version") != 1 || !Number(value.at("updatedAt")) || !Days(value.at("days")))
            return false;
        const auto& sessions = value.at("sessions");
        if (!sessions.is_array() || sessions.size() > 500)
            return false;
        for (const auto& session : sessions)
            if (!Record(session, true))
                return false;
        for (const auto& day : value.at("days"))
            if (!Record(day, false))
                return false;
        return true;
    } catch (...) {
        return false;
    }
}
double CharactersPerMinute(const Json& record) {
    const double seconds = record.at("activeSeconds");
    return seconds > 0
               ? record.at("committedCharacterCount").get<double>() * 60 / std::max(1.0, seconds)
               : 0;
}
KeyFrequency::KeyFrequency(Json file) : file_(file.is_null() ? Empty(false) : std::move(file)) {
    if (!ValidKeysFile(file_))
        throw std::runtime_error("键频数据结构或数值无效；原文件已保留");
    // Match native Codable: imported, unknown fields are not retained in the
    // aggregate. In particular no added plaintext field can propagate to an export.
    Json normalized = Empty(false);
    normalized["updatedAt"] = file_.at("updatedAt");
    for (const auto& [day, value] : file_.at("days").items())
        normalized["days"][day] = {{"keys", value.at("keys")}};
    file_ = std::move(normalized);
}
bool KeyFrequency::Record(const Event& event, const std::string& day) {
    if (event.kind != Event::Kind::Key || !Time(event.timestamp) || event.key.empty() ||
        event.key.size() > 80 || !CanAddDay(file_["days"], day))
        return false;
    auto record = file_["days"].value(day, Json{{"keys", Json::object()}});
    auto& keys = record["keys"];
    if (!keys.contains(event.key) && keys.size() >= 512)
        return false;
    const auto previous = keys.value(event.key, std::int64_t(0));
    if (previous >= kMaximumCounter)
        return false;
    keys[event.key] = previous + 1;
    file_["days"][day] = record;
    file_["updatedAt"] = event.timestamp;
    return true;
}
Json KeyFrequency::Snapshot(const std::string& day) const {
    const auto keys = file_.at("days").value(day, Json{{"keys", Json::object()}}).at("keys");
    std::int64_t total = 0, maximum = 0;
    std::optional<std::string> top;
    for (const auto& [id, count] : keys.items()) {
        const auto n = count.get<std::int64_t>();
        total += n;
        if (!top || n > maximum) {
            top = id;
            maximum = n;
        }
    }
    return {{"dayKey", day},
            {"counts", keys},
            {"total", total},
            {"maxCount", maximum},
            {"topKeyId", top ? Json(*top) : Json(nullptr)}};
}
void KeyFrequency::Clear(const std::optional<std::string>& day, double now) {
    if (day)
        file_["days"].erase(*day);
    else
        file_["days"] = Json::object();
    file_["updatedAt"] = now;
}
TypingActivity::TypingActivity(Json file) : file_(file.is_null() ? Empty(true) : std::move(file)) {
    if (!ValidActivityFile(file_))
        throw std::runtime_error("输入趋势数据结构或数值无效；原文件已保留");
    Json normalized = Empty(true);
    normalized["updatedAt"] = file_.at("updatedAt");
    auto record = [](const Json& value, bool session) {
        Json result;
        for (const auto* key :
             {"keyCount", "committedCharacterCount", "chordCount", "activeSeconds"})
            result[key] = value.at(key);
        if (session)
            for (const auto* key : {"startedAt", "endedAt", "dayKey"})
                result[key] = value.at(key);
        else
            result["sessionCount"] = value.at("sessionCount");
        return result;
    };
    for (const auto& [day, value] : file_.at("days").items())
        normalized["days"][day] = record(value, false);
    for (const auto& value : file_.at("sessions"))
        normalized["sessions"].push_back(record(value, true));
    file_ = std::move(normalized);
}
bool TypingActivity::Consume(const Event& event, const std::string& day) {
    if (!CountsForActivity(event) || !CanAddDay(file_["days"], day))
        return false;
    auto& sessions = file_["sessions"];
    const bool fresh = !last_at_ || event.timestamp < *last_at_ ||
                       event.timestamp - *last_at_ > 10 || sessions.empty() ||
                       sessions.back().at("dayKey") != day;
    auto record = file_["days"].value(day, EmptyDay());
    if (fresh) {
        auto session = EmptyDay();
        session.erase("sessionCount");
        session["dayKey"] = day;
        session["startedAt"] = event.timestamp;
        session["endedAt"] = event.timestamp;
        sessions.push_back(session);
        if (sessions.size() > 500)
            sessions.erase(sessions.begin());
        Add(record["sessionCount"], 1);
    }
    auto& session = sessions.back();
    if (!fresh) {
        const double delta = event.timestamp - *last_at_;
        for (auto* value : {&session, &record})
            (*value)["activeSeconds"] =
                std::min(double(kMaximumCounter), (*value)["activeSeconds"].get<double>() + delta);
    }
    session["endedAt"] = std::max(session["endedAt"].get<double>(), event.timestamp);
    const char* field = event.kind == Event::Kind::Key      ? "keyCount"
                        : event.kind == Event::Kind::Commit ? "committedCharacterCount"
                                                            : "chordCount";
    const auto amount = event.kind == Event::Kind::Commit ? event.characters : 1;
    Add(session[field], amount);
    Add(record[field], amount);
    file_["days"][day] = record;
    file_["updatedAt"] = event.timestamp;
    last_at_ = event.timestamp;
    return true;
}
void TypingActivity::EndSession() { last_at_.reset(); }
void TypingActivity::Clear(double now) {
    file_ = Empty(true);
    file_["updatedAt"] = now;
    EndSession();
}
} // namespace rimes::metrics
