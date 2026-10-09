#include "browser/browser_client.h"
#include "browser/browser_window.h"
#include "browser/js_escape.h"
#include "browser/query_handler.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_helpers.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <fstream>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <chrono>
#include <gtk/gtk.h>

namespace {

int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string ExtractDomain(const std::string& url) {
    auto spos = url.find("://");
    if (spos == std::string::npos) return "";
    std::string domain = url.substr(spos + 3);
    auto epos = domain.find('/');
    if (epos != std::string::npos) domain = domain.substr(0, epos);
    return domain;
}

double GetSavedZoom(const std::string& domain) {
    if (domain.empty()) return 0.0;
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string path = std::string(home) + "/.config/orb-browser/zoom.json";
    std::ifstream f(path);
    if (!f.good()) return 0.0;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    if (data.empty() || data == "[]" || data == "{}") return 0.0;
    std::string search = "\"" + domain + "\":";
    auto pos = data.find(search);
    if (pos == std::string::npos) return 0.0;
    auto vstart = pos + search.length();
    auto vend = data.find_first_of(",}", vstart);
    std::string val = data.substr(vstart, vend - vstart);
    try { return std::stod(val); }
    catch (...) { return 0.0; }
}

void SaveDomainZoom(const std::string& domain, double zoom) {
    if (domain.empty()) return;
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string config = std::string(home) + "/.config";
    mkdir(config.c_str(), 0755);
    std::string dir = config + "/orb-browser";
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/zoom.json";

    std::string data;
    {
        std::ifstream f(path);
        if (f.good()) {
            std::stringstream ss;
            ss << f.rdbuf();
            data = ss.str();
        }
    }
    if (data.empty() || data == "[]") data = "{}";

    std::string search = "\"" + domain + "\"";
    auto pos = data.find(search);
    if (pos != std::string::npos) {
        auto colon = data.find(':', pos);
        auto vend = data.find_first_of(",}", colon + 1);
        data.replace(colon + 1, vend - colon - 1, std::to_string(zoom));
    } else {
        if (data.back() == '}') {
            data.pop_back();
            if (data.size() > 1) data += ",";
            data += "\"" + domain + "\":" + std::to_string(zoom) + "}";
        }
    }
    std::ofstream f(path);
    f << data;
}

// Keep only a file name: no directory components, no traversal, no control
// characters, and no leading dot (a hostile name must not land on a dotfile).
std::string SanitizeDownloadName(const std::string& suggested) {
    std::string name = suggested;
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '/' || c == '\\') continue;
        out += c;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) out = "download";
    if (out.size() > 200) out.resize(200);
    return out;
}

// The download directory is user state, never page state: it must be an
// existing directory inside $HOME. Anything else falls back to ~/Downloads.
std::string ResolveDownloadDir(const std::string& configured, const std::string& home) {
    const std::string fallback = home + "/Downloads";
    if (configured.empty() || configured[0] != '/') return fallback;

    char resolved[PATH_MAX];
    char home_resolved[PATH_MAX];
    if (!realpath(configured.c_str(), resolved)) return fallback;
    if (!realpath(home.c_str(), home_resolved)) return fallback;

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return fallback;

    const std::string dir(resolved);
    const std::string root(home_resolved);
    if (dir != root && dir.rfind(root + "/", 0) != 0) return fallback;
    return dir;
}

// Never silently overwrite an existing file.
std::string UniqueDownloadPath(const std::string& dir, const std::string& name) {
    struct stat st;
    std::string path = dir + "/" + name;
    if (stat(path.c_str(), &st) != 0) return path;

    std::string stem = name, ext;
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int i = 1; i < 1000; i++) {
        std::string candidate = dir + "/" + stem + " (" + std::to_string(i) + ")" + ext;
        if (stat(candidate.c_str(), &st) != 0) return candidate;
    }
    return path;
}

}  // namespace

BrowserClient::BrowserClient(TabManager* tab_manager,
                             BrowserWindow* window,
                             std::shared_ptr<FilterRuleSet> filter_rules)
    : tab_manager_(tab_manager)
    , window_(window)
    , request_filter_(new RequestFilter(filter_rules))
{
    CefMessageRouterConfig config;
    config.js_query_function = "cefQuery";
    config.js_cancel_function = "cefQueryCancel";
    message_router_ = CefMessageRouterBrowserSide::Create(config);
    message_router_->AddHandler(new OrbQueryHandler(tab_manager_, window_), false);
}

void BrowserClient::ArmClipboardToken(int browser_id) {
    CEF_REQUIRE_UI_THREAD();
    clipboard_token_browser_ = browser_id;
    clipboard_token_deadline_ms_ = NowMillis() + 1000;
}

bool BrowserClient::ConsumeClipboardToken(int browser_id) {
    CEF_REQUIRE_UI_THREAD();
    if (clipboard_token_browser_ != browser_id) return false;
    if (NowMillis() > clipboard_token_deadline_ms_) return false;
    clipboard_token_browser_ = -1;
    return true;
}

bool BrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                             CefRefPtr<CefFrame> frame,
                                             CefProcessId source_process,
                                             CefRefPtr<CefProcessMessage> message) {
    return message_router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  int popup_id,
                                  const CefString& target_url,
                                  const CefString& target_frame_name,
                                  CefLifeSpanHandler::WindowOpenDisposition target_disposition,
                                  bool user_gesture,
                                  const CefPopupFeatures& popup_features,
                                  CefWindowInfo& window_info,
                                  CefRefPtr<CefClient>& client,
                                  CefBrowserSettings& settings,
                                  CefRefPtr<CefDictionaryValue>& extra_info,
                                  bool* no_javascript_access) {
    // Intercept popups — open in a new tab instead
    std::string url = target_url.ToString();
    if (!url.empty() && url != "about:blank") {
        int tab_id = tab_manager_->CreateTab(url);
        if (window_) window_->CreateTabBrowser(tab_id, url);
    }
    return true;  // Cancel the popup
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    browser_count_++;
    if (window_) {
        window_->OnBrowserCreated(browser);
    }
}

bool BrowserClient::DoClose(CefRefPtr<CefBrowser> browser) {
    return false;
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    message_router_->OnBeforeClose(browser);
    browser_count_--;

    // Drop every reference the window still holds, otherwise later input is
    // routed to a host that no longer exists.
    if (window_) window_->OnBrowserClosed(browser);

    // Notify tab manager
    tab_manager_->OnBrowserClosed(browser->GetIdentifier());

    if (browser_count_ <= 0) {
        CefQuitMessageLoop();
    }
}

void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    const CefString& url) {
    CEF_REQUIRE_UI_THREAD();
    if (!frame->IsMain()) return;

    int tab_id = tab_manager_->GetTabIdByBrowserId(browser->GetIdentifier());
    if (tab_id < 0) return;

    tab_manager_->UpdateTabUrl(tab_id, url.ToString());

    // Record in history
    std::string url_str = url.ToString();
    if (url_str.find("orb://") != 0) {
        const TabInfo* info = tab_manager_->GetTab(tab_id);
        std::string title = info ? info->title : "";
        NotifySidebar("if(typeof orb !== 'undefined' && typeof sendCommand === 'function') "
                      "sendCommand({cmd:'historyAdd',url:'" + JsEscape(url_str) + "',title:'" + JsEscape(title) + "'});");
    }

    if (chrome_browser_ && chrome_browser_->GetMainFrame()) {
        std::string js = "if(typeof orb !== 'undefined') orb.onAddressChange(" +
                         std::to_string(tab_id) + ",'" + JsEscape(url.ToString()) + "');";
        chrome_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }

    // Restore per-domain zoom level
    std::string domain = ExtractDomain(url_str);
    double zoom = GetSavedZoom(domain);
    browser->GetHost()->SetZoomLevel(zoom);
}

void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    int tab_id = tab_manager_->GetTabIdByBrowserId(browser->GetIdentifier());
    if (tab_id < 0) return;

    tab_manager_->UpdateTabTitle(tab_id, title.ToString());

    if (chrome_browser_ && chrome_browser_->GetMainFrame()) {
        std::string js = "if(typeof orb !== 'undefined') orb.onTitleChange(" +
                         std::to_string(tab_id) + ",'" + JsEscape(title.ToString()) + "');";
        chrome_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }
}

bool BrowserClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                     cef_log_severity_t level,
                                     const CefString& message,
                                     const CefString& source,
                                     int line) {
    std::string prefix;
    switch (level) {
        case LOGSEVERITY_WARNING: prefix = "WARN"; break;
        case LOGSEVERITY_ERROR:   prefix = "ERROR"; break;
        default:                  prefix = "LOG"; break;
    }
    fprintf(stderr, "[JS %s] %s (%s:%d)\n", prefix.c_str(),
            message.ToString().c_str(), source.ToString().c_str(), line);
    return false;
}

void BrowserClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                                           bool fullscreen) {
    if (!window_) return;
    GtkWidget* win = window_->GetMainWindow();
    if (!win) return;

    if (fullscreen) {
        // Hide sidebar and titlebar, go fullscreen
        window_->sidebar_visible_ = false;
        BrowserWindow::kSidebarWidth = 0;
        gtk_window_fullscreen(GTK_WINDOW(win));
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(win));
        if (window_->sidebar_pinned_) {
            window_->sidebar_visible_ = true;
            BrowserWindow::kSidebarWidth = BrowserWindow::kDefaultSidebarWidth;
        }
    }
    window_->ForceResize();
}

CefRefPtr<CefResourceRequestHandler> BrowserClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool is_navigation,
    bool is_download,
    const CefString& request_initiator,
    bool& disable_default_handling) {
    return request_filter_;
}

// Pasting must not spin a nested GTK loop on the CEF UI thread: ask for the
// selection asynchronously and inject it when the owner answers.
namespace {

struct PasteRequest {
    CefRefPtr<CefBrowser> browser;
};

void OnClipboardTextReceived(GtkClipboard* clipboard, const gchar* text, gpointer data) {
    std::unique_ptr<PasteRequest> req(static_cast<PasteRequest*>(data));
    if (!text || !req->browser) return;

    auto focused = req->browser->GetFocusedFrame();
    if (!focused) return;

    const std::string escaped = JsEscape(text);
    focused->ExecuteJavaScript(
        "(function(){"
        "  var el = document.activeElement;"
        "  var txt = '" + escaped + "';"
        "  if(el && (el.tagName==='INPUT'||el.tagName==='TEXTAREA')){"
        "    var start=el.selectionStart||0, end=el.selectionEnd||0;"
        "    el.value = el.value.substring(0,start) + txt + el.value.substring(end);"
        "    el.selectionStart = el.selectionEnd = start + txt.length;"
        "    el.dispatchEvent(new Event('input',{bubbles:true}));"
        "  } else if(el && el.isContentEditable){"
        "    document.execCommand('insertText',false,txt);"
        "  }"
        "})();",
        "", 0);
}

}  // namespace

bool BrowserClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                  const CefKeyEvent& event,
                                  CefEventHandle os_event,
                                  bool* is_keyboard_shortcut) {
    if (event.type != KEYEVENT_RAWKEYDOWN) return false;

    bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
    bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
    bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;

    // Alt+D: Show floating URL bar on active tab
    if (alt && event.windows_key_code == 'D') {
        ShowFloatingUrlBar();
        return true;
    }

    // F11: Fullscreen toggle
    if (event.windows_key_code == 0x7A) {  // VK_F11
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
        return true;
    }

    // F12: DevTools
    if (event.windows_key_code == 0x7B) {  // VK_F12 = 0x7B (123)
        int active = tab_manager_->GetActiveTabId();
        if (active >= 0) {
            auto tab_browser = tab_manager_->GetBrowser(active);
            if (tab_browser) {
                CefWindowInfo devtools_info;
                devtools_info.SetAsChild(0, CefRect(0, 0, 800, 600));
                CefBrowserSettings devtools_settings;
                tab_browser->GetHost()->ShowDevTools(devtools_info, nullptr,
                    devtools_settings, CefPoint());
            }
        }
        return true;
    }

    // Ctrl+F: Find in page
    if (ctrl && event.windows_key_code == 'F') {
        NotifySidebar("if(typeof orb !== 'undefined') orb.showFindBar();");
        return true;
    }

    // Escape: Close find bar
    if (event.windows_key_code == 0x1B) {
        NotifySidebar("if(typeof orb !== 'undefined') orb.hideFindBar();");
    }

    if (ctrl) {
        // Zoom controls — only zoom tab content, never the sidebar
        if (event.windows_key_code == 0xBB || event.windows_key_code == 0x6B) {
            // Ctrl+= or Ctrl+Numpad+ : Zoom in
            int active = tab_manager_->GetActiveTabId();
            if (active >= 0) {
                auto b = tab_manager_->GetBrowser(active);
                if (b) {
                    double lvl = b->GetHost()->GetZoomLevel() + 0.5;
                    b->GetHost()->SetZoomLevel(lvl);
                    // Persist per-domain zoom
                    const TabInfo* info = tab_manager_->GetTab(active);
                    if (info) SaveDomainZoom(ExtractDomain(info->url), lvl);
                }
            }
            if (chrome_browser_) chrome_browser_->GetHost()->SetZoomLevel(0.0);
            return true;
        }
        if (event.windows_key_code == 0xBD || event.windows_key_code == 0x6D) {
            // Ctrl+- or Ctrl+Numpad- : Zoom out
            int active = tab_manager_->GetActiveTabId();
            if (active >= 0) {
                auto b = tab_manager_->GetBrowser(active);
                if (b) {
                    double lvl = b->GetHost()->GetZoomLevel() - 0.5;
                    b->GetHost()->SetZoomLevel(lvl);
                    const TabInfo* info = tab_manager_->GetTab(active);
                    if (info) SaveDomainZoom(ExtractDomain(info->url), lvl);
                }
            }
            if (chrome_browser_) chrome_browser_->GetHost()->SetZoomLevel(0.0);
            return true;
        }
        if (event.windows_key_code == '0') {
            // Ctrl+0 : Reset zoom
            int active = tab_manager_->GetActiveTabId();
            if (active >= 0) {
                auto b = tab_manager_->GetBrowser(active);
                if (b) {
                    b->GetHost()->SetZoomLevel(0.0);
                    const TabInfo* info = tab_manager_->GetTab(active);
                    if (info) SaveDomainZoom(ExtractDomain(info->url), 0.0);
                }
            }
            if (chrome_browser_) chrome_browser_->GetHost()->SetZoomLevel(0.0);
            return true;
        }

        // Ctrl+P: Print
        if (event.windows_key_code == 'P') {
            int active = tab_manager_->GetActiveTabId();
            if (active >= 0) {
                auto b = tab_manager_->GetBrowser(active);
                if (b) b->GetHost()->Print();
            }
            return true;
        }

        // Ctrl+H: Open history page
        if (event.windows_key_code == 'H') {
            NotifySidebar("if(typeof sendCommand === 'function') sendCommand({cmd:'newTab',url:'orb://history/'});");
            return true;
        }

        // Ctrl+Shift+Tab: Previous tab
        if (shift && event.windows_key_code == 0x09) {
            tab_manager_->SwitchToPrevTab();
            return true;
        }

        // Ctrl+1 through Ctrl+9: Switch to tab N
        if (event.windows_key_code >= '1' && event.windows_key_code <= '9') {
            int idx = event.windows_key_code - '1';
            auto ids = tab_manager_->GetTabIds();
            if (event.windows_key_code == '9') {
                // Ctrl+9 = last tab
                if (!ids.empty()) tab_manager_->SwitchTab(ids.back());
            } else if (idx < (int)ids.size()) {
                tab_manager_->SwitchTab(ids[idx]);
            }
            return true;
        }

        switch (event.windows_key_code) {
            case 'C': {  // Ctrl+C: Copy to system clipboard
                auto focused = browser->GetFocusedFrame();
                if (!focused) return false;
                // The page itself hands the selection back through cefQuery;
                // allow exactly one such call for this user-initiated copy.
                ArmClipboardToken(browser->GetIdentifier());
                focused->ExecuteJavaScript(
                    "(function(){"
                    "  var s = window.getSelection().toString();"
                    "  if(!s) {"
                    "    var el = document.activeElement;"
                    "    if(el && (el.tagName==='INPUT'||el.tagName==='TEXTAREA') && "
                    "       typeof el.selectionStart==='number' && el.selectionStart !== el.selectionEnd)"
                    "      s = el.value.substring(el.selectionStart, el.selectionEnd);"
                    "  }"
                    "  if(s && typeof cefQuery==='function') cefQuery({request:JSON.stringify({cmd:'clipboardWrite',text:s})});"
                    "})();",
                    "", 0);
                focused->Copy();
                return false;
            }
            case 'V': {  // Ctrl+V: Paste from system clipboard
                GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                gtk_clipboard_request_text(clip, OnClipboardTextReceived,
                                           new PasteRequest{browser});
                return true;
            }
            case 'X': {  // Ctrl+X: Cut to system clipboard
                auto focused = browser->GetFocusedFrame();
                if (!focused) return false;
                ArmClipboardToken(browser->GetIdentifier());
                focused->ExecuteJavaScript(
                    "(function(){"
                    "  var s = window.getSelection().toString();"
                    "  if(!s) {"
                    "    var el = document.activeElement;"
                    "    if(el && (el.tagName==='INPUT'||el.tagName==='TEXTAREA') && "
                    "       typeof el.selectionStart==='number' && el.selectionStart !== el.selectionEnd)"
                    "      s = el.value.substring(el.selectionStart, el.selectionEnd);"
                    "  }"
                    "  if(s && typeof cefQuery==='function') cefQuery({request:JSON.stringify({cmd:'clipboardWrite',text:s})});"
                    "})();",
                    "", 0);
                focused->Cut();
                return false;
            }
            case 'A': {  // Ctrl+A: Select All
                auto focused = browser->GetFocusedFrame();
                if (focused) focused->SelectAll();
                return false;
            }
            case 'T': {  // Ctrl+T: New tab (let sidebar JS decide URL)
                NotifySidebar("if(typeof orb !== 'undefined' && typeof sendCommand === 'function') "
                              "sendCommand({cmd:'newTab',url:getNewTabUrl()});");
                return true;
            }
            case 'W':  // Ctrl+W: Close tab
                tab_manager_->CloseActiveTab();
                return true;
            case 'L':  // Ctrl+L: Show floating URL bar
                ShowFloatingUrlBar();
                return true;
            case 'D':  // Ctrl+D: Toggle bookmark
                NotifySidebar("if(typeof orb !== 'undefined') orb.toggleBookmark();");
                return true;
            case 'B':  // Ctrl+B: Toggle compact mode
                NotifySidebar("if(typeof toggleCompactMode==='function') toggleCompactMode();");
                return true;
            case 'S':  // Ctrl+S: Toggle sidebar pin / Ctrl+Shift+S: Close split view
                if (shift) {
                    if (window_) window_->CloseSplitView();
                } else {
                    if (window_) window_->ToggleSidebar();
                }
                return true;
            case 0x09:  // Ctrl+Tab: Next tab (without shift)
                if (!shift) {
                    tab_manager_->SwitchToNextTab();
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

// ─── CefLoadHandler ───────────────────────────────────────

void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                         bool isLoading,
                                         bool canGoBack,
                                         bool canGoForward) {
    CEF_REQUIRE_UI_THREAD();
    int tab_id = tab_manager_->GetTabIdByBrowserId(browser->GetIdentifier());
    if (tab_id < 0) return;

    if (chrome_browser_ && chrome_browser_->GetMainFrame()) {
        std::string js = "if(typeof orb !== 'undefined') orb.onLoadingStateChange(" +
                         std::to_string(tab_id) + "," +
                         (isLoading ? "true" : "false") + "," +
                         (canGoBack ? "true" : "false") + "," +
                         (canGoForward ? "true" : "false") + ");";
        chrome_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }
}

// ─── CefContextMenuHandler ────────────────────────────────

void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefContextMenuParams> params,
                                        CefRefPtr<CefMenuModel> model) {
    CEF_REQUIRE_UI_THREAD();

    // Skip context menu for sidebar browser
    if (chrome_browser_ && browser->GetIdentifier() == chrome_browser_->GetIdentifier()) {
        model->Clear();
        return;
    }

    // Clear default menu, build our own
    model->Clear();

    // Navigation
    model->AddItem(MENU_ID_BACK, "Back");
    model->AddItem(MENU_ID_FORWARD, "Forward");
    model->AddItem(MENU_ID_RELOAD, "Reload");
    model->AddSeparator();

    // Link items
    if (!params->GetLinkUrl().empty()) {
        model->AddItem(ORB_MENU_OPEN_LINK_NEW_TAB, "Open Link in New Tab");
        model->AddItem(ORB_MENU_COPY_LINK, "Copy Link Address");
        model->AddSeparator();
    }

    // Image items
    if (params->GetMediaType() == CM_MEDIATYPE_IMAGE) {
        model->AddItem(ORB_MENU_COPY_IMAGE_URL, "Copy Image URL");
        model->AddSeparator();
    }

    // Edit items (when right-clicking on editable content)
    if (params->IsEditable()) {
        model->AddItem(MENU_ID_UNDO, "Undo");
        model->AddItem(MENU_ID_REDO, "Redo");
        model->AddSeparator();
        model->AddItem(MENU_ID_CUT, "Cut");
        model->AddItem(MENU_ID_COPY, "Copy");
        model->AddItem(MENU_ID_PASTE, "Paste");
        model->AddItem(MENU_ID_SELECT_ALL, "Select All");
        model->AddSeparator();
    } else {
        // Selection items
        if (!params->GetSelectionText().empty()) {
            model->AddItem(MENU_ID_COPY, "Copy");
            model->AddSeparator();
        }
        model->AddItem(MENU_ID_SELECT_ALL, "Select All");
        model->AddSeparator();
    }

    // DevTools
    model->AddItem(ORB_MENU_INSPECT, "Inspect Element");
}

// Context for GTK popup menu
struct GtkMenuContext {
    CefRefPtr<CefRunContextMenuCallback> callback;
    GtkWidget* menu = nullptr;
    bool handled = false;  // a command was chosen
    bool closed = false;   // cleanup already scheduled
};

static gboolean FinishGtkMenu(gpointer data) {
    auto* ctx = static_cast<GtkMenuContext*>(data);
    if (!ctx->handled) ctx->callback->Cancel();
    if (ctx->menu) {
        gtk_widget_destroy(ctx->menu);
        g_object_unref(ctx->menu);
    }
    delete ctx;
    return G_SOURCE_REMOVE;
}

static void OnGtkMenuItem(GtkWidget* widget, gpointer data) {
    auto* ctx = static_cast<GtkMenuContext*>(data);
    int cmd_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "cmd_id"));
    if (!ctx->handled) {
        ctx->handled = true;
        ctx->callback->Continue(cmd_id, EVENTFLAG_NONE);
    }
}

static void OnGtkMenuDeactivate(GtkMenuShell* shell, gpointer data) {
    auto* ctx = static_cast<GtkMenuContext*>(data);
    // GTK can emit "deactivate" more than once for the same shell; the old
    // code deleted the context on each one.
    if (ctx->closed) return;
    ctx->closed = true;
    // Idle so that a menu item's "activate" is delivered first.
    g_idle_add(FinishGtkMenu, ctx);
}

bool BrowserClient::RunContextMenu(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefContextMenuParams> params,
                                   CefRefPtr<CefMenuModel> model,
                                   CefRefPtr<CefRunContextMenuCallback> callback) {
    CEF_REQUIRE_UI_THREAD();

    if (model->GetCount() == 0) {
        callback->Cancel();
        return true;
    }

    auto* ctx = new GtkMenuContext{callback, nullptr, false, false};
    GtkWidget* menu = gtk_menu_new();
    g_object_ref_sink(menu);
    ctx->menu = menu;

    for (size_t i = 0; i < model->GetCount(); i++) {
        cef_menu_item_type_t type = model->GetTypeAt(i);

        if (type == MENUITEMTYPE_SEPARATOR) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            continue;
        }

        int cmd_id = model->GetCommandIdAt(i);
        std::string label_str = model->GetLabelAt(i).ToString();

        GtkWidget* item = gtk_menu_item_new_with_label(label_str.c_str());
        if (!model->IsEnabledAt(i)) {
            gtk_widget_set_sensitive(item, FALSE);
        }

        g_object_set_data(G_OBJECT(item), "cmd_id", GINT_TO_POINTER(cmd_id));
        g_signal_connect(item, "activate", G_CALLBACK(OnGtkMenuItem), ctx);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    g_signal_connect(menu, "deactivate", G_CALLBACK(OnGtkMenuDeactivate), ctx);
    gtk_widget_show_all(menu);

    // params coordinates are relative to the browser view; the popup rectangle
    // is relative to the window's GdkWindow, so only our own chrome offsets
    // have to be added.
    GtkWidget* main_win = window_ ? window_->GetMainWindow() : nullptr;
    GdkWindow* gdk_win = main_win ? gtk_widget_get_window(main_win) : nullptr;
    if (gdk_win) {
        int sidebar_offset = 0;
        if (!chrome_browser_ || browser->GetIdentifier() != chrome_browser_->GetIdentifier()) {
            sidebar_offset = BrowserWindow::kSidebarWidth;
        }
        GdkRectangle rect;
        rect.x = params->GetXCoord() + sidebar_offset;
        rect.y = params->GetYCoord() + BrowserWindow::kTitleBarHeight;
        rect.width = 1;
        rect.height = 1;

        gtk_menu_attach_to_widget(GTK_MENU(menu), main_win, nullptr);
        gtk_menu_popup_at_rect(GTK_MENU(menu), gdk_win, &rect,
                               GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_WEST, nullptr);
        return true;
    }

    gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
    return true;
}

bool BrowserClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         CefRefPtr<CefContextMenuParams> params,
                                         int command_id,
                                         cef_event_flags_t event_flags) {
    CEF_REQUIRE_UI_THREAD();

    switch (command_id) {
        case ORB_MENU_OPEN_LINK_NEW_TAB: {
            std::string url = params->GetLinkUrl().ToString();
            if (!url.empty()) {
                int tab_id = tab_manager_->CreateTab(url);
                if (window_) window_->CreateTabBrowser(tab_id, url);
            }
            return true;
        }
        case ORB_MENU_COPY_LINK: {
            std::string url = params->GetUnfilteredLinkUrl().ToString();
            if (!url.empty() && frame) {
                frame->ExecuteJavaScript(
                    "navigator.clipboard.writeText('" + JsEscape(url) + "');", "", 0);
            }
            return true;
        }
        case ORB_MENU_COPY_IMAGE_URL: {
            std::string url = params->GetSourceUrl().ToString();
            if (!url.empty() && frame) {
                frame->ExecuteJavaScript(
                    "navigator.clipboard.writeText('" + JsEscape(url) + "');", "", 0);
            }
            return true;
        }
        case ORB_MENU_INSPECT: {
            CefWindowInfo devtools_info;
            devtools_info.SetAsChild(0, CefRect(0, 0, 800, 600));
            CefBrowserSettings devtools_settings;
            CefPoint inspect_point(params->GetXCoord(), params->GetYCoord());
            browser->GetHost()->ShowDevTools(devtools_info, nullptr,
                devtools_settings, inspect_point);
            return true;
        }
        default:
            break;
    }
    return false;  // Let CEF handle built-in commands (copy/paste/undo/etc)
}

// ─── CefFindHandler ───────────────────────────────────────

void BrowserClient::OnFindResult(CefRefPtr<CefBrowser> browser,
                                 int identifier,
                                 int count,
                                 const CefRect& selectionRect,
                                 int activeMatchOrdinal,
                                 bool finalUpdate) {
    CEF_REQUIRE_UI_THREAD();
    if (!finalUpdate) return;

    if (chrome_browser_ && chrome_browser_->GetMainFrame()) {
        std::string js = "if(typeof orb !== 'undefined') orb.onFindResult(" +
                         std::to_string(activeMatchOrdinal) + "," +
                         std::to_string(count) + ");";
        chrome_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }
}

// ─── CefRenderHandler (OSR) ────────────────────────────────

void BrowserClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    if (window_) {
        window_->GetOsrRect(browser, rect);
    } else {
        rect.Set(0, 0, 800, 600);
    }
}

void BrowserClient::OnPaint(CefRefPtr<CefBrowser> browser,
                            PaintElementType type,
                            const RectList& dirtyRects,
                            const void* buffer,
                            int width, int height) {
    if (window_ && type == PET_VIEW) {
        window_->OnOsrPaint(browser, dirtyRects, buffer, width, height);
    }
}

bool BrowserClient::OnCursorChange(CefRefPtr<CefBrowser> browser,
                                   CefCursorHandle cursor,
                                   cef_cursor_type_t type,
                                   const CefCursorInfo& custom_cursor_info) {
    if (window_) {
        window_->SetCursor(type);
    }
    return true;
}

// ─── Helper ───────────────────────────────────────────────

void BrowserClient::NotifySidebar(const std::string& js) {
    if (chrome_browser_ && chrome_browser_->GetMainFrame()) {
        chrome_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }
}

void BrowserClient::ShowFloatingUrlBar() {
    int active = tab_manager_->GetActiveTabId();
    if (active < 0) return;
    auto browser = tab_manager_->GetBrowser(active);
    if (!browser || !browser->GetMainFrame()) return;

    // Get current URL to prefill. It is page-controlled, so it must be escaped
    // like every other value we splice into an ExecuteJavaScript payload.
    std::string current_url = JsEscape(browser->GetMainFrame()->GetURL().ToString());
    // Don't show internal URLs
    if (current_url.rfind("orb://", 0) == 0) current_url = "";

    std::string js = R"(
(function(){
    if (document.getElementById('__orb_floating_url')) {
        document.getElementById('__orb_floating_url_input').focus();
        document.getElementById('__orb_floating_url_input').select();
        return;
    }
    var overlay = document.createElement('div');
    overlay.id = '__orb_floating_url';
    overlay.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;z-index:2147483647;background:rgba(0,0,0,0.5);display:flex;align-items:flex-start;justify-content:center;padding-top:15vh;backdrop-filter:blur(4px);';
    var box = document.createElement('div');
    box.style.cssText = 'width:min(90%,700px);background:#1a1b26;border:1px solid rgba(255,255,255,0.15);border-radius:14px;box-shadow:0 30px 100px rgba(0,0,0,0.6);padding:6px;';
    var input = document.createElement('input');
    input.id = '__orb_floating_url_input';
    input.type = 'text';
    input.placeholder = 'Search or enter URL...';
    input.value = ')" + current_url + R"(';
    input.style.cssText = 'width:100%;height:48px;background:transparent;border:none;outline:none;color:#c0caf5;font-size:16px;font-family:inherit;padding:0 14px;';
    input.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') {
            var val = input.value.trim();
            if (val) {
                if (!/^https?:\/\//i.test(val) && !/^[a-z][a-z0-9+.-]*:/i.test(val)) {
                    if (/^[^\s]+\.[^\s]+$/.test(val)) val = 'https://' + val;
                    else val = 'https://www.google.com/search?q=' + encodeURIComponent(val);
                }
                window.location.href = val;
            }
            overlay.remove();
        }
        if (e.key === 'Escape') overlay.remove();
    });
    overlay.addEventListener('click', function(e) { if (e.target === overlay) overlay.remove(); });
    box.appendChild(input);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    input.focus();
    input.select();
})();
    )";
    browser->GetMainFrame()->ExecuteJavaScript(js, "orb://floating-url", 0);
}

// ─── CefDownloadHandler ───────────────────────────────────

bool BrowserClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefDownloadItem> download_item,
                                     const CefString& suggested_name,
                                     CefRefPtr<CefBeforeDownloadCallback> callback) {
    CEF_REQUIRE_UI_THREAD();

    // Read download dir from settings, fall back to ~/Downloads
    std::string configured_dir;
    const char* home_env = getenv("HOME");
    const std::string home = home_env ? home_env : "/tmp";
    std::string settings_path = home + "/.config/orb-browser/settings.json";
    {
        std::ifstream sf(settings_path);
        if (sf.good()) {
            std::stringstream ss;
            ss << sf.rdbuf();
            std::string data = ss.str();
            // Extract downloadDir value
            std::string search = "\"downloadDir\":\"";
            auto pos = data.find(search);
            if (pos != std::string::npos) {
                auto vstart = pos + search.length();
                auto vend = data.find('"', vstart);
                if (vend != std::string::npos) {
                    configured_dir = data.substr(vstart, vend - vstart);
                }
            }
        }
    }

    std::string dir = ResolveDownloadDir(configured_dir, home);
    mkdir(dir.c_str(), 0755);

    const std::string name = SanitizeDownloadName(suggested_name.ToString());
    const std::string path = UniqueDownloadPath(dir, name);
    callback->Continue(path, false);

    // Notify sidebar
    NotifySidebar("if(typeof orb !== 'undefined') orb.onDownloadStart(" +
                  std::to_string(download_item->GetId()) + ",'" + JsEscape(name) + "');");
    return true;
}

void BrowserClient::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefDownloadItem> download_item,
                                      CefRefPtr<CefDownloadItemCallback> callback) {
    CEF_REQUIRE_UI_THREAD();

    int id = download_item->GetId();
    int percent = download_item->GetPercentComplete();
    bool complete = download_item->IsComplete();
    bool canceled = download_item->IsCanceled();

    if (complete || canceled) {
        NotifySidebar("if(typeof orb !== 'undefined') orb.onDownloadEnd(" +
                      std::to_string(id) + "," +
                      (complete ? "true" : "false") + ");");
    } else {
        NotifySidebar("if(typeof orb !== 'undefined') orb.onDownloadProgress(" +
                      std::to_string(id) + "," + std::to_string(percent) + ");");
    }
}
