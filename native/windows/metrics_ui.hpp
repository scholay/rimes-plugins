// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>

#include "../ui/settings_layout.hpp"
#include "metrics.hpp"
#include "official_plugins.hpp"
#include "provider.hpp"

namespace rimes::windows::workbench {
struct ExtensionCallbacks {
  std::shared_ptr<metrics::Service> metrics;
  std::function<Settings()> configuration;
  std::function<std::vector<PluginView>()> plugins;
  std::function<bool(const std::string&, const std::string&, std::string*)>
      manage;
  std::function<void(ui::SettingsPage)> navigate;
};
class ExtensionPages {
 public:
  ExtensionPages(HWND parent, ExtensionCallbacks callbacks);
  ~ExtensionPages();
  void Layout(ui::SettingsPage page, int subpage, const ui::DipRect& body,
              unsigned dpi, ui::ThemeId theme);
  bool Owns(HWND window) const;
  void Deactivate();
  HWND window() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace rimes::windows::workbench
