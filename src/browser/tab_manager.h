#pragma once

#include "include/cef_browser.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class BrowserWindow;

struct TabInfo {
    int id;
    int browser_id = -1;
    CefRefPtr<CefBrowser> browser;
    std::string title;
    std::string url;
    bool loading = false;
    bool incognito = false;
};

class TabManager {
public:
    using NotifyCallback = std::function<void(const std::string& js)>;

    TabManager();

    void SetNotifyCallback(NotifyCallback cb) { notify_cb_ = cb; }
    void SetBrowserWindow(BrowserWindow* window) { window_ = window; }

    int CreateTab(const std::string& url);
    int CreateIncognitoTab(const std::string& url);
    void CloseTab(int tab_id);
    void CloseActiveTab();
    void SwitchTab(int tab_id);
    void SwitchToNextTab();
    void SwitchToPrevTab();

    void OnBrowserCreated(int tab_id, CefRefPtr<CefBrowser> browser);
    void OnBrowserClosed(int browser_id);

    int GetTabIdByBrowserId(int browser_id) const;
    CefRefPtr<CefBrowser> GetBrowser(int tab_id) const;
    int GetActiveTabId() const { return active_tab_id_; }
    const TabInfo* GetTab(int tab_id) const;

    void UpdateTabUrl(int tab_id, const std::string& url);
    void UpdateTabTitle(int tab_id, const std::string& title);

    std::vector<int> GetTabIds() const;

private:
    void NotifyUI(const std::string& js);

    std::map<int, TabInfo> tabs_;
    std::unordered_map<int, int> browser_to_tab_;  // browser_id → tab_id
    int next_id_ = 1;
    int active_tab_id_ = -1;
    NotifyCallback notify_cb_;
    BrowserWindow* window_ = nullptr;
};
