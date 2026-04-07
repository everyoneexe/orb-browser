#include "browser/tab_manager.h"
#include "browser/browser_window.h"

#include <algorithm>
#include <fstream>

namespace {
std::string JsEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\'': out += "\\'"; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:   out += c; break;
        }
    }
    return out;
}
}  // namespace

TabManager::TabManager() {}

int TabManager::CreateTab(const std::string& url) {
    int id = next_id_++;
    TabInfo info;
    info.id = id;
    info.url = url;
    info.title = "New Tab";
    tabs_[id] = info;

    // Notify UI about new tab
    NotifyUI("if(typeof orb !== 'undefined') orb.onTabCreated(" +
             std::to_string(id) + ",'" + JsEscape(url) + "');");

    return id;
}

int TabManager::CreateIncognitoTab(const std::string& url) {
    int id = next_id_++;
    TabInfo info;
    info.id = id;
    info.url = url;
    info.title = "New Incognito Tab";
    info.incognito = true;
    tabs_[id] = info;

    NotifyUI("if(typeof orb !== 'undefined') orb.onTabCreated(" +
             std::to_string(id) + ",'" + JsEscape(url) + "',true);");

    return id;
}

void TabManager::CloseTab(int tab_id) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) return;

    // If closing active tab, switch to another first
    if (tab_id == active_tab_id_) {
        auto ids = GetTabIds();
        if (ids.size() > 1) {
            for (size_t i = 0; i < ids.size(); i++) {
                if (ids[i] == tab_id) {
                    int next = (i + 1 < ids.size()) ? ids[i + 1] : ids[i - 1];
                    SwitchTab(next);
                    break;
                }
            }
        } else {
            // Last tab — open a new newtab before closing
            if (window_) {
                // Read search engine from settings for newtab URL
                std::string engine = "google";
                const char* home = getenv("HOME");
                if (home) {
                    std::string path = std::string(home) + "/.config/orb-browser/settings.json";
                    std::ifstream f(path);
                    if (f.good()) {
                        std::string content((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
                        auto pos = content.find("\"searchEngine\"");
                        if (pos != std::string::npos) {
                            auto q1 = content.find('"', content.find(':', pos) + 1);
                            auto q2 = content.find('"', q1 + 1);
                            if (q1 != std::string::npos && q2 != std::string::npos) {
                                engine = content.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                }
                std::string url = "orb://newtab/?engine=" + engine;
                int new_id = CreateTab(url);
                window_->CreateTabBrowser(new_id, url);
                SwitchTab(new_id);
            }
        }
    }

    if (it->second.browser) {
        it->second.browser->GetHost()->CloseBrowser(false);
    }
}

void TabManager::CloseActiveTab() {
    if (active_tab_id_ >= 0) {
        CloseTab(active_tab_id_);
    }
}

void TabManager::SwitchTab(int tab_id) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) return;

    active_tab_id_ = tab_id;

    // Show the tab's surface in BrowserWindow
    if (window_) window_->ShowTab(tab_id);

    NotifyUI("if(typeof orb !== 'undefined') orb.onActiveTabChanged(" +
             std::to_string(tab_id) + ");");

    // Also push current URL
    NotifyUI("if(typeof orb !== 'undefined') orb.onAddressChange(" +
             std::to_string(tab_id) + ",'" + JsEscape(it->second.url) + "');");
}

void TabManager::SwitchToNextTab() {
    auto ids = GetTabIds();
    if (ids.size() <= 1) return;
    for (size_t i = 0; i < ids.size(); i++) {
        if (ids[i] == active_tab_id_) {
            int next = ids[(i + 1) % ids.size()];
            SwitchTab(next);
            return;
        }
    }
}

void TabManager::SwitchToPrevTab() {
    auto ids = GetTabIds();
    if (ids.size() <= 1) return;
    for (size_t i = 0; i < ids.size(); i++) {
        if (ids[i] == active_tab_id_) {
            int prev = ids[(i + ids.size() - 1) % ids.size()];
            SwitchTab(prev);
            return;
        }
    }
}

void TabManager::OnBrowserCreated(int tab_id, CefRefPtr<CefBrowser> browser) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) return;

    it->second.browser = browser;
    it->second.browser_id = browser->GetIdentifier();
    browser_to_tab_[browser->GetIdentifier()] = tab_id;
}

void TabManager::OnBrowserClosed(int browser_id) {
    int tab_id = GetTabIdByBrowserId(browser_id);
    if (tab_id < 0) return;

    NotifyUI("if(typeof orb !== 'undefined') orb.onTabClosed(" +
             std::to_string(tab_id) + ");");

    browser_to_tab_.erase(browser_id);
    tabs_.erase(tab_id);

    if (tabs_.empty()) {
        active_tab_id_ = -1;
    }
}

int TabManager::GetTabIdByBrowserId(int browser_id) const {
    auto it = browser_to_tab_.find(browser_id);
    if (it != browser_to_tab_.end()) return it->second;
    return -1;
}

CefRefPtr<CefBrowser> TabManager::GetBrowser(int tab_id) const {
    auto it = tabs_.find(tab_id);
    if (it != tabs_.end()) return it->second.browser;
    return nullptr;
}

const TabInfo* TabManager::GetTab(int tab_id) const {
    auto it = tabs_.find(tab_id);
    if (it != tabs_.end()) return &it->second;
    return nullptr;
}

void TabManager::UpdateTabUrl(int tab_id, const std::string& url) {
    auto it = tabs_.find(tab_id);
    if (it != tabs_.end()) {
        it->second.url = url;
    }
}

void TabManager::UpdateTabTitle(int tab_id, const std::string& title) {
    auto it = tabs_.find(tab_id);
    if (it != tabs_.end()) {
        it->second.title = title;
    }
}

std::vector<int> TabManager::GetTabIds() const {
    std::vector<int> ids;
    for (const auto& pair : tabs_) {
        ids.push_back(pair.first);
    }
    return ids;
}

void TabManager::NotifyUI(const std::string& js) {
    if (notify_cb_) {
        notify_cb_(js);
    }
}
