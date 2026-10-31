// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#include "metrics_ui.hpp"

#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <atomic>
#include <thread>
#include <utility>
#include "material_library.hpp"
#include <imm.h>
#include <msctf.h>
#include <richedit.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

#include "../tsf/Guids.h"
#include "../ui/settings_paint.hpp"

namespace rimes::windows::workbench {
using metrics::Json;
namespace {
std::wstring Text(HWND h) {
  std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
  GetWindowTextW(h, s.data(), static_cast<int>(s.size()));
  s.resize(wcslen(s.c_str()));
  return s;
}
std::wstring Number(double n, int digits = 1) {
  std::wostringstream out;
  out << std::fixed << std::setprecision(digits) << n;
  return out.str();
}
std::wstring Percent(const Json& n) {
  return n.is_number() ? Number(n.get<double>() * 100) + L"%" : L"—";
}
std::wstring Duration(double seconds) {
  const int s = static_cast<int>(seconds);
  wchar_t b[40];
  swprintf_s(b, L"%02d:%02d", s / 60, s % 60);
  return b;
}
std::string Guid(const GUID& g) {
  wchar_t text[40]{};
  StringFromGUID2(g, text, 40);
  return Utf8(text);
}
std::string Hash(const std::string& bytes) {
  std::array<unsigned char, 32> digest{};
  BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
             reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
             static_cast<ULONG>(bytes.size()), digest.data(), 32);
  std::string text;
  for (auto b : digest) {
    text += "0123456789abcdef"[b >> 4];
    text += "0123456789abcdef"[b & 15];
  }
  return text;
}
std::pair<std::string, bool> InputSource() {
  ITfInputProcessorProfileMgr* manager = nullptr;
  TF_INPUTPROCESSORPROFILE p{};
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&manager)))) {
    const auto result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &p);
    manager->Release();
    if (SUCCEEDED(result))
      return {Guid(p.clsid) + ":" + Guid(p.guidProfile) + ":" +
                  std::to_string(p.langid) + ":" +
                  std::to_string(reinterpret_cast<UINT_PTR>(p.hkl)),
              IsEqualGUID(p.clsid, tsf::kTextServiceClsid) != 0};
  }
  return {"keyboard:" +
              std::to_string(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(0))),
          false};
}
bool Composing(HWND editor) {
  bool active = false;
  ITfThreadMgr* manager = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&manager)))) {
    ITfDocumentMgr* doc = nullptr;
    ITfContext* context = nullptr;
    ITfContextComposition* compositions = nullptr;
    IEnumITfCompositionView* enumeration = nullptr;
    ITfCompositionView* view = nullptr;
    if (SUCCEEDED(manager->GetFocus(&doc)) && doc &&
        SUCCEEDED(doc->GetTop(&context)) && context &&
        SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&compositions))) &&
        SUCCEEDED(compositions->EnumCompositions(&enumeration)))
      active = enumeration->Next(1, &view, nullptr) == S_OK;
    if (view) view->Release();
    if (enumeration) enumeration->Release();
    if (compositions) compositions->Release();
    if (context) context->Release();
    if (doc) doc->Release();
    manager->Release();
  }
  if (const auto context = ImmGetContext(editor)) {
    active = active ||
             ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
    ImmReleaseContext(editor, context);
  }
  return active;
}
const wchar_t* Kind(const std::string& kind) {
  return kind == "extension" ? L"内置扩展"
         : kind == "capsule" ? L"Capsule 插件"
         : kind == "mailbox" ? L"Mailbox 插件"
                             : L"缓冲插件";
}
std::wstring Description(const std::string& id) {
  if (id == "builtin.typing-speed")
    return L"文章跟打、实时速度与准确率，以及按方案分组的本机成绩。";
  if (id == "builtin.statistics")
    return L"按键分布、输入趋势与历史日历；仅记录本机汇总，不保存正文。";
  if (id == "builtin.fly-chord-learning")
    return L"并击输入、键位配置与实时预览。";
  if (id == "builtin.apple-translation")
    return L"在 Buffer 中实时翻译，支持本地翻译与 AI 连接器。";
  if (id == "builtin.codex-cli")
    return L"通过 Codex CLI 连接 Buffer 的 AI 功能。";
  if (id == "builtin.openai-compatible")
    return L"通过 OpenAI 兼容接口连接 Buffer 的 AI 功能。";
  if (id.ends_with("temporary")) return L"可开启剪贴板历史，收录文字、图片和文件；支持搜索、复制与收藏。";
  if (id.ends_with("capture")) return L"Alt+Shift+4 框选截图、左下角堆栈、图片复制与再次编辑。录屏、OCR、多图拼接尚未实现。";
  if (id.ends_with("notes")) return L"在本机保存、编辑与查找笔记。";
  if (id.ends_with("resources")) return L"保存和打开本机资源路径；文献引用排版尚未实现。";
  if (id.ends_with("passwords")) return L"加密保存、四组物理并击验证、查看与复制；结构化字段和 TOTP 尚未实现。";
  if (id.ends_with("terminal")) return L"管理持续运行的终端会话。";
  if (id.ends_with("chat")) return L"独立 AI 对话与完整消息记录。";
  return L"集中查看收件与消息内容。";
}
}  // namespace

struct ExtensionPages::Impl {
  HWND hwnd = nullptr, parent = nullptr, editor = nullptr, target = nullptr, scrollbar = nullptr;
  ExtensionCallbacks cb;
  std::shared_ptr<metrics::Service> service;
  ui::SettingsPage page = ui::SettingsPage::kInput;
  int sub = 0, scroll = 0, total = 0, width = 650, height = 480, article = 0,
      range = 7, metric = 0;
  unsigned dpi = 96;
  ui::ThemeId theme = ui::ThemeId::kNight;
  ui::SettingsFonts fonts;
  HFONT editor_font = nullptr;
  HBRUSH background = nullptr, surface = nullptr;
  std::map<int, HWND> controls;
  std::map<int, ui::DipRect> positions;
  int track_columns = 20, formatted_columns = 0;
  unsigned formatted_dpi = 0;
  std::wstring track_formatted;
  Json track_correctness = Json::array();
  std::string formatted_article;
  std::vector<rimes::practice::materials::TrackRow> track_rows;
  std::vector<metrics::Article> articles = metrics::Articles();
  std::vector<Json> books;
  bool materials_visible = true, books_tab = false, grid_visible = true, cells_visible = true;
  std::string selected_book;
  int track_left = 12;
  float practice_width = 650, track_top = 268;
  struct MaterialTask {
    std::atomic<bool> done{false};
    std::vector<metrics::Article> articles;
    std::vector<Json> books;
    std::wstring error;
  };
  std::shared_ptr<MaterialTask> material_task;
  std::vector<PluginView> plugins;
  std::vector<std::pair<ui::DipRect, std::size_t>> cards;
  std::vector<std::pair<int, std::wstring>> sections;
  std::vector<Json> results;
  std::vector<std::string> comparisons;
  std::vector<std::pair<ui::DipRect, std::string>> dates;
  std::vector<int> years;
  std::string selected_day = metrics::Day(metrics::Wall()), source, comparison;
  std::uint64_t revision = 0;
  UINT_PTR practice_generation = 0;
  std::wstring status = L"选择文章后开始跟打", message;
  bool suppress = false, visible = false, stopping = false, was_armed = false, ime_composing = false,
       colors_pending = false;
  bool ComposingEditor() const { return ime_composing || (editor && Composing(editor)); }
  void RememberView() {
    service->SetPracticePreferences({{"materialsCollapsed", !materials_visible},
                                    {"gridHidden", !grid_visible}, {"cellsHidden", !cells_visible},
                                    {"lastArticleID", articles.at(article).id}});
  }
  Json last_states;
  Json last_practice;
  Json last_data;
  std::string last_issue;
  std::wstring last_practice_status;
  int Px(double v) const { return static_cast<int>(std::lround(v * dpi / 96)); }
  RECT Rect(ui::DipRect r) const {
    return {Px(r.left), Px(r.top - scroll), Px(r.right), Px(r.bottom - scroll)};
  }
  const ui::ThemePalette& Palette() const { return ui::Palette(theme); }
  explicit Impl(HWND owner, ExtensionCallbacks callbacks)
      : parent(owner), cb(std::move(callbacks)), service(cb.metrics) {
    if (!service) throw std::runtime_error("Extension metrics service missing");
    const auto preferences = service->PracticePreferences();
    materials_visible = !preferences.value("materialsCollapsed", false);
    grid_visible = !preferences.value("gridHidden", false);
    cells_visible = !preferences.value("cellsHidden", false);
    const auto selected = preferences.value("lastArticleID", std::string());
    for (std::size_t i = 0; i < articles.size(); ++i)
      if (articles[i].id == selected) article = static_cast<int>(i);
    LoadLibraryW(L"Msftedit.dll");
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_DATE_CLASSES};
    InitCommonControlsEx(&init);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"Rimes.ExtensionPages";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd =
        CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"内置插件内容",
                        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1, 1,
                        parent, nullptr, wc.hInstance, this);
    // Retain native scrollbar dragging, keyboard control and accessibility,
    // but draw a narrow theme-aware client control instead of the system gutter.
    scrollbar = CreateWindowExW(0, L"SCROLLBAR", L"页面滚动",
        WS_CHILD | WS_TABSTOP | SBS_VERT, 0, 0, 1, 1, hwnd,
        reinterpret_cast<HMENU>(7200), wc.hInstance, nullptr);
    SetWindowSubclass(scrollbar, ScrollbarProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetTimer(hwnd, 1, 200, nullptr);
    LoadMaterials();
  }
  ~Impl() {
    Stop("interrupted");
    service->ForgetEditor(editor);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    fonts.Release();
    if (editor_font) DeleteObject(editor_font);
    if (background) DeleteObject(background);
    if (surface) DeleteObject(surface);
  }
  HWND Make(int id, const wchar_t* cls, const wchar_t* text, DWORD style = 0) {
    if (controls.contains(id)) return controls[id];
    auto h =
        CreateWindowExW(0, cls, text, WS_CHILD | WS_TABSTOP | style, 0, 0, 1, 1,
                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        GetModuleHandleW(nullptr), nullptr);
    controls[id] = h;
    SetWindowSubclass(h, FocusProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.body), FALSE);
    return h;
  }
  HWND Button(int id, const wchar_t* text) {
    const auto h = Make(id, L"BUTTON", text, BS_OWNERDRAW);
    if (Text(h) != text) SetWindowTextW(h, text);
    return h;
  }
  HWND Combo(int id) {
    auto h = Make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
    SetWindowSubclass(h, ComboProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return h;
  }
  void LayoutTrack() {
    if (!editor || ComposingEditor()) return;
    namespace mat = rimes::practice::materials;
    RECT client{}; GetClientRect(editor, &client);
    track_columns = std::max(6, (static_cast<int>(client.right) - Px(24)) / std::max(1, Px(28)));
    track_left = (static_cast<int>(client.right) - track_columns * Px(28)) / 2;
    const auto& a = articles.at(article);
    track_rows = mat::TrackRows(a.display_text.empty() ? a.text : a.display_text, track_columns, a.language == "chinese" ? 2 : 0);
    const auto before = Text(editor);
    const auto chars = mat::Characters(Utf8(before));
    std::vector<std::string> typed;
    for (const auto& c : chars) if (c != "\r" && c != "\n" && c != "\r\n") typed.push_back(c);
    // Extra characters still have a visible input row; never clip accepted text.
    const auto expected_count = metrics::Characters(a.text).size();
    for (auto start = expected_count; start < typed.size(); start += track_columns)
      track_rows.push_back({"", start, std::min<std::size_t>(track_columns, typed.size() - start), 0});
    CHARRANGE saved{}; SendMessageW(editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    auto logical = [&](LONG at) { return metrics::Characters(Utf8(before.substr(0, static_cast<std::size_t>(std::max(0L, at))))).size(); };
    const auto begin = logical(saved.cpMin), end = logical(saved.cpMax);
    std::wstring formatted;
    std::vector<int> indents;
    std::size_t offset = 0, row = 0;
    do {
      const auto count = row < track_rows.size() ? track_rows[row].length : static_cast<std::size_t>(track_columns);
      indents.push_back(row < track_rows.size() ? track_rows[row].indent : 0);
      const auto take = std::min(count, typed.size() - offset);
      for (std::size_t i = 0; i < take; ++i) formatted += Wide(typed[offset + i]);
      offset += take; ++row;
      if (offset < typed.size() || (offset == typed.size() && take == count && row < track_rows.size())) formatted += L'\r';
      else break;
    } while (row < 20000);
    auto physical = [&](std::size_t wanted) {
      std::size_t units = 0, seen = 0;
      for (const auto& c : mat::Characters(Utf8(formatted))) {
        const bool line = c == "\r" || c == "\n" || c == "\r\n";
        if (!line && seen == wanted) break;
        units += Wide(c).size(); if (!line) ++seen;
      }
      return static_cast<LONG>(units);
    };
    std::size_t unchanged = 0;
    if (before == formatted && formatted_columns == track_columns && formatted_dpi == dpi && formatted_article == a.id)
      while (unchanged < track_formatted.size() && unchanged < formatted.size() && track_formatted[unchanged] == formatted[unchanged]) ++unchanged;
    POINT scroll_pos{}; SendMessageW(editor, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));
    suppress = true;
    SendMessageW(editor, WM_SETREDRAW, FALSE, 0);
    if (before != formatted) SetWindowTextW(editor, formatted.c_str());
    const int full_height = 66 + static_cast<int>(track_rows.size()) * 76;
    if (positions.contains(7004)) {
      auto& bounds = positions[7004];
      if (bounds.bottom - bounds.top != full_height) {
        bounds.bottom = bounds.top + full_height;
        total = static_cast<int>(bounds.bottom + 30);
        ShowControls();
        GetClientRect(editor, &client);
      }
    }
    RECT inset{track_left + Px(3), Px(42), client.right - Px(10), client.bottom - Px(14)};
    SendMessageW(editor, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&inset));
    SendMessageW(editor, EM_SETTARGETDEVICE, 0, 1); // Explicit rows; no word-boundary rewrap.
    PARAFORMAT2 paragraph{}; paragraph.cbSize = sizeof(paragraph);
    paragraph.dwMask = PFM_LINESPACING | PFM_STARTINDENT | PFM_OFFSET | PFM_SPACEAFTER;
    paragraph.dySpaceAfter = 0;
    paragraph.dxStartIndent = indents.front() * 28 * 15;
    paragraph.bLineSpacingRule = 4; paragraph.dyLineSpacing = 76 * 15;
    auto dc = GetDC(editor); auto old = SelectObject(dc, editor_font);
    const auto correctness = service->Practice().value("typedCorrectness", Json::array());
    LONG position = 0; std::size_t line_index = 0;
    std::size_t typed_index = 0;
    for (const auto& c : mat::Characters(Utf8(formatted))) {
      const auto text = Wide(c); CHARRANGE span{position, position + static_cast<LONG>(text.size())};
      const bool line_break = c == "\r" || c == "\n" || c == "\r\n";
      const bool same_correctness = typed_index < correctness.size() &&
          typed_index < track_correctness.size() && correctness[typed_index] == track_correctness[typed_index];
      if (span.cpMax <= static_cast<LONG>(unchanged) && (line_break || same_correctness)) {
        if (line_break) ++line_index;
        else ++typed_index;
        position = span.cpMax; continue;
      }
      SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&span));
      paragraph.dxStartIndent = indents[std::min(line_index, indents.size() - 1)] * 28 * 15;
      paragraph.dxOffset = 0;
      SendMessageW(editor, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
      if (c == "\r" || c == "\n" || c == "\r\n") ++line_index;
      else {
        SIZE extent{}; GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
        CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_SPACING | CFM_COLOR | CFM_BACKCOLOR;
        const bool incorrect = typed_index < correctness.size() && !correctness[typed_index].get<bool>();
        format.crTextColor = ui::ToColorRef(incorrect ? Palette().danger_text : Palette().text_primary);
        format.crBackColor = ui::ToColorRef(incorrect ? ui::Blend(Palette().surface, Palette().danger_text, .18f) : Palette().surface);
        format.sSpacing = static_cast<SHORT>(std::clamp(MulDiv(Px(28) - extent.cx, 1440, static_cast<int>(dpi)), -32000, 32000));
        SendMessageW(editor, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        ++typed_index;
      }
      position = span.cpMax;
    }
    SelectObject(dc, old); ReleaseDC(editor, dc);
    CHARRANGE tail{position, position};
    SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
    PARAFORMAT2 trailing{}; trailing.cbSize = sizeof(trailing); trailing.dwMask = PFM_SPACEAFTER;
    trailing.dySpaceAfter = static_cast<LONG>(track_rows.size() > indents.size() ? track_rows.size() - indents.size() : 0) * 76 * 15;
    SendMessageW(editor, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&trailing));
    CHARRANGE restored{physical(begin), physical(end)};
    SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&restored));
    if (restored.cpMin == restored.cpMax) {
      const auto caret_row = static_cast<std::size_t>(std::count(formatted.begin(), formatted.begin() + restored.cpMin, L'\r'));
      paragraph.dxStartIndent = indents[std::min(caret_row, indents.size() - 1)] * 28 * 15;
      paragraph.dwMask = PFM_LINESPACING | PFM_STARTINDENT | PFM_OFFSET;
      SendMessageW(editor, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
      // Preedit is usually ASCII code; give its insertion attributes the same
      // cell advance while leaving the IME free to own the marked range.
      SIZE advance{}; auto insertion_dc = GetDC(editor); auto insertion_font = SelectObject(insertion_dc, editor_font);
      GetTextExtentPoint32W(insertion_dc, L"n", 1, &advance);
      SelectObject(insertion_dc, insertion_font); ReleaseDC(editor, insertion_dc);
      CHARFORMAT2W insertion{}; insertion.cbSize = sizeof(insertion);
      insertion.dwMask = CFM_SPACING | CFM_COLOR | CFM_BACKCOLOR;
      insertion.sSpacing = static_cast<SHORT>(MulDiv(Px(28) - advance.cx, 1440, static_cast<int>(dpi)));
      insertion.crTextColor = ui::ToColorRef(Palette().text_primary); insertion.crBackColor = ui::ToColorRef(Palette().surface);
      SendMessageW(editor, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&insertion));
    }
    track_formatted = formatted; formatted_columns = track_columns; formatted_dpi = dpi; formatted_article = a.id;
    track_correctness = correctness;
    SendMessageW(editor, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));
    SendMessageW(editor, WM_SETREDRAW, TRUE, 0);
    suppress = false; InvalidateRect(editor, nullptr, FALSE);
    RevealCaret();
  }
  void RevealCaret() {
    if (!editor || !positions.contains(7004) || GetFocus() != editor) return;
    CHARRANGE selection{};
    SendMessageW(editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    POINTL caret{};
    SendMessageW(editor, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&caret), selection.cpMax);
    const auto top = positions[7004].top + caret.y * 96.0f / dpi - 40;
    const auto next = static_cast<int>(top + 84 > scroll + height ? top + 84 - height : top < scroll ? top - 8 : scroll);
    if (next != scroll) {
      scroll = std::clamp(next, 0, std::max(0, total - height));
      ShowControls(); InvalidateRect(hwnd, nullptr, FALSE);
    }
  }
  void PaintTrack(HWND edit, HDC dc) {
    if (track_rows.empty()) return;
    RECT client{}; GetClientRect(edit, &client);
    RECT clip{};
    if (GetClipBox(dc, &clip) != ERROR) IntersectRect(&client, &client, &clip);
    POINT scroll_pos{}; SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll_pos));
    auto old = SelectObject(dc, editor_font); SetBkMode(dc, TRANSPARENT);
    auto grid = CreatePen(PS_SOLID, std::max(1, Px(1)), ui::ToColorRef(Palette().border));
    const auto old_pen = SelectObject(dc, grid);
    for (std::size_t row = 0; row < track_rows.size(); ++row) {
      const auto y = Px(10 + static_cast<int>(row) * 76) - scroll_pos.y;
      if (y + Px(60) < client.top || y > client.bottom) continue;
      const auto& source_row = track_rows[row];
      const auto chars = rimes::practice::materials::Characters(source_row.text);
      for (std::size_t i = 0; i < chars.size(); ++i) {
        const auto index = source_row.start + i;
        const int state = last_states.is_array() && index < last_states.size() ? last_states[index].get<int>() : 0;
        SetTextColor(dc, ui::ToColorRef(state == 1 ? Palette().accent_text : state >= 2 ? Palette().danger_text : Palette().text_secondary));
        RECT cell{track_left + Px((source_row.indent + static_cast<int>(i)) * 28), y, track_left + Px((source_row.indent + static_cast<int>(i) + 1) * 28), y + Px(32)};
        if (state) ui::FillRectColor(dc, cell, ui::ToColorRef(ui::Blend(Palette().surface, state == 1 ? Palette().accent : Palette().danger_text, .14f)));
        const auto text = Wide(chars[i]); DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      if (grid_visible) {
        const auto columns = source_row.indent + static_cast<int>(source_row.length);
        for (int col = 0; col <= columns; ++col) {
          const auto x = track_left + Px(col * 28);
          MoveToEx(dc, x, y, nullptr); LineTo(dc, x, y + Px(60));
        }
        for (int offset : {0, 32, 60}) {
          MoveToEx(dc, track_left, y + Px(offset), nullptr);
          LineTo(dc, track_left + Px(columns * 28), y + Px(offset));
        }
      }
    }
    // Native caret/marked text retain TSF ownership; highlight the next committed cell.
    const auto count = service->Practice().value("metrics", Json::object()).value("committed", 0);
    for (std::size_t row = 0; row < track_rows.size(); ++row) {
      const auto& r = track_rows[row];
      if (count < r.start || count >= r.start + r.length) continue;
      const auto column = r.indent + count - static_cast<int>(r.start);
      RECT cell{track_left + Px(column * 28 + 1), Px(43 + static_cast<int>(row) * 76) - scroll_pos.y,
                track_left + Px((column + 1) * 28 - 1), Px(69 + static_cast<int>(row) * 76) - scroll_pos.y};
      auto accent = CreateSolidBrush(ui::ToColorRef(Palette().accent)); FrameRect(dc, &cell, accent); DeleteObject(accent);
      break;
    }
    SelectObject(dc, old_pen); DeleteObject(grid); SelectObject(dc, old);
  }
  void LoadMaterials(const std::filesystem::path& path = {}, const std::string& remove = {}) {
    if (material_task) { status = L"素材正在处理，请稍后…"; return; }
    auto job = material_task = std::make_shared<MaterialTask>();
    if (controls.contains(7007)) EnableWindow(controls[7007], FALSE);
    if (controls.contains(7008)) EnableWindow(controls[7008], FALSE);
    const auto storage = service->MaterialRoot();
    std::thread([job, storage, path, remove] {
      try {
        rimes::practice::materials::Library library(storage);
        if (!path.empty()) library.Import(path);
        if (!remove.empty()) library.Remove(remove);
        job->books = library.Books();
        job->articles = metrics::Articles();
        for (const auto& a : library.Articles()) {
          metrics::Article next{a.at("id").get<std::string>(), a.at("language").get<std::string>(), a.at("title").get<std::string>(), a.at("theme").get<std::string>(),
                                a.at("difficulty").get<std::string>(), a.at("text").get<std::string>(), a.at("version").get<int>()};
          next.display_text = a.value("displayText", next.text);
          job->articles.push_back(std::move(next));
        }
      } catch (const std::exception& error) { job->error = Wide(error.what()); }
      job->done.store(true, std::memory_order_release);
    }).detach();
  }
  void AdoptMaterials() {
    if (!material_task || !material_task->done.load(std::memory_order_acquire) || service->Armed()) return;
    auto job = std::exchange(material_task, {});
    if (controls.contains(7007)) EnableWindow(controls[7007], TRUE);
    if (controls.contains(7008)) EnableWindow(controls[7008], !selected_book.empty());
    if (!job->error.empty()) { status = L"文库：" + job->error; InvalidateRect(hwnd, nullptr, FALSE); return; }
    const bool changed = articles.size() != job->articles.size() || books != job->books ||
        !std::equal(articles.begin(), articles.end(), job->articles.begin(), job->articles.end(), [](const auto& a, const auto& b) {
          return a.id == b.id && a.version == b.version && a.text == b.text && a.display_text == b.display_text;
        });
    if (!changed) return;
    auto selected = articles.at(article).id;
    if (articles.size() == metrics::Articles().size())
      selected = service->PracticePreferences().value("lastArticleID", selected);
    articles = std::move(job->articles); books = std::move(job->books);
    if (!selected_book.empty() && std::none_of(books.begin(), books.end(), [&](const auto& book) {
          return book.value("id", std::string()) == selected_book;
        })) selected_book.clear();
    article = 0;
    for (std::size_t i = 0; i < articles.size(); ++i)
      if (articles[i].id == selected) { article = static_cast<int>(i); break; }
    if (controls.contains(7000)) SendMessageW(controls[7000], CB_RESETCONTENT, 0, 0);
    if (PracticePage()) Build();
  }
  void ImportMaterials() {
    if (service->Armed()) { status = L"请先结束跟打，再导入书籍"; return; }
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = parent;
    dialog.lpstrFilter = L"EPUB 电子书 (*.epub)\0*.epub\0\0";
    dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"导入跟打书籍";
    if (GetOpenFileNameW(&dialog)) { status = L"正在读取 EPUB…"; LoadMaterials(std::filesystem::path(path)); }
  }
  void Materials(float w) {
    Button(7006, materials_visible ? L"收起素材" : L"素材");
    Put(7006, w - 96, 0, 96, 28);
    if (!materials_visible) return;
    Button(7009, books_tab ? L"文章" : L"文章 ✓");
    Button(7012, books_tab ? L"书籍 ✓" : L"书籍");
    Button(7007, L"导入 EPUB…"); Button(7008, L"移除本书…");
    const auto x = w - 224;
    Put(7009, x, 42, 108, 28); Put(7012, x + 116, 42, 108, 28);
    auto list = Make(7014, L"LISTBOX", L"文章和书籍素材", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL);
    SendMessageW(list, LB_SETITEMHEIGHT, 0, Px(64));
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (books_tab && selected_book.empty()) {
      for (std::size_t i = 0; i < books.size(); ++i) {
        const auto n = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Wide(books[i].value("title", std::string("未命名书籍"))).c_str()));
        if (n >= 0) SendMessageW(list, LB_SETITEMDATA, n, -2 - static_cast<LPARAM>(i));
      }
    }
    for (std::size_t i = 0; i < articles.size(); ++i) {
      const bool imported = articles[i].id.rfind("epub.", 0) == 0;
      if (imported != books_tab || (books_tab && (selected_book.empty() || articles[i].id.rfind("epub." + selected_book + ".", 0) != 0))) continue;
      auto n = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Wide(articles[i].title).c_str()));
      if (n >= 0) { SendMessageW(list, LB_SETITEMDATA, n, static_cast<LPARAM>(i));
        if (i == static_cast<std::size_t>(article)) SendMessageW(list, LB_SETCURSEL, n, 0); }
    }
    const float list_top = books_tab && !selected_book.empty() ? 116 : 80;
    if (list_top > 80) { Button(7017, L"‹ 返回书架"); Put(7017, x, 80, 224, 28); }
    Put(7014, x, list_top, 224, std::max(40.0f, height - list_top - 84));
    Put(7007, x, height - 74.0f, 224, 28);
    if (books_tab && !selected_book.empty()) Put(7008, x, height - 38.0f, 224, 28);
    EnableWindow(controls[7007], !material_task && !service->Armed());
    EnableWindow(controls[7008], !material_task && !service->Armed() && !selected_book.empty());
  }
  bool PracticePage() const {
    return page == ui::SettingsPage::kTyping ||
           (page == ui::SettingsPage::kStatistics && sub == 2);
  }
  void Put(int id, float x, float y, float w, float h) {
    positions[id] = {x, y, x + w, y + h};
  }
  void ShowControls() {
    for (auto [id, h] : controls) {
      if (!positions.contains(id)) {
        ShowWindow(h, SW_HIDE);
        continue;
      }
      auto r = Rect(positions[id]);
      if (PracticePage() && materials_visible && (id == 7006 || id == 7007 || id == 7008 || id == 7009 || id == 7012 || id == 7014 || id == 7017))
        OffsetRect(&r, 0, Px(scroll)); // The material column belongs to the viewport.
      SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left,
                   r.bottom - r.top,
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = (std::max)(0, total - 1);
    info.nPage = height;
    info.nPos = scroll;
    SetScrollInfo(scrollbar, SB_CTL, &info, TRUE);
    SetWindowPos(scrollbar, nullptr, Px(width - 12), 0, Px(12), Px(height),
                 SWP_NOACTIVATE | SWP_NOZORDER);
    ShowWindow(scrollbar, total > height ? SW_SHOWNA : SW_HIDE);
    InvalidateRect(scrollbar, nullptr, FALSE);
  }
  static LRESULT CALLBACK ScrollbarProc(HWND h, UINT msg, WPARAM w, LPARAM l,
                                        UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Impl*>(ref);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, ScrollbarProc, id);
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_PRINT) {
      PAINTSTRUCT paint{};
      auto dc = msg == WM_PAINT ? BeginPaint(h, &paint) : reinterpret_cast<HDC>(w);
      RECT r{}; GetClientRect(h, &r);
      const auto& p = self->Palette();
      ui::FillRectColor(dc, r, ui::ToColorRef(p.settings_background));
      SCROLLBARINFO bar{sizeof(bar)};
      if (GetScrollBarInfo(h, OBJID_CLIENT, &bar)) {
        const bool pressed = (bar.rgstate[3] & STATE_SYSTEM_PRESSED) != 0;
        POINT mouse{}; GetCursorPos(&mouse); ScreenToClient(h, &mouse);
        const bool hover = PtInRect(&r, mouse) != FALSE;
        const int thumb_width = self->Px(hover || pressed ? 7 : 5);
        RECT thumb{(r.right - thumb_width) / 2, bar.xyThumbTop,
                   (r.right + thumb_width) / 2, bar.xyThumbBottom};
        if (thumb.bottom > thumb.top)
          ui::FillRoundRect(dc, thumb, self->Px(3), ui::ToColorRef(
              pressed ? p.text_secondary : ui::Blend(p.settings_background, p.text_secondary, hover ? .65f : .45f)));
        // Keep the native arrow hit areas visible; no bevels or white boxes.
        const auto pen = CreatePen(PS_SOLID, (std::max)(1, self->Px(1)), ui::ToColorRef(p.text_muted));
        const auto old = SelectObject(dc, pen);
        for (int direction : {-1, 1}) {
          const int cy = direction < 0 ? bar.dxyLineButton / 2 : r.bottom - bar.dxyLineButton / 2;
          const int cx = r.right / 2, span = self->Px(2);
          MoveToEx(dc, cx - span, cy - direction * span / 2, nullptr);
          LineTo(dc, cx, cy + direction * span / 2);
          LineTo(dc, cx + span, cy - direction * span / 2);
        }
        SelectObject(dc, old); DeleteObject(pen);
      }
      if (msg == WM_PAINT) EndPaint(h, &paint);
      return 0;
    }
    if (msg == WM_MOUSEMOVE) {
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
      TrackMouseEvent(&track);
    }
    const auto result = DefSubclassProc(h, msg, w, l);
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_LBUTTONUP || msg == WM_ENABLE)
      InvalidateRect(h, nullptr, FALSE);
    return result;
  }
  void Reveal(HWND control) {
    const int id = GetDlgCtrlID(control);
    if (!positions.contains(id)) return;
    if (id == 7004) { RevealCaret(); return; }
    if (PracticePage() && materials_visible && (id == 7006 || id == 7007 || id == 7008 || id == 7009 || id == 7012 || id == 7014 || id == 7017)) return;
    const auto r = positions[id];
    const float bottom = (id == 7000 || id == 7010) ? r.top + 30 : r.bottom;
    if (bottom > scroll + height)
      scroll = static_cast<int>(bottom) - height + 12;
    if (r.top < scroll) scroll = static_cast<int>(r.top);
    scroll = (std::clamp)(scroll, 0, (std::max)(0, total - height));
    ShowControls();
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  static LRESULT CALLBACK FocusProc(HWND h, UINT msg, WPARAM w, LPARAM l,
                                    UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Impl*>(ref);
    const auto control = GetDlgCtrlID(h);
    if (control == 7014 && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
      const auto hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
      if (!HIWORD(hit)) {
        SendMessageW(h, LB_SETCURSEL, LOWORD(hit), 0);
        self->Command(7014, LBN_SELCHANGE);
      }
      return 0;
    }
    if (control == 7014 && msg == WM_LBUTTONUP) return 0;
    if (control == 7006 || control == 7015 || control == 7016 || control == 7002 ||
        control == 7009 || control == 7012 || control == 7017) {
      // Display controls do not become the input target. In particular an
      // active composition must survive a click on grid/monitor/collapse.
      if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        SetCapture(h); SendMessageW(h, BM_SETSTATE, TRUE, 0); return 0;
      }
      if (msg == WM_LBUTTONUP && GetCapture() == h) {
        ReleaseCapture(); SendMessageW(h, BM_SETSTATE, FALSE, 0);
        RECT r{}; GetClientRect(h, &r);
        if (PtInRect(&r, {static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))}))
          self->Command(control, BN_CLICKED);
        return 0;
      }
      if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE)
        SendMessageW(h, BM_SETSTATE, FALSE, 0);
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, FocusProc, id);
    if (msg == WM_SETFOCUS) self->Reveal(h);
    return DefSubclassProc(h, msg, w, l);
  }
  void Colors() {
    // Applying SCF_ALL during a TSF edit can replace the IME's marked attributes.
    // Keep the native text store untouched until the composition completes.
    if (ComposingEditor()) { colors_pending = true; return; }
    colors_pending = false;
    fonts.Ensure(dpi);
    if (background) DeleteObject(background);
    if (surface) DeleteObject(surface);
    background =
        CreateSolidBrush(ui::ToColorRef(Palette().settings_background));
    surface = CreateSolidBrush(ui::ToColorRef(Palette().surface));
    if (editor_font) DeleteObject(editor_font);
    editor_font =
        CreateFontW(-Px(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (auto [id, h] : controls)
      SendMessageW(h, WM_SETFONT,
                   reinterpret_cast<WPARAM>(
                       id == 7003 || id == 7004 ? editor_font : fonts.body),
                   FALSE);
    for (auto h : {editor, target})
      if (h) {
        SendMessageW(h, EM_SETBKGNDCOLOR, 0, ui::ToColorRef(Palette().surface));
        CHARFORMAT2W f{};
        f.cbSize = sizeof(f);
        f.dwMask = CFM_COLOR | CFM_UNDERLINE;
        f.crTextColor = ui::ToColorRef(Palette().text_primary);
        SendMessageW(h, EM_SETCHARFORMAT, SCF_ALL,
                     reinterpret_cast<LPARAM>(&f));
      }
    last_states = Json();
    formatted_dpi = 0;
  }
  void Build() {
    positions.clear();
    cards.clear();
    sections.clear();
    dates.clear();
    const float w = static_cast<float>(width - 18);
    if (page == ui::SettingsPage::kPlugins) {
      Gallery();
    } else if (PracticePage()) {
      Materials(w);
      practice_width = w - (materials_visible ? 242.0f : 0.0f);
      const float practice_w = practice_width;
      auto picker = Combo(7000);
      if (SendMessageW(picker, CB_GETCOUNT, 0, 0) == 0)
        for (const auto& a : articles)
          SendMessageW(picker, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(Wide(a.title).c_str()));
      SendMessageW(picker, CB_SETCURSEL, article, 0);
      // The article picker is needed for historical comparisons. In practice
      // the selected title is the heading and selection lives in the material column.
      if (sub) Put(7000, 0, 0, practice_w - 130, 300);
      if (!sub) {
        Button(7001, L"开始跟打");
        Button(7002, L"结束");
        Button(7005, L"随机");
        Button(7015, grid_visible ? L"隐藏格线" : L"显示格线");
        Button(7016, cells_visible ? L"收起监控" : L"实时监控");
        const auto actions_right = practice_w - (materials_visible ? 0 : 104);
        Put(7005, actions_right - 268, 0, 52, 30);
        Put(7001, actions_right - 208, 0, 84, 30);
        Put(7002, actions_right - 116, 0, 48, 30);
        Put(7016, actions_right - 64, 0, 64, 30);
        target = Make(7003, MSFTEDIT_CLASS, L"",
                      ES_MULTILINE | ES_READONLY | WS_VSCROLL);
        editor =
            Make(7004, MSFTEDIT_CLASS, L"",
                 ES_MULTILINE | ES_WANTRETURN | ES_NOOLEDRAGDROP);
        if (!ComposingEditor()) {
          SendMessageW(editor, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
          SendMessageW(editor, EM_EXLIMITTEXT, 0, 16384);
          SendMessageW(editor, EM_SETUNDOLIMIT, 0, 0);
          SendMessageW(editor, EM_SETREADONLY, !service->Armed(), 0);
        }
        EnableWindow(controls[7002], service->Armed());
        DragAcceptFiles(editor, FALSE);
        SetWindowSubclass(editor, EditorProc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        track_top = cells_visible ? 268.0f : 122.0f;
        Put(7015, practice_w - 90, track_top - 32, 90, 26);
        const auto rows = rimes::practice::materials::TrackRows(articles[article].display_text.empty() ? articles[article].text : articles[article].display_text,
            std::max(6, static_cast<int>((practice_w - 24) / 28)), articles[article].language == "chinese" ? 2 : 0);
        Put(7004, 0, track_top, practice_w, 66.0f + static_cast<float>(rows.size()) * 76);
        total = static_cast<int>(positions[7004].bottom + 30);
        suppress = true;
        SetWindowTextW(target, Wide(articles[article].text).c_str());
        suppress = false;
      } else {
        auto contexts = Combo(7010);
        Put(7010, 0, 44, practice_w, 250);
        Button(7011, L"清空成绩…");
        Put(7011, practice_w - 120, 0, 120, 30);
        PopulateHistory(contexts);
        total = 500 + static_cast<int>(results.size()) * 66;
      }
    } else {
      Button(7100, L"7 天");
      Button(7101, L"30 天");
      Button(7102, L"成文字数");
      Button(7103, L"日常速度");
      Put(7100, 0, 0, 65, 28);
      Put(7101, 70, 0, 65, 28);
      Put(7102, 150, 0, 100, 28);
      Put(7103, 255, 0, 100, 28);
      auto date =
          Make(7104, DATETIMEPICK_CLASSW, L"选择统计日期", DTS_SHORTDATEFORMAT);
      SetWindowSubclass(date, ComboProc, 1, reinterpret_cast<DWORD_PTR>(this));
      SendMessageW(date, DTM_SETFORMATW, 0,
                   reinterpret_cast<LPARAM>(L"yyyy-MM-dd"));
      Put(7104, w - 170, 0, 170, 28);
      SYSTEMTIME limits[2]{};
      GetLocalTime(&limits[1]);
      SendMessageW(date, DTM_SETRANGE, GDTR_MAX,
                   reinterpret_cast<LPARAM>(limits));

      Button(7110, L"按键分布");
      Button(7111, L"输入趋势");
      Button(7112, L"清空输入趋势…");
      Button(7113, sub ? L"清空键频历史…" : L"清空当天键频…");
      years.clear();
      const auto data = service->Data();
      std::set<int, std::greater<int>> recordedYears;
      recordedYears.insert(
          std::stoi(metrics::Day(metrics::Wall()).substr(0, 4)));
      for (const auto& [day, value] : data["days"].items()) {
        (void)value;
        recordedYears.insert(std::stoi(day.substr(0, 4)));
      }
      years.assign(recordedYears.begin(), recordedYears.end());
      const float bottom =
          sub ? 1080.0f + static_cast<float>(years.size() - 1) * 150 : 650.0f;
      Put(7110, 0, bottom, 110, 30);
      Put(7111, 118, bottom, 110, 30);
      Put(7112, w - 320, bottom, 150, 30);
      Put(7113, w - 160, bottom, 160, 30);
      total = static_cast<int>(bottom + 115);
    }
    Button(7200, L"备份并重建…");
    if (!service->Issue().empty()) {
      Put(7200, 0, static_cast<float>(total), 150, 30);
      total += 85;
    }
    scroll = (std::clamp)(scroll, 0, (std::max)(0, total - height));
    ShowControls();
    Colors();
    if (PracticePage() && !sub) LayoutTrack();
    last_practice = Json();
    last_practice_status.clear();
    last_data = service->Data();
    last_issue = service->Issue();
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  void PopulateHistory(HWND contexts) {
    const auto data = service->Data();
    const auto& a = articles[article];
    auto old = comparison;
    comparisons.clear();
    SendMessageW(contexts, CB_RESETCONTENT, 0, 0);
    for (const auto& r : data["results"])
      if (r["articleID"] == a.id) {
        const auto key = r["context"].value("comparison", "");
        if (std::find(comparisons.begin(), comparisons.end(), key) ==
            comparisons.end()) {
          comparisons.push_back(key);
          const auto title = Wide(r["context"].value("schema", "未知输入源")) +
                             L" · 文章 v" +
                             std::to_wstring(r.value("articleVersion", 1)) +
                             L" · " + Wide(key.substr(0, 8));
          SendMessageW(contexts, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(title.c_str()));
        }
      }
    const auto found = std::find(comparisons.begin(), comparisons.end(), old);
    auto index = found == comparisons.end()
                     ? 0
                     : static_cast<int>(found - comparisons.begin());
    if (comparisons.empty()) {
      SendMessageW(contexts, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"暂无输入方案记录"));
      comparison.clear();
    } else
      comparison = comparisons[index];
    SendMessageW(contexts, CB_SETCURSEL, index, 0);
    results.clear();
    for (auto it = data["results"].rbegin(); it != data["results"].rend(); ++it)
      if ((*it)["articleID"] == a.id &&
          (*it)["context"].value("comparison", "") == comparison)
        results.push_back(*it);
    for (std::size_t i = 0; i < results.size(); ++i) {
      Button(7300 + static_cast<int>(i), L"详情");
      Put(7300 + static_cast<int>(i), practice_width - 76,
          440 + static_cast<float>(i) * 66, 70, 28);
    }
  }
  void Gallery() {
    plugins = cb.plugins ? cb.plugins() : std::vector<PluginView>{};
    float y = 0;
    int index = 0;
    const float gap = 10, w = static_cast<float>(width - 18),
                card = (w - 2 * gap) / 3;
    constexpr const char* order[] = {"mailbox", "capsule", "extension",
                                     "buffer"};
    constexpr const char* filters[] = {"", "buffer", "capsule", "mailbox",
                                       "extension"};
    for (auto kind : order) {
      if (sub > 0 && sub < 5 && std::string(kind) != filters[sub]) continue;
      std::vector<std::size_t> selected;
      for (std::size_t i = 0; i < plugins.size(); ++i)
        if (plugins[i].kind == kind) selected.push_back(i);
      if (selected.empty()) continue;
      sections.push_back({static_cast<int>(y), Kind(kind)});
      y += 30;
      for (auto i : selected) {
        const auto& p = plugins[i];
        const int col = index % 3;
        const float x = col * (card + gap);
        cards.push_back({{x, y, x + card, y + 128}, i});
        const int id = 8000 + static_cast<int>(i) * 4;
        Button(id, p.enabled ? L"停用" : p.installed ? L"启用" : p.can_install ? L"安装" : L"未实现");
        EnableWindow(controls[id], p.installed || p.can_install);
        Button(id + 1, L"详情");
        Button(id + 2, L"设置");
        EnableWindow(controls[id + 2], p.installed && p.enabled);
        Button(id + 3, L"卸载…");
        Put(id, x + card - 63, y + 94, 54, 26);
        Put(id + 1, x + card - 104, y + 9, 42, 23);
        Put(id + 2, x + card - 55, y + 9, 46, 23);
        if (p.installed && p.can_install) Put(id + 3, x + 9, y + 94, 62, 26);
        if (++index % 3 == 0) y += 138;
      }
      if (index % 3) y += 138;
      index = 0;
      y += 20;
    }
    total = static_cast<int>(y + 60);
  }
  void Start() {
    Stop("interrupted");
    ++practice_generation;
    suppress = true;
    SetWindowTextW(editor, L"");
    suppress = false;
    const auto config = cb.configuration ? cb.configuration() : Settings{};
    auto input = InputSource();
    source = input.first;
    revision = config.revision;
    Json context = {
        {"inputSource", source},
        {"schema", input.second ? config.schema : source},
        {"chordAvailable",
         input.second && config.schema == "my_combo" && config.chord_enabled}};
    context["keymapRevision"] =
        context["chordAvailable"] == true
            ? Hash(config.chord_profile.dump() + ":" +
                   std::to_string(config.chord_interval_ms))
            : "";
    context["comparison"] =
        Hash(context.dump() + ":" +
             std::to_string(articles[article].version));
    service->Arm(editor, articles[article], context);
    ime_composing = false;
    SendMessageW(editor, EM_SETREADONLY, FALSE, 0);
    SetFocus(editor);
    LayoutTrack();
    was_armed = true;
    status = service->Practice()["context"].value("repeatArticle", false)
                 ? L"同篇重练 · 从第一个字开始"
                 : L"首次挑战 · 从第一个字开始";
    SetWindowTextW(controls[7001], L"重新开始");
    EnableWindow(controls[7002], TRUE);
    EnableWindow(controls[7000], FALSE);
    EnableWindow(controls[7005], FALSE);
    if (controls.contains(7007)) EnableWindow(controls[7007], FALSE);
    if (controls.contains(7008)) EnableWindow(controls[7008], FALSE);
    last_states = Json();
  }
  void Stop(const std::string& reason) {
    if (stopping || !service->Armed()) return;
    stopping = true;
    service->Finish(reason);
    if (editor) SendMessageW(editor, EM_SETREADONLY, TRUE, 0);
    status = reason.empty()
                 ? L"本次结束 · 成绩已保存"
                 : L"本次记为练习 · " +
                       Wide(reason == "focusLost"              ? "输入区失焦"
                            : reason == "configurationChanged" ? "方案已改变"
                            : reason == "inputSourceChanged"   ? "输入源已改变"
                                                               : "练习中断");
    stopping = false;
  }
  void ResetArticle() {
    Stop("interrupted");
    service->ResetPractice();
    was_armed = false;
    last_states = Json();
    status = L"选择文章后开始跟打";
    suppress = true;
    if (editor) SetWindowTextW(editor, L"");
    suppress = false;
    if (controls.contains(7001)) SetWindowTextW(controls[7001], L"开始跟打");
    if (controls.contains(7000)) EnableWindow(controls[7000], TRUE);
    if (controls.contains(7005)) EnableWindow(controls[7005], TRUE);
  }
  void Publish() {
    if (suppress || !editor || !service->Armed()) return;
    if (ComposingEditor()) {
      service->Composition();
      return;
    }
    if (colors_pending) Colors();
    const auto text = Text(editor);
    // A surrogate pair may arrive in two WM_CHAR messages. Wait for the
    // complete UTF-16 value instead of admitting a replacement character.
    if (!text.empty() &&
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), nullptr, 0, nullptr,
                             nullptr))
      return;
    service->Reconcile(Utf8(text));
    LayoutTrack();
    Refresh();
  }
  void Refresh() {
    if (!visible) return;
    if (colors_pending && !ComposingEditor()) {
      Colors();
      if (PracticePage() && !sub) LayoutTrack();
    }
    AdoptMaterials();
    if (page == ui::SettingsPage::kPlugins && cb.plugins) {
      auto current = cb.plugins();
      bool changed = current.size() != plugins.size();
      for (std::size_t i = 0; !changed && i < current.size(); ++i)
        changed = current[i].id != plugins[i].id ||
                  current[i].name != plugins[i].name ||
                  current[i].version != plugins[i].version ||
                  current[i].kind != plugins[i].kind ||
                  current[i].bundled != plugins[i].bundled ||
                  current[i].can_install != plugins[i].can_install ||
                  current[i].installed != plugins[i].installed ||
                  current[i].enabled != plugins[i].enabled;
      if (changed) {
        Build();
        PostMessageW(parent, WM_APP + 73, 0, 0);
      }
      return;
    }
    if (page == ui::SettingsPage::kTyping && !sub) {
      const bool armed = service->Armed();
      if (armed) {
        if (InputSource().first != source) {
          Stop("inputSourceChanged");
        } else if (cb.configuration && cb.configuration().revision != revision)
          Stop("configurationChanged");
      }
      const auto snapshot = service->Practice();
      if (!snapshot.is_null() && snapshot.contains("states") &&
          snapshot["states"] != last_states) {
        last_states = snapshot["states"];
        Highlight();
      }
      if (was_armed && !service->Armed()) {
        SendMessageW(editor, EM_SETREADONLY, TRUE, 0);
        SetWindowTextW(controls[7001], L"同篇重练");
        EnableWindow(controls[7002], FALSE);
        EnableWindow(controls[7000], TRUE);
        EnableWindow(controls[7005], TRUE);
        if (controls.contains(7007)) EnableWindow(controls[7007], !material_task);
        if (controls.contains(7008)) EnableWindow(controls[7008], !material_task && !selected_book.empty());
        if (snapshot.value("finished", false))
          status =
              snapshot.value("complete", false) && snapshot["reasons"].empty()
                  ? L"本次完成 · 成绩已保存"
                  : L"本次结束 · 已保存为练习";
        else
          status = L"尚未输入，不产生空成绩";
        if (!service->Issue().empty())
          status = L"成绩未能保存 · " + Wide(service->Issue());
        was_armed = false;
      }
      // A resting practice page has no animation. Timer ticks still check
      // input-source/configuration changes, but only changed metrics repaint.
      if (snapshot != last_practice || status != last_practice_status) {
        last_practice = snapshot;
        last_practice_status = status;
        // Keep the article and RichEdit input out of periodic metric updates.
        // Scrolling/layout changes still invalidate the entire page separately.
        for (const auto& area : {ui::DipRect{0, 48, practice_width, track_top - 37}}) {
          const auto rect = Rect(area);
          InvalidateRect(hwnd, &rect, FALSE);
        }
      }
      return;
    }
    // Polling is not animation. An unchanged history/gallery/statistics page
    // must not repaint its native children at 5 Hz through WS_EX_COMPOSITED.
    const auto data = service->Data();
    const auto issue = service->Issue();
    if (data != last_data || issue != last_issue) {
      const bool history = PracticePage();
      if (issue != last_issue ||
          (history && data["results"] != last_data["results"]) ||
          (!history && (data["recordKeys"] != last_data["recordKeys"] ||
                        data["recordActivity"] != last_data["recordActivity"]))) {
        Build(); // History rows and recovery controls may have changed.
      } else {
        last_data = data;
        last_issue = issue;
        // Daily typing does not change practice history. Keep its comparison
        // dropdown and native controls intact while unrelated stats arrive.
        if (!history) InvalidateRect(hwnd, nullptr, FALSE);
      }
    }
  }
  void Highlight() {
    if (editor) InvalidateRect(editor, nullptr, FALSE);
    if (!target || !last_states.is_array()) return;
    const auto& articleValue = articles[article];
    const auto chars = metrics::Characters(articleValue.text);
    SendMessageW(target, WM_SETREDRAW, FALSE, 0);
    LONG offset = 0;
    for (std::size_t i = 0; i < chars.size(); ++i) {
      CHARRANGE selection{offset, offset + static_cast<LONG>(chars[i].size())};
      SendMessageW(target, EM_EXSETSEL, 0,
                   reinterpret_cast<LPARAM>(&selection));
      CHARFORMAT2W f{};
      f.cbSize = sizeof(f);
      f.dwMask = CFM_COLOR | CFM_UNDERLINE;
      const int state = i < last_states.size() ? last_states[i].get<int>() : 0;
      f.crTextColor = ui::ToColorRef(state == 1   ? Palette().accent_text
                                     : state >= 2 ? Palette().danger_text
                                                  : Palette().text_secondary);
      f.dwEffects = state == 3 ? CFE_UNDERLINE : 0;
      SendMessageW(target, EM_SETCHARFORMAT, SCF_SELECTION,
                   reinterpret_cast<LPARAM>(&f));
      offset = selection.cpMax;
    }
    CHARRANGE end{0, 0};
    SendMessageW(target, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(target, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(target, nullptr, FALSE);
  }
  void Label(HDC dc, const std::wstring& text, ui::DipRect rect, HFONT font,
             std::uint32_t color,
             UINT flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) {
    auto r = Rect(rect);
    SelectObject(dc, font);
    SetTextColor(dc, ui::ToColorRef(color));
    DrawTextW(dc, text.c_str(), -1, &r, flags);
  }
  void Box(HDC dc, ui::DipRect rect) {
    ui::FillRoundRect(dc, Rect(rect), Px(8),
                      ui::ToColorRef(Palette().surface_secondary));
    ui::StrokeRoundRect(dc, Rect(rect), Px(8),
                        ui::ToColorRef(Palette().border));
  }
  void MetricCards(HDC dc, float y, const std::array<std::wstring, 4>& titles,
                   const std::array<std::wstring, 4>& values) {
    const float w = ((PracticePage() ? practice_width : static_cast<float>(width - 18)) - 30) / 4.0f;
    for (int i = 0; i < 4; ++i) {
      const float x = i * (w + 10);
      Box(dc, {x, y, x + w, y + 72});
      Label(dc, titles[i], {x + 12, y + 10, x + w - 8, y + 28}, fonts.family,
            Palette().text_secondary);
      Label(dc, values[i], {x + 12, y + 34, x + w - 8, y + 64}, fonts.heading,
            Palette().text_primary, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  }
  void Chart(HDC dc, float y, float h,
             const std::vector<std::optional<double>>& values,
             const std::wstring& title, float left = 0, float chart_width = 0) {
    const float w = chart_width > 0 ? chart_width : PracticePage() ? practice_width : static_cast<float>(width - 18);
    Box(dc, {left, y, left + w, y + h});
    Label(dc, title, {left + 14, y + 10, left + w - 14, y + 32}, fonts.body,
          Palette().text_secondary);
    double max = 1;
    for (const auto& n : values)
      if (n) max = (std::max)(max, *n);
    bool any = false;
    auto pen = CreatePen(PS_SOLID, Px(2), ui::ToColorRef(Palette().accent));
    auto old = SelectObject(dc, pen);
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (!values[i]) {
        any = false;
        continue;
      }
      const auto x = Px(left + 18 + (w - 36) * (values.size() > 1
                                             ? static_cast<float>(i) /
                                                   (values.size() - 1)
                                             : 0.5f)),
                 v = Px(y + h - 26 -
                        static_cast<float>(*values[i] / max) * (h - 70) -
                        scroll);
      if (any)
        LineTo(dc, x, v);
      else
        MoveToEx(dc, x, v, nullptr);
      Ellipse(dc, x - Px(2), v - Px(2), x + Px(2), v + Px(2));
      any = true;
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    if (values.empty() ||
        std::none_of(values.begin(), values.end(),
                     [](const auto& v) { return v.has_value(); }))
      Label(dc, L"开始输入后显示速度轨迹", {left + 8, y + 50, left + w - 8, y + h - 12}, fonts.family,
            Palette().text_muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Label(dc, L"0", {left + 10, y + h - 22, left + 50, y + h - 4}, fonts.family,
          Palette().text_muted);
    Label(dc, Number(max, 0), {left + w - 65, y + 12, left + w - 12, y + 32}, fonts.family,
          Palette().text_muted, DT_RIGHT | DT_SINGLELINE);
  }
  void Keyboard(HDC dc, float y, const Json& counts) {
    const std::vector<std::vector<unsigned>> keys = {
        {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', VK_OEM_MINUS,
         VK_OEM_PLUS, VK_BACK},
        {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', VK_OEM_4, VK_OEM_6,
         VK_OEM_5},
        {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7,
         VK_RETURN},
        {'Z', 'X', 'C', 'V', 'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD,
         VK_OEM_2},
        {VK_SHIFT, VK_CONTROL, VK_MENU, VK_SPACE}};
    unsigned maximum = 1, totalKeys = 0;
    std::string top;
    for (const auto& [key, count] : counts.items())
      if (count.is_number_unsigned() || count.is_number_integer()) {
        const auto value = count.get<unsigned>();
        totalKeys += value;
        if (value >= maximum) {
          maximum = value;
          top = key;
        }
      }
    Label(dc, L"键盘分布 · " + std::to_wstring(totalKeys) + L" 次",
          {0, y, static_cast<float>(width - 18), y + 24}, fonts.body,
          Palette().text_primary);
    const float cell = (width - 18 - 12 * 4) / 13.0f;
    for (std::size_t row = 0; row < keys.size(); ++row) {
      float x = static_cast<float>(row == 2   ? cell * .3
                                   : row == 3 ? cell * .8
                                              : 0);
      for (auto key : keys[row]) {
        const float size = key == VK_SPACE ? cell * 6 : cell;
        const auto n = counts.value(std::to_string(key), 0U);
        ui::DipRect box{x, y + 32 + row * 38, x + size, y + 66 + row * 38};
        ui::FillRoundRect(dc, Rect(box), Px(5),
                          ui::ToColorRef(ui::Blend(
                              Palette().surface_secondary, Palette().accent,
                              n ? 0.18f + 0.65f * n / maximum : 0.0f)));
        std::wstring name = key >= L'0' && key <= L'Z'
                                ? std::wstring(1, static_cast<wchar_t>(key))
                            : key == VK_BACK       ? L"⌫"
                            : key == VK_RETURN     ? L"↵"
                            : key == VK_SPACE      ? L"Space"
                            : key == VK_SHIFT      ? L"⇧"
                            : key == VK_CONTROL    ? L"Ctrl"
                            : key == VK_MENU       ? L"Alt"
                            : key == VK_OEM_COMMA  ? L","
                            : key == VK_OEM_PERIOD ? L"."
                            : key == VK_OEM_2      ? L"/"
                            : key == VK_OEM_1      ? L";"
                            : key == VK_OEM_7      ? L"'"
                            : key == VK_OEM_4      ? L"["
                            : key == VK_OEM_6      ? L"]"
                            : key == VK_OEM_5      ? L"\\"
                            : key == VK_OEM_MINUS  ? L"-"
                                                   : L"=";
        Label(dc, name, {box.left, box.top + 3, box.right, box.bottom - 13},
              fonts.family, Palette().text_primary, DT_CENTER | DT_SINGLELINE);
        Label(dc, n ? std::to_wstring(n) : L"",
              {box.left, box.bottom - 14, box.right, box.bottom}, fonts.family,
              Palette().text_muted, DT_CENTER | DT_SINGLELINE);
        x += size + 4;
      }
    }
  }
  std::wstring Detail(const Json& m) {
    return L"有效速度 " + Number(m.value("effectiveCPM", 0.0)) +
           L" CPM  · 原始速度 " + Number(m.value("rawCPM", 0.0)) +
           L" CPM\n过程正确率 " + Percent(m.value("processAccuracy", Json())) +
           L" · 最终正确率 " + Percent(m.value("finalAccuracy", Json())) +
           L"\n错 " + std::to_wstring(m.value("substitutions", 0)) + L" · 漏 " +
           std::to_wstring(m.value("omissions", 0)) + L" · 多 " +
           std::to_wstring(m.value("extras", 0)) + L" · 修正 " +
           std::to_wstring(m.value("corrections", 0)) + L" 次" +
           L"\n组字退格 " +
           std::to_wstring(m.value("compositionBackspaces", 0)) +
           L" · 上屏退格 " +
           std::to_wstring(m.value("committedBackspaces", 0)) +
           L" · 删除字符 " + std::to_wstring(m.value("deleted", 0)) +
           L"\n并击 " +
           (m.value("chords", Json()).is_null()
                ? L"—"
                : std::to_wstring(m["chords"].get<unsigned>())) +
           L" · 码长 " +
           (m.value("committed", 0) > 0
                ? Number(m.value("keys", 0.0) / m["committed"].get<double>())
                : L"—") +
           L" 键/字 · 重复键 " + std::to_wstring(m.value("repeats", 0));
  }
  void Paint(HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    const auto frame = CreateCompatibleDC(dc);
    const auto bitmap = frame ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
    if (!bitmap) {
      if (frame) DeleteDC(frame);
      PaintContent(dc);
      return;
    }
    const auto previous = SelectObject(frame, bitmap);
    // Publish background, cards and glyphs together; suppressing erase alone
    // cannot hide the intermediate background from a direct GDI paint.
    PaintContent(frame);
    BitBlt(dc, 0, 0, client.right, client.bottom, frame, 0, 0, SRCCOPY);
    SelectObject(frame, previous);
    DeleteObject(bitmap);
    DeleteDC(frame);
  }
  void PaintContent(HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, background);
    SetBkMode(dc, TRANSPARENT);
    const auto& p = Palette();
    const float w = PracticePage() ? practice_width : static_cast<float>(width - 18);
    if (page == ui::SettingsPage::kPlugins) {
      for (const auto& [y, name] : sections)
        Label(dc, name,
              {0, static_cast<float>(y), w, static_cast<float>(y + 24)},
              fonts.title, p.text_secondary);
      for (const auto& [box, i] : cards) {
        const auto& item = plugins[i];
        Box(dc, box);
        const auto x = box.left, y = box.top;
        ui::DrawIconGlyph(dc,
                          item.kind == "extension" ? ui::IconId::kKeyboard
                          : item.kind == "buffer"  ? ui::IconId::kLink
                                                   : ui::IconId::kGrid,
                          Rect({x + 12, y + 12, x + 36, y + 36}),
                          ui::ToColorRef(p.accent));
        Label(dc,
              Wide(item.id == "builtin.apple-translation" ? "翻译" : item.name),
              {x + 12, y + 45, box.right - 10, y + 66}, fonts.title,
              p.text_primary, DT_SINGLELINE | DT_END_ELLIPSIS);
        Label(dc, L"出品方 · RIMES 官方",
              {x + 12, y + 70, box.right - 10, y + 87}, fonts.family,
              p.text_muted, DT_SINGLELINE);
        if (!(item.installed && item.can_install))
          Label(dc,
                item.installed ? (item.enabled ? L"已启用" : L"已停用")
                               : L"未安装",
                {x + 12, y + 98, box.right - 70, y + 118}, fonts.family,
                p.text_muted, DT_SINGLELINE);
      }
      Label(
          dc, message,
          {0, static_cast<float>(total - 50), w, static_cast<float>(total - 5)},
          fonts.body, p.text_secondary);
    } else if (PracticePage()) {
      const auto& a = articles[article];
      if (!sub) {
        const auto snapshot = service->Practice();
        const auto m = snapshot.value("metrics", Json::object());
        const bool english = a.language == "english";
        const auto w = practice_width;
        const auto heading_right = std::max(90.0f, w - (materials_visible ? 278 : 382));
        Label(dc, Wide(a.title), {0, 0, heading_right, 25}, fonts.title,
              p.text_primary, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        Label(dc, Wide(a.theme + " · " + a.difficulty) + L" · " +
                  std::to_wstring(metrics::Characters(a.text).size()) + L" 字符",
              {0, 30, w, 47}, fonts.family, p.text_muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        if (cells_visible) {
          const std::array<std::wstring, 4> titles{L"有效速度", L"击键", L"回退", L"正确率"};
          const std::array<std::wstring, 4> values{
              m.empty() ? L"—" : Number(m.value("effectiveCPM", 0.0) / (english ? 5 : 1)) + (english ? L" WPM" : L" 字/分"),
              std::to_wstring(m.value("keys", 0)) + L" 键",
              std::to_wstring(m.value("backspaces", 0)), Percent(m.value("processAccuracy", Json()))};
          const auto elapsed = m.value("elapsed", 0.0);
          const auto committed = m.value("committed", 0.0);
          const auto chord = m.value("chords", Json());
          const std::array<std::wstring, 4> details{
              L"原始 " + Number(m.value("rawCPM", 0.0) / (english ? 5 : 1)) + (english ? L" WPM" : L" 字/分"),
              Number(elapsed > 0 ? m.value("keys", 0.0) / elapsed : 0.0) + L" 键/秒 · " +
                  (chord.is_number() ? std::to_wstring(chord.get<int>()) + L" 拍" : L"拍数 —") + L" · 码长 " +
                  (committed > 0 ? Number(m.value("keys", 0.0) / committed) : L"—"),
              L"组字 " + std::to_wstring(m.value("compositionBackspaces", 0)) + L" · 成文 " + std::to_wstring(m.value("committedBackspaces", 0)),
              L"最终 " + Percent(m.value("finalAccuracy", Json()))};
          const float tile_width = (w / 2 - 12) / 2;
          for (int i = 0; i < 4; ++i) {
            const auto x = (i % 2) * (tile_width + 8), y = 52.0f + (i / 2) * 72;
            Box(dc, {x, y, x + tile_width, y + 64});
            Label(dc, titles[i], {x + 10, y + 5, x + tile_width - 8, y + 20}, fonts.family, p.text_secondary);
            Label(dc, values[i], {x + 10, y + 22, x + tile_width - 8, y + 42}, fonts.body, p.text_primary, DT_SINGLELINE | DT_END_ELLIPSIS);
            Label(dc, details[i], {x + 10, y + 44, x + tile_width - 8, y + 60}, fonts.family, p.text_muted, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
          }
          std::vector<std::optional<double>> samples;
          for (const auto& s : m.value("samples", Json::array())) samples.push_back(s[1].get<double>() / (english ? 5 : 1));
          Chart(dc, 52, 136, samples, L"速度轨迹", w / 2 + 4, w / 2 - 4);
        }
        const float status_y = track_top - 66;
        Label(dc, status, {0, status_y, w - 180, status_y + 24}, fonts.family, p.text_secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
        const auto progress = m.value("expected", 0) > 0 ? std::clamp(
            static_cast<float>((m.value("correct", 0.0) + m.value("substitutions", 0.0) + m.value("omissions", 0.0)) / m["expected"].get<double>()), 0.0f, 1.0f) : 0;
        ui::FillRoundRect(dc, Rect({w - 174, status_y + 8, w - 94, status_y + 13}), Px(3), ui::ToColorRef(p.border));
        if (progress > 0) ui::FillRoundRect(dc, Rect({w - 174, status_y + 8, w - 174 + 80 * progress, status_y + 13}), Px(3), ui::ToColorRef(p.accent));
        Label(dc, Number(progress * 100, 0) + L"%  " + Duration(m.value("elapsed", 0.0)),
              {w - 88, status_y, w, status_y + 24}, fonts.family, p.text_secondary, DT_SINGLELINE | DT_RIGHT);
        Box(dc, {0, track_top - 36, w, positions.at(7004).bottom + 8});
        Label(dc, L"原文在上 · 输入在下 · 组字不判分", {12, track_top - 27, w - 100, track_top - 6}, fonts.family, p.text_muted, DT_SINGLELINE | DT_END_ELLIPSIS);
      } else {
        const auto count = results.size();
        std::vector<std::optional<double>> curve;
        double best = 0, totalSpeed = 0;
        unsigned comparable = 0;
        for (auto i = results.rbegin(); i != results.rend(); ++i) {
          const bool valid =
              (*i)["complete"] == true && (*i)["reasons"].empty();
          const auto speed = (*i)["metrics"].value("effectiveCPM", 0.0) /
                             (a.language == "english" ? 5 : 1);
          curve.push_back(valid ? std::optional<double>(speed) : std::nullopt);
          if (valid) {
            best = (std::max)(best, speed);
            totalSpeed += speed;
            ++comparable;
          }
        }
        MetricCards(dc, 94,
                    {L"最近成绩", L"有效成绩", L"最佳速度", L"平均速度"},
                    {std::to_wstring(count), std::to_wstring(comparable),
                     comparable ? Number(best) : L"—",
                     comparable ? Number(totalSpeed / comparable) : L"—"});
        Chart(dc, 190, 180, curve, L"相同文章、版本与输入方案的成绩");
        Label(dc,
              count > 0 ? L"最近成绩 · 点击详情查看完整指标"
                        : L"等待你的第一次成绩",
              {0, 394, w, 420}, fonts.body, p.text_secondary);
        for (std::size_t i = 0; i < count; ++i) {
          float y = 430 + static_cast<float>(i) * 66;
          Box(dc, {0, y, w, y + 56});
          const auto& r = results[i];
          Label(dc,
                Wide(metrics::Day(r["completedAt"].get<double>())) + L" · " +
                    (r["context"].value("repeatArticle", false) ? L"重练"
                                                                : L"首次") +
                    (r["reasons"].empty() ? L"" : L" · 非正式练习"),
                {12, y + 6, w - 100, y + 27}, fonts.family, p.text_secondary);
          Label(dc,
                Number(r["metrics"].value("effectiveCPM", 0.0) /
                       (a.language == "english" ? 5 : 1)) +
                    (a.language == "english" ? L" WPM" : L" 字/分") + L" · " +
                    Percent(r["metrics"].value("processAccuracy", Json())) +
                    L" · " + Duration(r["metrics"].value("elapsed", 0.0)),
                {12, y + 30, w - 100, y + 51}, fonts.body, p.text_primary);
        }
      }
    } else {
      const auto data = service->Data();
      const auto empty = Json{{"keys", Json::object()},
                              {"keyCount", 0},
                              {"characters", 0},
                              {"active", 0.0}};
      auto day = data["days"].value(selected_day, empty);
      const auto active = day.value("active", 0.0);
      auto cardValues = std::array<std::wstring, 4>{
          std::to_wstring(day.value("characters", 0)),
          std::to_wstring(day.value("keyCount", 0)), Duration(active),
          active > 0 ? Number(day.value("characters", 0) * 60 / active) : L"—"};
      if (!sub) {
        Label(dc, Wide(selected_day) + L" · 本机输入汇总", {0, 45, w, 70},
              fonts.body, p.text_secondary);
        MetricCards(dc, 82,
                    {L"成文字数", L"输入按键", L"活跃时长", L"日常速度"},
                    cardValues);
      }
      std::vector<std::optional<double>> trend;
      const auto base = SelectedTime();
      for (int i = range - 1; i >= 0; --i) {
        const auto key = metrics::Day(base - i * 86400);
        const auto d = data["days"].value(key, empty);
        const double sec = d.value("active", 0.0);
        trend.push_back(
            metric ? (sec > 0 ? std::optional<double>(d.value("characters", 0) *
                                                      60 / sec)
                              : std::nullopt)
                   : std::optional<double>(d.value("characters", 0)));
      }
      Chart(dc, sub ? 48.0f : 174.0f, 165, trend,
            std::to_wstring(range) + L" 天 · " +
                (metric ? L"日常速度 (CPM)" : L"成文字数"));
      const float axis = sub ? 194.0f : 320.0f;
      Label(dc, Wide(metrics::Day(base - (range - 1) * 86400)),
            {45, axis, 180, axis + 18}, fonts.family, p.text_muted);
      Label(dc, Wide(selected_day), {w - 160, axis, w - 12, axis + 18},
            fonts.family, p.text_muted, DT_RIGHT | DT_SINGLELINE);
      if (!sub)
        Keyboard(dc, 363.0f, day["keys"]);
      else {
        Label(dc, L"按键日历 · 全部历史", {0, 236, w, 264}, fonts.body,
              p.text_primary);
        dates.clear();
        const int size = static_cast<int>(w / 54);
        for (std::size_t yearIndex = 0; yearIndex < years.size(); ++yearIndex) {
          const float offset = static_cast<float>(yearIndex) * 150;
          if (420 + offset < scroll || 270 + offset > scroll + height) continue;
          Label(dc, std::to_wstring(years[yearIndex]),
                {0, 270 + offset, w, 292 + offset}, fonts.family, p.text_muted);
          tm start{};
          start.tm_year = years[yearIndex] - 1900;
          start.tm_mon = 0;
          start.tm_mday = 1;
          start.tm_hour = 12;
          start.tm_isdst = -1;
          const auto first = static_cast<double>(mktime(&start));
          for (int i = 0; i < 366; ++i) {
            const auto date = metrics::Day(first + i * 86400);
            if (std::stoi(date.substr(0, 4)) != years[yearIndex] ||
                date > metrics::Day(metrics::Wall()))
              break;
            std::uint64_t count = 0;
            if (data["days"].contains(date))
              for (const auto& n : data["days"][date]["keys"])
                count += n.get<unsigned>();
            float x = static_cast<float>((i + start.tm_wday) / 7 * size),
                  y = 300.0f + (i + start.tm_wday) % 7 * size + offset;
            ui::DipRect box{x, y, x + size - 2, y + size - 2};
            ui::FillRoundRect(
                dc, Rect(box), Px(2),
                ui::ToColorRef(
                    count ? ui::Blend(
                                p.surface_secondary, p.accent,
                                (std::min)(0.9f,
                                           0.2f + std::log1p(static_cast<float>(
                                                      count)) /
                                                      12))
                          : p.surface_secondary));
            dates.push_back({box, date});
          }
        }
        const float offset = static_cast<float>(years.size() - 1) * 150;
        Label(dc, Wide(selected_day) + L" · 选中日期",
              {0, 394 + offset, w, 418 + offset}, fonts.body, p.text_secondary);
        MetricCards(dc, 432 + offset,
                    {L"成文字数", L"输入按键", L"活跃时长", L"日常速度"},
                    cardValues);
        Keyboard(dc, 530 + offset, day["keys"]);
        std::vector<std::optional<double>> sessions;
        for (const auto& s : data["sessions"]) {
          const double sec = s.value("active", 0.0);
          sessions.push_back(sec > 0 ? std::optional<double>(
                                           s.value("characters", 0) * 60 / sec)
                                     : std::nullopt);
        }
        if (sessions.size() > 100)
          sessions.erase(sessions.begin(), sessions.end() - 100);
        Chart(dc, 796 + offset, 165, sessions, L"最近输入会话 · 日常速度");
        Label(dc, L"选择日期或点击日历格查看当天记录。",
              {0, 990 + offset, w, 1030 + offset}, fonts.family, p.text_muted);
      }
      Label(
          dc,
          L"只记录 RIMES 输入路径的汇总。活跃时长按 10 "
          L"秒内的连续输入累计。\n按键分布和输入趋势可独立关闭，停用保留历史；"
          L"文章跟打不计入日常统计。",
          {0, static_cast<float>(total - 70), w, static_cast<float>(total - 8)},
          fonts.family, p.text_muted);
    }
    if (!service->Issue().empty())
      Label(dc, Wide(service->Issue()),
            {160, static_cast<float>(total - 85), w,
             static_cast<float>(total - 8)},
            fonts.family, p.warning_text);
  }
  double SelectedTime() {
    tm value{};
    int y = 0, m = 0, d = 0;
    if (sscanf_s(selected_day.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
      return metrics::Wall();
    value.tm_year = y - 1900;
    value.tm_mon = m - 1;
    value.tm_mday = d;
    value.tm_hour = 12;
    value.tm_isdst = -1;
    return static_cast<double>(mktime(&value));
  }
  void ConfirmClear(const std::string& kind, const std::string& day = {}) {
    if (MessageBoxW(
            hwnd,
            L"确定清空这些本机记录？此操作无法撤销，其他类型的记录将保留。",
            L"清空记录", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
      service->Clear(kind, day);
      Build();
    }
  }
  void Command(int id, int notification) {
    // RichEdit emits EN_UPDATE while painting. Treating it as an action and
    // invalidating all children below creates a paint -> EN_UPDATE -> paint
    // loop even when no text changes. Only actual editor changes publish.
    if (id == 7003 || id == 7004) {
      if (id == 7004 && notification == EN_CHANGE && !suppress)
        PostMessageW(hwnd, WM_APP + 72, 0, 0);
      return;
    }
    if (id == 7014) {
      if (notification == LBN_SELCHANGE) {
        const auto n = SendMessageW(controls[id], LB_GETCURSEL, 0, 0);
        const auto selected = n < 0 ? -1 : SendMessageW(controls[id], LB_GETITEMDATA, n, 0);
        if (selected <= -2 && -(selected + 2) < static_cast<LRESULT>(books.size())) {
          selected_book = books[static_cast<std::size_t>(-(selected + 2))].at("id").get<std::string>();
          Build(); return;
        }
        if (selected >= 0 && selected < static_cast<LRESULT>(articles.size()) && selected != article) {
          ResetArticle(); article = static_cast<int>(selected); RememberView(); Build();
        }
      }
      return;
    }
    if (id == 7000 || id == 7010) {
      if (notification != CBN_SELCHANGE) return;
    } else if (notification != BN_CLICKED) {
      return;
    }
    if (id == 7000 && notification == CBN_SELCHANGE) {
      ResetArticle();
      article =
          static_cast<int>(SendMessageW(controls[id], CB_GETCURSEL, 0, 0));
      RememberView();
      status = L"选择文章后开始跟打";
      if (editor) {
        suppress = true;
        SetWindowTextW(editor, L"");
        suppress = false;
      }
      Build();
    }
    if (id == 7006 || id == 7015 || id == 7016) {
      if (id == 7006) materials_visible = !materials_visible;
      if (id == 7015) grid_visible = !grid_visible;
      if (id == 7016) cells_visible = !cells_visible;
      RememberView(); Build();
      if (service->Armed() && GetFocus() != editor) SetFocus(editor);
      return;
    }
    if (id == 7009 || id == 7012) { books_tab = id == 7012; selected_book.clear(); Build(); }
    if (id == 7017) { selected_book.clear(); Build(); }
    if (id == 7007) ImportMaterials();
    if (id == 7008 && !service->Armed() && !selected_book.empty()) {
      if (MessageBoxW(parent, L"移除本书的跟打素材？已保存的成绩会保留。", L"移除书籍", MB_YESNO | MB_ICONQUESTION) == IDYES)
        LoadMaterials({}, selected_book);
    }
    if (id == 7001) {
      Start();
      Refresh();
    }
    if (id == 7002) {
      if (ComposingEditor()) {
        status = L"请先确认或取消当前组字，再结束";
        SetFocus(editor);
      } else
        Stop("");
      Refresh();
    }
    if (id == 7005) {
      ResetArticle();
      std::vector<int> choices;
      const auto current_book = articles[article].id.rfind("epub.", 0) == 0 ? articles[article].id.substr(5, 16) : "";
      for (std::size_t i = 0; i < articles.size(); ++i) {
        const bool in_pool = current_book.empty() ? articles[i].id.rfind("epub.", 0) != 0
            : articles[i].id.rfind("epub." + current_book + ".", 0) == 0;
        if (in_pool && i != article) choices.push_back(static_cast<int>(i));
      }
      if (!choices.empty()) article = choices[std::random_device{}() % choices.size()];
      RememberView();
      Build();
    }
    if (id == 7010 && notification == CBN_SELCHANGE) {
      auto n = SendMessageW(controls[id], CB_GETCURSEL, 0, 0);
      if (n >= 0 && n < static_cast<LRESULT>(comparisons.size()))
        comparison = comparisons[n];
      Build();
    }
    if (id == 7011) ConfirmClear("results");
    if (id >= 7300 && id < 7800 &&
        id - 7300 < static_cast<int>(results.size())) {
      const auto& r = results[id - 7300];
      const auto title = Wide(articles[article].title);
      MessageBoxW(
          hwnd,
          (Detail(r["metrics"]) + L"\n\n输入方案：" +
           Wide(r["context"].value("schema", "")) + L"\n文章版本：" +
           std::to_wstring(r.value("articleVersion", 1)) + L"\n输入源：" +
           Wide(r["context"].value("inputSource", "")) + L"\n键位版本：" +
           Wide(r["context"].value("keymapRevision", "")))
              .c_str(),
          title.c_str(), MB_OK);
    }
    if (id == 7100 || id == 7101) range = id == 7100 ? 7 : 30;
    if (id == 7102 || id == 7103) metric = id == 7102 ? 0 : 1;
    if (id == 7110 || id == 7111) {
      auto d = service->Data();
      service->Preferences(id == 7110 ? !d["recordKeys"].get<bool>()
                                      : d["recordKeys"].get<bool>(),
                           id == 7111 ? !d["recordActivity"].get<bool>()
                                      : d["recordActivity"].get<bool>());
    }
    if (id == 7112) ConfirmClear("activity");
    if (id == 7113) ConfirmClear("keys", sub ? "" : selected_day);
    if (id == 7200 &&
        MessageBoxW(hwnd, L"将先备份当前统计文件，再创建空记录。是否继续？",
                    L"备份并重建", MB_YESNO | MB_DEFBUTTON2) == IDYES) {
      service->Repair();
      Build();
    }
    if (id >= 8000) {
      const auto index = (id - 8000) / 4;
      const auto action = (id - 8000) % 4;
      if (index >= static_cast<int>(plugins.size())) return;
      const auto p = plugins[index];
      if (action == 1) {
        MessageBoxW(hwnd,
                    (Description(p.id) + L"\n\n" + Kind(p.kind) + L" · " +
                     Wide(p.version) + L"\n" + Wide(p.id) +
                     L"\n停用或卸载保留本机数据。")
                        .c_str(),
                    Wide(p.name).c_str(), MB_OK);
        return;
      }
      if (action == 2) {
        if (cb.navigate) {
          auto route =
              p.id == "builtin.typing-speed" ? ui::SettingsPage::kTyping
              : p.id == "builtin.statistics" ? ui::SettingsPage::kStatistics
              : p.kind == "extension"        ? ui::SettingsPage::kChord
              : p.kind == "capsule"          ? ui::SettingsPage::kCapsule
              : p.kind == "mailbox"          ? ui::SettingsPage::kMailbox
                                             : ui::SettingsPage::kBuffer;
          if (p.enabled)
            cb.navigate(route);
          else
            message = L"先启用该插件，再打开设置。";
        }
        return;
      }
      if (action == 3 &&
          MessageBoxW(hwnd, L"卸载此插件？本机数据将保留。",
                      Wide(p.name).c_str(), MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
      std::string error;
      const auto op = action == 3    ? "uninstall"
                      : !p.installed ? "install"
                      : p.enabled    ? "disable"
                                     : "enable";
      if (cb.manage && cb.manage(p.id, op, &error))
        message = L"已更新 · " + Wide(p.name);
      else
        message = Wide(error);
      Build();
      PostMessageW(parent, WM_APP + 73, 0, 0);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
  }
  static LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM w, LPARAM l,
                                    UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Impl*>(ref);
    if (msg == WM_NCDESTROY) {
      RemoveWindowSubclass(h, ComboProc, id);
      return DefSubclassProc(h, msg, w, l);
    }
    if (msg != WM_PAINT && msg != WM_PRINTCLIENT && msg != WM_PRINT)
      return DefSubclassProc(h, msg, w, l);
    // Preserve the native segmented date editor while it has keyboard focus.
    // Its resting state follows the same theme as the other selector controls.
    if (GetDlgCtrlID(h) == 7104 && GetFocus() == h)
      return DefSubclassProc(h, msg, w, l);
    PAINTSTRUCT ps{};
    auto dc = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
    RECT r{};
    GetClientRect(h, &r);
    const auto& p = self->Palette();
    ui::FillRectColor(dc, r, ui::ToColorRef(p.settings_background));
    ui::FillRoundRect(dc, r, self->Px(6), ui::ToColorRef(p.surface));
    ui::StrokeRoundRect(dc, r, self->Px(6),
                        ui::ToColorRef(GetFocus() == h ? p.accent : p.border));
    RECT label = r;
    label.left += self->Px(8);
    label.right -= self->Px(28);
    SelectObject(dc, self->fonts.body);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ToColorRef(p.text_primary));
    const auto text =
        GetDlgCtrlID(h) == 7104 ? Wide(self->selected_day) : Text(h);
    DrawTextW(dc, text.c_str(), -1, &label,
              DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    auto pen =
        CreatePen(PS_SOLID, self->Px(1), ui::ToColorRef(p.text_secondary));
    const auto old = SelectObject(dc, pen);
    const int x = r.right - self->Px(14), y = (r.top + r.bottom) / 2;
    MoveToEx(dc, x - self->Px(4), y - self->Px(2), nullptr);
    LineTo(dc, x, y + self->Px(2));
    LineTo(dc, x + self->Px(4), y - self->Px(2));
    SelectObject(dc, old);
    DeleteObject(pen);
    if (msg == WM_PAINT) EndPaint(h, &ps);
    return 0;
  }
  static LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM w, LPARAM l,
                                     UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Impl*>(ref);
    if (msg == WM_NCDESTROY) {
      RemoveWindowSubclass(h, EditorProc, id);
      return DefSubclassProc(h, msg, w, l);
    }
    if (msg == WM_GETDLGCODE)
      return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_PRINT) {
      PAINTSTRUCT paint{};
      auto destination = msg == WM_PAINT ? BeginPaint(h, &paint) : reinterpret_cast<HDC>(w);
      if (!destination) return 0;
      RECT r{}; GetClientRect(h, &r);
      RECT visible = r;
      if (msg == WM_PAINT) {
        RECT clip{};
        if (GetClipBox(destination, &clip) != ERROR) IntersectRect(&visible, &r, &clip);
      }
      if (IsRectEmpty(&visible)) {
        if (msg == WM_PAINT) EndPaint(h, &paint);
        return 0;
      }
      // The document can be thousands of pixels high. Allocate only the dirty
      // viewport band instead of a full-document bitmap for every key/caret.
      const auto frame = CreateCompatibleDC(destination);
      const auto bitmap = frame ? CreateCompatibleBitmap(destination, std::max(1L, r.right), std::max(1L, visible.bottom - visible.top)) : nullptr;
      if (bitmap) {
        const auto old = SelectObject(frame, bitmap);
        SetViewportOrgEx(frame, 0, -visible.top, nullptr);
        // RichEdit supplies native text, marked text, selection and TSF geometry.
        // Draw its frame and both tracks together, then present only once.
        DefSubclassProc(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame), PRF_CLIENT | PRF_ERASEBKGND);
        self->PaintTrack(h, frame);
        BitBlt(destination, 0, visible.top, r.right, visible.bottom - visible.top, frame, 0, visible.top, SRCCOPY);
        SelectObject(frame, old); DeleteObject(bitmap);
      } else {
        DefSubclassProc(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(destination), PRF_CLIENT | PRF_ERASEBKGND);
        self->PaintTrack(h, destination);
      }
      if (frame) DeleteDC(frame);
      if (msg == WM_PAINT) EndPaint(h, &paint);
      return 0;
    }
    if (msg == WM_CHAR && (w == '\r' || w == '\n') && !self->ComposingEditor()) return 0;
    // Paragraph separators are layout, not a second deletion step. Backspace
    // at the first cell removes the preceding real character with its separator.
    if (msg == WM_CHAR && w == '\b' && !self->ComposingEditor() && self->service->Armed()) {
      CHARRANGE range{}; SendMessageW(h, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
      const auto text = Text(h);
      if (range.cpMin == range.cpMax && range.cpMin > 0 && text[range.cpMin - 1] == '\r') {
        auto start = range.cpMin - 1;
        const auto preceding = rimes::practice::materials::Characters(Utf8(text.substr(0, start)));
        if (!preceding.empty()) start -= static_cast<LONG>(Wide(preceding.back()).size());
        range.cpMin = start;
        SendMessageW(h, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        SendMessageW(h, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        PostMessageW(self->hwnd, WM_APP + 72, 0, 0); return 0;
      }
    }
    if (msg == WM_PASTE || msg == WM_DROPFILES || msg == WM_UNDO ||
        (msg == WM_KEYDOWN &&
         ((w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) ||
          (w == VK_INSERT && (GetKeyState(VK_SHIFT) & 0x8000))))) {
      if (self->service->Armed()) {
        self->service->Mark("assistedInput");
        self->status = L"不支持粘贴或拖入 · 本次记为练习";
        MessageBeep(MB_OK);
      }
      return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
      unsigned mods = 0;
      if (GetKeyState(VK_CONTROL) & 0x8000)
        mods |= static_cast<unsigned>(core::KeyModifiers::kControl);
      if (GetKeyState(VK_MENU) & 0x8000)
        mods |= static_cast<unsigned>(core::KeyModifiers::kAlt);
      if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
        mods |= static_cast<unsigned>(core::KeyModifiers::kWindows);
      self->service->LocalKey(
          static_cast<unsigned>(w), mods, (l & (1LL << 30)) != 0,
          static_cast<DWORD>(GetMessageTime()), self->ComposingEditor());
      self->Refresh();
    }
    if (msg == WM_IME_STARTCOMPOSITION) { self->ime_composing = true; self->service->Composition(); }
    if (msg == WM_KILLFOCUS && self->service->Armed()) {
      self->service->Mark("focusLost");
      PostMessageW(self->hwnd, WM_APP + 74, self->practice_generation, 0);
    }
    CHARRANGE replaced{};
    std::wstring before;
    if (msg == WM_CHAR && w >= 32 && self->service->Armed() && !self->ComposingEditor()) {
      SendMessageW(h, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&replaced));
      if (replaced.cpMin < replaced.cpMax) before = Text(h);
    }
    const auto result = DefSubclassProc(h, msg, w, l);
    if (msg == WM_IME_ENDCOMPOSITION) self->ime_composing = false;
    // RichEdit can replace a selection with identical text. A plain text
    // diff cannot see this real attempt/correction, so preserve its range.
    if (!before.empty() && before == Text(h) && !self->ComposingEditor()) {
      const auto prefix =
          metrics::Characters(Utf8(before.substr(0, replaced.cpMin)));
      const auto selection = metrics::Characters(
          Utf8(before.substr(replaced.cpMin, replaced.cpMax - replaced.cpMin)));
      const auto suffix =
          metrics::Characters(Utf8(before.substr(replaced.cpMax)));
      if (prefix.size() + selection.size() + suffix.size() ==
          metrics::Characters(Utf8(before)).size())
        self->service->Reconcile(
            Utf8(before), {{prefix.size(), prefix.size() + selection.size()}});
    }
    if (msg == WM_IME_ENDCOMPOSITION || msg == WM_CHAR)
      PostMessageW(self->hwnd, WM_APP + 72, 0, 0);
    if (msg == WM_MOUSEWHEEL) { SendMessageW(self->hwnd, msg, w, l); return 0; }
    return result;
  }
  static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<Impl*>(
          reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, msg, w, l);
    switch (msg) {
      case WM_PRINTCLIENT:
        self->Paint(reinterpret_cast<HDC>(w));
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        self->Paint(dc);
        EndPaint(h, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_TIMER:
        self->Refresh();
        return 0;
      case WM_APP + 72:
        self->Publish();
        return 0;
      case WM_APP + 74:
        if (w == self->practice_generation) {
          self->Stop("focusLost");
          self->Refresh();
        }
        return 0;
      case WM_COMMAND:
        self->Command(LOWORD(w), HIWORD(w));
        return 0;
      case WM_NOTIFY: {
        auto* n = reinterpret_cast<NMHDR*>(l);
        if (n->idFrom == 7104 && n->code == DTN_DATETIMECHANGE) {
          auto* d = reinterpret_cast<NMDATETIMECHANGE*>(l);
          if (d->dwFlags == GDT_VALID) {
            char text[16];
            sprintf_s(text, "%04u-%02u-%02u", d->st.wYear, d->st.wMonth,
                      d->st.wDay);
            self->selected_day = text;
            InvalidateRect(h, nullptr, FALSE);
          }
        }
        return 0;
      }
      case WM_LBUTTONUP: {
        const float x = static_cast<short>(LOWORD(l)) * 96.0f / self->dpi,
                    y = static_cast<short>(HIWORD(l)) * 96.0f / self->dpi +
                        self->scroll;
        for (const auto& [r, day] : self->dates)
          if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            self->selected_day = day;
            SYSTEMTIME selected{};
            selected.wYear = static_cast<WORD>(std::stoi(day.substr(0, 4)));
            selected.wMonth = static_cast<WORD>(std::stoi(day.substr(5, 2)));
            selected.wDay = static_cast<WORD>(std::stoi(day.substr(8, 2)));
            SendMessageW(self->controls[7104], DTM_SETSYSTEMTIME, GDT_VALID,
                         reinterpret_cast<LPARAM>(&selected));
            InvalidateRect(h, nullptr, FALSE);
            break;
          }
        return 0;
      }
      case WM_VSCROLL:
      case WM_MOUSEWHEEL: {
        int next = self->scroll;
        if (msg == WM_MOUSEWHEEL)
          next -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 70;
        else {
          SCROLLINFO s{sizeof(s), SIF_TRACKPOS};
          GetScrollInfo(self->scrollbar, SB_CTL, &s);
          switch (LOWORD(w)) {
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
              next = s.nTrackPos;
              break;
            case SB_TOP:
              next = 0;
              break;
            case SB_BOTTOM:
              next = self->total - self->height;
              break;
            case SB_LINEUP:
              next -= 32;
              break;
            case SB_LINEDOWN:
              next += 32;
              break;
            case SB_PAGEUP:
              next -= self->height - 32;
              break;
            case SB_PAGEDOWN:
              next += self->height - 32;
              break;
          }
        }
        self->scroll =
            (std::clamp)(next, 0, (std::max)(0, self->total - self->height));
        self->ShowControls();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX: {
        auto dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, ui::ToColorRef(self->Palette().text_primary));
        SetBkColor(dc, ui::ToColorRef(self->Palette().surface));
        return reinterpret_cast<LRESULT>(self->surface);
      }
      case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (item->CtlType == ODT_LISTBOX && item->CtlID == 7014) {
          const auto& p = self->Palette();
          const bool selected = (item->itemState & ODS_SELECTED) != 0;
          auto r = item->rcItem;
          ui::FillRectColor(item->hDC, r, ui::ToColorRef(p.settings_background));
          if (item->itemID == static_cast<UINT>(-1)) return TRUE;
          InflateRect(&r, -self->Px(2), -self->Px(3));
          ui::FillRoundRect(item->hDC, r, self->Px(8), ui::ToColorRef(selected ? ui::Blend(p.surface, p.accent, .12f) : p.surface));
          ui::StrokeRoundRect(item->hDC, r, self->Px(8), ui::ToColorRef(selected ? p.accent : p.border));
          std::wstring title, detail;
          const auto index = static_cast<INT_PTR>(item->itemData);
          if (index <= -2 && -(index + 2) < static_cast<INT_PTR>(self->books.size())) {
            const auto& book = self->books[static_cast<std::size_t>(-(index + 2))];
            title = Wide(book.value("title", std::string("未命名书籍")));
            detail = Wide(book.value("author", std::string())) + L" · 打开章节段落";
          } else if (index >= 0 && index < static_cast<INT_PTR>(self->articles.size())) {
            const auto& a = self->articles[static_cast<std::size_t>(index)];
            const auto dot = a.id.find_last_of('.');
            title = a.id.rfind("epub.", 0) == 0 ? L"第 " + Wide(a.id.substr(dot + 1)) + L" 段 · " + Wide(a.title) : Wide(a.title);
            detail = std::to_wstring(metrics::Characters(a.text).size()) + L" 字符 · " + Wide(a.theme);
          }
          SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, ui::ToColorRef(p.text_primary));
          SelectObject(item->hDC, self->fonts.body);
          auto text = r; InflateRect(&text, -self->Px(10), 0); text.top += self->Px(9); text.bottom = text.top + self->Px(20);
          DrawTextW(item->hDC, title.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
          text.top += self->Px(24); text.bottom += self->Px(24);
          SetTextColor(item->hDC, ui::ToColorRef(p.text_muted)); SelectObject(item->hDC, self->fonts.family);
          DrawTextW(item->hDC, detail.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
          if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &r);
          return TRUE;
        }
        if (item->CtlType != ODT_BUTTON) break;
        const auto& p = self->Palette();
        bool selected = false;
        auto d = self->service->Data();
        if (item->CtlID == 7100) selected = self->range == 7;
        if (item->CtlID == 7101) selected = self->range == 30;
        if (item->CtlID == 7102) selected = self->metric == 0;
        if (item->CtlID == 7103) selected = self->metric == 1;
        if (item->CtlID == 7110) selected = d["recordKeys"].get<bool>();
        if (item->CtlID == 7111) selected = d["recordActivity"].get<bool>();
        auto r = item->rcItem;
        ui::FillRectColor(
            item->hDC, r,
            ui::ToColorRef(item->CtlID >= 8000 ? p.surface_secondary
                                               : p.settings_background));
        if (item->CtlID >= 8000 && (item->CtlID - 8000) % 4 == 0) {
          const auto index = (item->CtlID - 8000) / 4;
          if (index < self->plugins.size() && self->plugins[index].installed) {
            const bool enabled = self->plugins[index].enabled;
            RECT track{r.right - self->Px(36), (r.bottom - self->Px(20)) / 2,
                       r.right, (r.bottom + self->Px(20)) / 2};
            ui::FillRoundRect(
                item->hDC, track, self->Px(10),
                ui::ToColorRef(enabled ? p.accent : p.border_strong));
            RECT knob{
                enabled ? track.right - self->Px(18) : track.left + self->Px(2),
                track.top + self->Px(2),
                enabled ? track.right - self->Px(2) : track.left + self->Px(18),
                track.bottom - self->Px(2)};
            ui::FillRoundRect(item->hDC, knob, self->Px(8), RGB(255, 255, 255));
            if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &r);
            return TRUE;
          }
        }
        ui::FillRoundRect(
            item->hDC, r, self->Px(6),
            ui::ToColorRef(selected ? p.accent : p.surface_secondary));
        ui::StrokeRoundRect(
            item->hDC, r, self->Px(6),
            ui::ToColorRef(item->itemState & ODS_FOCUS ? p.accent : p.border));
        SelectObject(item->hDC, self->fonts.body);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, ui::ToColorRef(item->itemState & ODS_DISABLED
                                                   ? p.text_muted
                                               : selected ? p.accent_foreground
                                                          : p.text_primary));
        auto text = Text(item->hwndItem);
        DrawTextW(item->hDC, text.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
      }
      case WM_DESTROY:
        KillTimer(h, 1);
        break;
    }
    return DefWindowProcW(h, msg, w, l);
  }
};
ExtensionPages::ExtensionPages(HWND parent, ExtensionCallbacks callbacks)
    : impl_(std::make_unique<Impl>(parent, std::move(callbacks))) {}
ExtensionPages::~ExtensionPages() = default;
HWND ExtensionPages::window() const { return impl_->hwnd; }
bool ExtensionPages::Owns(HWND h) const {
  return h == impl_->hwnd || IsChild(impl_->hwnd, h);
}
void ExtensionPages::Deactivate() { impl_->Stop("focusLost"); }
void ExtensionPages::Layout(ui::SettingsPage page, int subpage,
                            const ui::DipRect& body, unsigned dpi,
                            ui::ThemeId theme) {
  auto& s = *impl_;
  const bool show = page == ui::SettingsPage::kTyping ||
                    page == ui::SettingsPage::kStatistics ||
                    page == ui::SettingsPage::kPlugins;
  if (s.page != page || s.sub != subpage) {
    s.Stop("interrupted");
    if (page == ui::SettingsPage::kTyping && subpage == 0) {
      s.ResetArticle();
    }
    s.scroll = 0;
  }
  s.page = page;
  s.sub = subpage;
  s.visible = show;
  if (!show) {
    ShowWindow(s.hwnd, SW_HIDE);
    return;
  }
  s.dpi = dpi;
  s.theme = theme;
  s.width = static_cast<int>(body.width());
  s.height = static_cast<int>(body.height());
  SetWindowPos(s.hwnd, nullptr, s.Px(body.left), s.Px(body.top),
               s.Px(body.width()), s.Px(body.height()),
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  s.Build();
}
}  // namespace rimes::windows::workbench
