#include "browser/query_handler.h"
#include "browser/browser_window.h"
#include "browser/browser_client.h"
#include "include/cef_task.h"

#include <gtk/gtk.h>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <sys/stat.h>

// Minimal JSON parsing helpers (avoid external dependency)
namespace {

std::string GetJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    // Number or other value
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) end = json.size();
    std::string val = json.substr(pos, end - pos);
    // Trim whitespace
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\n'))
        val.pop_back();
    return val;
}

int GetJsonInt(const std::string& json, const std::string& key) {
    std::string val = GetJsonString(json, key);
    if (val.empty()) return -1;
    try { return std::stoi(val); }
    catch (...) { return -1; }
}

bool GetJsonBool(const std::string& json, const std::string& key) {
    std::string val = GetJsonString(json, key);
    return val == "true";
}

// ─── Bookmark persistence ─────────────────────────────────

std::string GetBookmarkPath() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/orb-browser/bookmarks.json";
}

std::string GetHistoryPath() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/orb-browser/history.json";
}

std::string GetSettingsPath() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/orb-browser/settings.json";
}

std::string GetZoomPath() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/orb-browser/zoom.json";
}

void EnsureConfigDir() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string config = std::string(home) + "/.config";
    mkdir(config.c_str(), 0755);
    std::string dir = config + "/orb-browser";
    mkdir(dir.c_str(), 0755);
}

std::string ReadBookmarkFile() {
    std::ifstream f(GetBookmarkPath());
    if (!f.good()) return "[]";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    if (content.empty()) return "[]";
    return content;
}

void WriteBookmarkFile(const std::string& json) {
    EnsureConfigDir();
    std::ofstream f(GetBookmarkPath());
    f << json;
}

std::string ReadFileJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return "[]";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    if (content.empty()) return "[]";
    return content;
}

void WriteFileJson(const std::string& path, const std::string& json) {
    EnsureConfigDir();
    std::ofstream f(path);
    f << json;
}

// Escape string for JSON
std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

}  // namespace

OrbQueryHandler::OrbQueryHandler(TabManager* tab_manager, BrowserWindow* window)
    : tab_manager_(tab_manager), window_(window) {}

bool OrbQueryHandler::OnQuery(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               int64_t query_id,
                               const CefString& request,
                               bool persistent,
                               CefRefPtr<Callback> callback) {
    std::string json = request.ToString();
    std::string cmd = GetJsonString(json, "cmd");

    if (cmd == "newTab") {
        std::string url = GetJsonString(json, "url");
        if (url.empty()) url = "about:blank";

        int tab_id = tab_manager_->CreateTab(url);
        window_->CreateTabBrowser(tab_id, url);

        callback->Success("{\"tabId\":" + std::to_string(tab_id) + "}");
        return true;
    }

    if (cmd == "closeTab") {
        int tab_id = GetJsonInt(json, "tabId");
        if (tab_id >= 0) {
            tab_manager_->CloseTab(tab_id);
            callback->Success("{}");
        } else {
            callback->Failure(1, "Invalid tabId");
        }
        return true;
    }

    if (cmd == "switchTab") {
        int tab_id = GetJsonInt(json, "tabId");
        if (tab_id >= 0) {
            tab_manager_->SwitchTab(tab_id);
            callback->Success("{}");
        } else {
            callback->Failure(1, "Invalid tabId");
        }
        return true;
    }

    if (cmd == "navigate") {
        int tab_id = GetJsonInt(json, "tabId");
        std::string url = GetJsonString(json, "url");
        auto browser_ptr = tab_manager_->GetBrowser(tab_id);
        if (browser_ptr && browser_ptr->GetMainFrame()) {
            browser_ptr->GetMainFrame()->LoadURL(url);
            callback->Success("{}");
        } else {
            callback->Failure(1, "Tab not found");
        }
        return true;
    }

    if (cmd == "goBack") {
        int tab_id = GetJsonInt(json, "tabId");
        auto browser_ptr = tab_manager_->GetBrowser(tab_id);
        if (browser_ptr && browser_ptr->CanGoBack()) {
            browser_ptr->GoBack();
        }
        callback->Success("{}");
        return true;
    }

    if (cmd == "goForward") {
        int tab_id = GetJsonInt(json, "tabId");
        auto browser_ptr = tab_manager_->GetBrowser(tab_id);
        if (browser_ptr && browser_ptr->CanGoForward()) {
            browser_ptr->GoForward();
        }
        callback->Success("{}");
        return true;
    }

    if (cmd == "stop") {
        int tab_id = GetJsonInt(json, "tabId");
        auto browser_ptr = tab_manager_->GetBrowser(tab_id);
        if (browser_ptr) {
            browser_ptr->StopLoad();
        }
        callback->Success("{}");
        return true;
    }

    if (cmd == "reload") {
        int tab_id = GetJsonInt(json, "tabId");
        auto browser_ptr = tab_manager_->GetBrowser(tab_id);
        if (browser_ptr) {
            browser_ptr->Reload();
        }
        callback->Success("{}");
        return true;
    }

    if (cmd == "getTabInfo") {
        int active_id = tab_manager_->GetActiveTabId();
        callback->Success("{\"activeTabId\":" + std::to_string(active_id) + "}");
        return true;
    }

    // ─── Find in page ──────────────────────────────────────

    if (cmd == "find") {
        std::string text = GetJsonString(json, "text");
        bool forward = GetJsonString(json, "forward") != "false";
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0 && !text.empty()) {
            auto tab_browser = tab_manager_->GetBrowser(active);
            if (tab_browser) {
                tab_browser->GetHost()->Find(text, forward, false, false);
            }
        }
        callback->Success("{}");
        return true;
    }

    if (cmd == "stopFind") {
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0) {
            auto tab_browser = tab_manager_->GetBrowser(active);
            if (tab_browser) {
                tab_browser->GetHost()->StopFinding(true);
            }
        }
        callback->Success("{}");
        return true;
    }

    // ─── Bookmarks ─────────────────────────────────────────

    if (cmd == "bookmarkList") {
        std::string data = ReadBookmarkFile();
        callback->Success(data);
        return true;
    }

    if (cmd == "bookmarkAdd") {
        std::string url = GetJsonString(json, "url");
        std::string title = GetJsonString(json, "title");
        if (url.empty()) {
            callback->Failure(1, "No URL");
            return true;
        }

        // Read existing bookmarks, append new one
        std::string data = ReadBookmarkFile();
        // Remove trailing ]
        if (!data.empty() && data.back() == ']') {
            data.pop_back();
        }
        // Add comma if not empty array
        if (data.size() > 1) {
            data += ",";
        }
        data += "{\"url\":\"" + JsonEscape(url) + "\",\"title\":\"" + JsonEscape(title) + "\"}]";
        WriteBookmarkFile(data);
        callback->Success("{}");
        return true;
    }

    if (cmd == "bookmarkRemove") {
        std::string url = GetJsonString(json, "url");
        if (url.empty()) {
            callback->Failure(1, "No URL");
            return true;
        }

        // Read bookmarks, filter out the one with matching URL
        std::string data = ReadBookmarkFile();
        // Simple approach: find and remove the entry
        std::string search = "\"url\":\"" + JsonEscape(url) + "\"";
        auto pos = data.find(search);
        if (pos != std::string::npos) {
            // Find the { before this entry
            auto start = data.rfind('{', pos);
            // Find the } after this entry
            auto end = data.find('}', pos);
            if (start != std::string::npos && end != std::string::npos) {
                end++; // include the }
                // Remove comma before or after
                if (end < data.size() && data[end] == ',') end++;
                else if (start > 0 && data[start - 1] == ',') start--;
                data.erase(start, end - start);
            }
        }
        WriteBookmarkFile(data);
        callback->Success("{}");
        return true;
    }

    // ─── History ───────────────────────────────────────────

    if (cmd == "historyAdd") {
        std::string url = GetJsonString(json, "url");
        std::string title = GetJsonString(json, "title");
        if (url.empty() || url.find("orb://") == 0) {
            callback->Success("{}");
            return true;
        }
        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();

        std::string data = ReadFileJson(GetHistoryPath());
        if (!data.empty() && data.back() == ']') data.pop_back();
        if (data.size() > 1) data += ",";
        data += "{\"url\":\"" + JsonEscape(url) + "\",\"title\":\"" + JsonEscape(title) +
                "\",\"time\":" + std::to_string(epoch) + "}]";

        // Keep max 500 entries — trim from front
        // Count entries by counting '{' at top level
        int count = 0;
        for (char c : data) { if (c == '{') count++; }
        if (count > 500) {
            auto first_end = data.find("},", 1);
            if (first_end != std::string::npos) {
                data = "[" + data.substr(first_end + 2);
            }
        }

        WriteFileJson(GetHistoryPath(), data);
        callback->Success("{}");
        return true;
    }

    if (cmd == "historyList") {
        std::string data = ReadFileJson(GetHistoryPath());
        callback->Success(data);
        return true;
    }

    if (cmd == "historyClear") {
        WriteFileJson(GetHistoryPath(), "[]");
        callback->Success("{}");
        return true;
    }

    // ─── Settings ──────────────────────────────────────────

    if (cmd == "settingsGet") {
        std::string data = ReadFileJson(GetSettingsPath());
        if (data == "[]") data = "{}";
        callback->Success(data);
        return true;
    }

    if (cmd == "settingsSave") {
        std::string key = GetJsonString(json, "key");
        std::string value = GetJsonString(json, "value");
        // Simple key-value store
        std::string data = ReadFileJson(GetSettingsPath());
        if (data == "[]") data = "{}";
        // Check if key exists, update or add
        std::string search = "\"" + key + "\"";
        auto pos = data.find(search);
        if (pos != std::string::npos) {
            // Replace value
            auto colon = data.find(':', pos);
            if (colon != std::string::npos) {
                auto vstart = colon + 1;
                while (vstart < data.size() && data[vstart] == ' ') vstart++;
                auto vend = data.find_first_of(",}", vstart);
                if (data[vstart] == '"') {
                    vend = data.find('"', vstart + 1);
                    if (vend != std::string::npos) vend++;
                }
                data.replace(vstart, vend - vstart, "\"" + JsonEscape(value) + "\"");
            }
        } else {
            // Add new key
            if (data.size() > 1 && data.back() == '}') {
                data.pop_back();
                if (data.size() > 1) data += ",";
                data += "\"" + JsonEscape(key) + "\":\"" + JsonEscape(value) + "\"}";
            }
        }
        WriteFileJson(GetSettingsPath(), data);
        callback->Success("{}");
        return true;
    }

    // ─── Zoom (from menu) ───────────────────────────────────

    if (cmd == "zoomIn") {
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0) {
            auto b = tab_manager_->GetBrowser(active);
            if (b) {
                double lvl = b->GetHost()->GetZoomLevel() + 0.5;
                b->GetHost()->SetZoomLevel(lvl);
                // Save per-domain zoom
                const TabInfo* info = tab_manager_->GetTab(active);
                if (info && !info->url.empty()) {
                    try {
                        std::string domain = info->url;
                        auto spos = domain.find("://");
                        if (spos != std::string::npos) { domain = domain.substr(spos + 3); auto epos = domain.find('/'); if (epos != std::string::npos) domain = domain.substr(0, epos); }
                        std::string data = ReadFileJson(GetZoomPath());
                        if (data == "[]") data = "{}";
                        std::string search = "\"" + domain + "\"";
                        auto p = data.find(search);
                        if (p != std::string::npos) {
                            auto colon = data.find(':', p); auto vend = data.find_first_of(",}", colon + 1);
                            data.replace(colon + 1, vend - colon - 1, std::to_string(lvl));
                        } else {
                            if (data.back() == '}') { data.pop_back(); if (data.size() > 1) data += ","; data += "\"" + domain + "\":" + std::to_string(lvl) + "}"; }
                        }
                        WriteFileJson(GetZoomPath(), data);
                    } catch (...) {}
                }
            }
        }
        if (browser && browser->GetMainFrame())
            browser->GetHost()->SetZoomLevel(0.0);
        callback->Success("{}");
        return true;
    }

    if (cmd == "zoomOut") {
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0) {
            auto b = tab_manager_->GetBrowser(active);
            if (b) {
                double lvl = b->GetHost()->GetZoomLevel() - 0.5;
                b->GetHost()->SetZoomLevel(lvl);
                const TabInfo* info = tab_manager_->GetTab(active);
                if (info && !info->url.empty()) {
                    try {
                        std::string domain = info->url;
                        auto spos = domain.find("://");
                        if (spos != std::string::npos) { domain = domain.substr(spos + 3); auto epos = domain.find('/'); if (epos != std::string::npos) domain = domain.substr(0, epos); }
                        std::string data = ReadFileJson(GetZoomPath());
                        if (data == "[]") data = "{}";
                        std::string search = "\"" + domain + "\"";
                        auto p = data.find(search);
                        if (p != std::string::npos) {
                            auto colon = data.find(':', p); auto vend = data.find_first_of(",}", colon + 1);
                            data.replace(colon + 1, vend - colon - 1, std::to_string(lvl));
                        } else {
                            if (data.back() == '}') { data.pop_back(); if (data.size() > 1) data += ","; data += "\"" + domain + "\":" + std::to_string(lvl) + "}"; }
                        }
                        WriteFileJson(GetZoomPath(), data);
                    } catch (...) {}
                }
            }
        }
        if (browser && browser->GetMainFrame())
            browser->GetHost()->SetZoomLevel(0.0);
        callback->Success("{}");
        return true;
    }

    if (cmd == "zoomReset") {
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0) {
            auto b = tab_manager_->GetBrowser(active);
            if (b) {
                b->GetHost()->SetZoomLevel(0.0);
                const TabInfo* info = tab_manager_->GetTab(active);
                if (info && !info->url.empty()) {
                    try {
                        std::string domain = info->url;
                        auto spos = domain.find("://");
                        if (spos != std::string::npos) { domain = domain.substr(spos + 3); auto epos = domain.find('/'); if (epos != std::string::npos) domain = domain.substr(0, epos); }
                        std::string data = ReadFileJson(GetZoomPath());
                        if (data == "[]") data = "{}";
                        std::string search = "\"" + domain + "\"";
                        auto p = data.find(search);
                        if (p != std::string::npos) {
                            auto colon = data.find(':', p); auto vend = data.find_first_of(",}", colon + 1);
                            data.replace(colon + 1, vend - colon - 1, "0.0");
                        }
                        WriteFileJson(GetZoomPath(), data);
                    } catch (...) {}
                }
            }
        }
        if (browser && browser->GetMainFrame())
            browser->GetHost()->SetZoomLevel(0.0);
        callback->Success("{}");
        return true;
    }

    if (cmd == "getZoom") {
        std::string domain = GetJsonString(json, "domain");
        if (!domain.empty()) {
            std::string data = ReadFileJson(GetZoomPath());
            if (data != "[]" && data != "{}") {
                std::string search = "\"" + domain + "\":";
                auto pos = data.find(search);
                if (pos != std::string::npos) {
                    auto vstart = pos + search.length();
                    auto vend = data.find_first_of(",}", vstart);
                    std::string val = data.substr(vstart, vend - vstart);
                    callback->Success("{\"zoom\":" + val + "}");
                    return true;
                }
            }
        }
        callback->Success("{\"zoom\":0}");
        return true;
    }

    // ─── Print ─────────────────────────────────────────────

    if (cmd == "print") {
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0) {
            auto b = tab_manager_->GetBrowser(active);
            if (b) b->GetHost()->Print();
        }
        callback->Success("{}");
        return true;
    }

    // ─── Fullscreen ────────────────────────────────────────

    if (cmd == "fullscreen") {
        if (window_) {
            GtkWidget* win = window_->GetMainWindow();
            if (win) {
                GdkWindow* gdk_win = gtk_widget_get_window(win);
                GdkWindowState state = gdk_window_get_state(gdk_win);
                if (state & GDK_WINDOW_STATE_FULLSCREEN) {
                    gtk_window_unfullscreen(GTK_WINDOW(win));
                } else {
                    gtk_window_fullscreen(GTK_WINDOW(win));
                }
            }
        }
        callback->Success("{}");
        return true;
    }

    // ─── New Incognito Tab ─────────────────────────────────

    if (cmd == "newIncognitoTab") {
        std::string url = "about:blank";
        int tab_id = tab_manager_->CreateIncognitoTab(url);
        if (window_) window_->CreateTabBrowser(tab_id, url, true);
        callback->Success("{\"tabId\":" + std::to_string(tab_id) + "}");
        return true;
    }

    if (cmd == "setDownloadDir") {
        // Open GTK folder chooser dialog on main thread
        if (window_) {
            GtkWidget* win = window_->GetMainWindow();
            GtkWidget* dialog = gtk_file_chooser_dialog_new(
                "Choose Download Directory",
                GTK_WINDOW(win),
                GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                "_Cancel", GTK_RESPONSE_CANCEL,
                "_Select", GTK_RESPONSE_ACCEPT,
                nullptr);

            // Set current download dir as default
            std::string settings = ReadFileJson(GetSettingsPath());
            if (settings != "[]" && settings != "{}") {
                std::string current = GetJsonString(settings, "downloadDir");
                if (!current.empty()) {
                    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current.c_str());
                }
            }

            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (folder) {
                    // Save to settings
                    std::string data = ReadFileJson(GetSettingsPath());
                    if (data == "[]") data = "{}";
                    std::string search = "\"downloadDir\"";
                    auto pos = data.find(search);
                    if (pos != std::string::npos) {
                        auto colon = data.find(':', pos);
                        auto vstart = colon + 1;
                        while (vstart < data.size() && data[vstart] == ' ') vstart++;
                        auto vend = data.find_first_of(",}", vstart);
                        if (data[vstart] == '"') { vend = data.find('"', vstart + 1); if (vend != std::string::npos) vend++; }
                        data.replace(vstart, vend - vstart, "\"" + JsonEscape(folder) + "\"");
                    } else {
                        if (data.back() == '}') {
                            data.pop_back();
                            if (data.size() > 1) data += ",";
                            data += "\"downloadDir\":\"" + JsonEscape(folder) + "\"}";
                        }
                    }
                    WriteFileJson(GetSettingsPath(), data);
                    callback->Success("{\"dir\":\"" + JsonEscape(folder) + "\"}");
                    g_free(folder);
                } else {
                    callback->Success("{}");
                }
            } else {
                callback->Success("{}");
            }
            gtk_widget_destroy(dialog);
        } else {
            callback->Failure(1, "No window");
        }
        return true;
    }

    callback->Failure(404, "Unknown command: " + cmd);
    return true;
}

void OrbQueryHandler::OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       int64_t query_id) {
    // No persistent queries to cancel
}
