// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Windows.h>

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include "../core/broker_protocol.hpp"
#include "../core/control.hpp"

namespace rimes::windows::metrics {
using core::Json;
struct Article {
  std::string id, language, title, theme, difficulty, text;
  int version = 1;
};
const std::vector<Article>& Articles();
std::vector<std::wstring> Characters(const std::string& text);
std::string Normalize(std::string text);
std::string Day(double wall);
double Now();
double Wall();
bool TypingKey(unsigned key, unsigned modifiers);

// Same bounded alignment and scoring contract as TypingTestSession.swift.
// The editor's text is transient; Result() deliberately contains aggregates
// only.
class Session {
 public:
  Session(Article article, Json context);
  void Key(double now, bool repeat, bool backspace, bool composing);
  void Chord(double now);
  void UpgradeBackspace();
  void Start(double now);
  bool Reconcile(
      const std::string& text, double now,
      std::optional<std::pair<std::size_t, std::size_t>> insertion = {});
  void Mark(const std::string& reason);
  void Tick(double now);
  std::optional<Json> Finish(double now, double wall);
  Json Snapshot() const;
  bool Complete() const;
  bool Started() const { return start_ >= 0; }
  bool Finished() const { return finished_; }
  const Article& article() const { return article_; }

 private:
  struct Alignment {
    std::vector<int> states;
    std::vector<bool> correct;
    int consumed = 0, matches = 0, substitutions = 0, omissions = 0, extras = 0;
  };
  static Alignment Align(const std::vector<std::wstring>& expected,
                         const std::vector<std::wstring>& typed, bool final);
  void Refresh();
  void Sample(bool force);
  Article article_;
  Json context_, metrics_, reasons_ = Json::array();
  std::vector<std::wstring> expected_, committed_;
  Alignment alignment_;
  double start_ = -1, last_ = -1, interval_ = 1;
  bool finished_ = false;
  std::optional<Json> result_;
};

// One service per Broker. It consumes sanitized observations after the TSF
// privacy gate. No global keyboard hook and no host text reads are involved.
class Service {
 public:
  explicit Service(std::filesystem::path root = {});
  ~Service();
  void Enable(bool statistics, bool practice);
  void ObserveKey(unsigned process, const core::KeyEvent& key, bool composing);
  void ObserveChord(unsigned process);
  void ObserveCommit(unsigned process, unsigned count);
  void LocalKey(unsigned key, unsigned modifiers, bool repeat, DWORD stamp,
                bool composing);
  void Arm(HWND editor, Article article, Json context);
  void Mark(const std::string& reason);
  void Composition();
  bool Reconcile(
      const std::string& text,
      std::optional<std::pair<std::size_t, std::size_t>> insertion = {});
  void Finish(const std::string& reason = {});
  void ResetPractice();
  void ForgetEditor(HWND editor);
  Json Practice();
  Json Data();
  void Preferences(bool keys, bool activity);
  bool Clear(const std::string& kind, const std::string& day = {});
  bool Repair();
  void Save();
  bool Armed();
  std::string Issue();
  // Value-level event admission is exposed for deterministic regression tests.
  void Daily(unsigned key, bool repeat, unsigned modifiers, unsigned commits,
             unsigned chords, double time);

 private:
  bool PracticeFocused(unsigned process) const;
  void PracticeKey(unsigned key, bool repeat, DWORD stamp, bool composing);
  void Activity(const std::string& day, unsigned key, unsigned commits,
                unsigned chords, double time);
  void WriteSnapshot(Json value);
  void Load();
  void FinishLocked(const std::string& reason);
  std::mutex mutex_;
  std::mutex save_mutex_;
  std::uint64_t save_sequence_ = 0, written_sequence_ = 0;
  std::filesystem::path root_;
  Json data_;
  std::string issue_;
  bool statistics_ = true, practice_ = true, dirty_ = false;
  double last_daily_ = -1, saved_at_ = 0;
  HWND editor_ = nullptr;
  HWND practice_editor_ = nullptr;
  DWORD editor_thread_ = 0;
  std::unique_ptr<Session> session_;
  struct Identity {
    unsigned key;
    DWORD stamp;
    bool repeat, composing;
  };
  std::deque<Identity> ledger_;
};
}  // namespace rimes::windows::metrics
