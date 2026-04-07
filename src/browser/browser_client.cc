#include "browser/browser_client.h"
#include "browser/browser_window.h"
#include "browser/query_handler.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_helpers.h"

#include <cstring>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <gtk/gtk.h>

namespace {
// Escape a string for safe insertion into a JS single-quoted string literal
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

bool BrowserClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                  const CefKeyEvent& event,
                                  CefEventHandle os_event,
                                  bool* is_keyboard_shortcut) {
    if (event.type != KEYEVENT_RAWKEYDOWN) return false;

    bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
    bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;

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
            case 'C': {  // Ctrl+C: Copy
                browser->GetFocusedFrame()->Copy();
                return false;  // Also let CEF process it
            }
            case 'V': {  // Ctrl+V: Paste
                browser->GetFocusedFrame()->Paste();
                return false;
            }
            case 'X': {  // Ctrl+X: Cut
                browser->GetFocusedFrame()->Cut();
                return false;
            }
            case 'A': {  // Ctrl+A: Select All
                browser->GetFocusedFrame()->SelectAll();
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
            case 'L':  // Ctrl+L: Focus address bar
                NotifySidebar("if(typeof orb !== 'undefined') orb.focusAddressBar();");
                return true;
            case 'D':  // Ctrl+D: Toggle bookmark
                NotifySidebar("if(typeof orb !== 'undefined') orb.toggleBookmark();");
                return true;
            case 'B':  // Ctrl+B: Toggle sidebar pin
            case 'S':  // Ctrl+S: Toggle sidebar pin
                if (window_) window_->ToggleSidebar();
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
    bool handled = false;
};

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
    // Use idle callback so menu item activate fires first
    g_idle_add([](gpointer data) -> gboolean {
        auto* ctx = static_cast<GtkMenuContext*>(data);
        if (!ctx->handled) {
            ctx->callback->Cancel();
        }
        delete ctx;
        return G_SOURCE_REMOVE;
    }, ctx);
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

    auto* ctx = new GtkMenuContext{callback, false};
    GtkWidget* menu = gtk_menu_new();

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

    // Use deprecated gtk_menu_popup which doesn't need a GdkEvent
    // The context menu coordinates are relative to the tab content area
    // We need to convert to screen coordinates
    GtkWidget* main_win = window_ ? window_->GetMainWindow() : nullptr;
    if (main_win) {
        GdkWindow* gdk_win = gtk_widget_get_window(main_win);
        if (gdk_win) {
            int win_x, win_y;
            gdk_window_get_origin(gdk_win, &win_x, &win_y);
            // params coordinates are relative to the browser view
            // For tab browsers, add sidebar width offset
            int sidebar_offset = 0;
            if (!chrome_browser_ || browser->GetIdentifier() != chrome_browser_->GetIdentifier()) {
                sidebar_offset = BrowserWindow::kSidebarWidth;
            }
            int menu_x = win_x + params->GetXCoord() + sidebar_offset;
            int menu_y = win_y + params->GetYCoord() + BrowserWindow::kTitleBarHeight;

            gtk_menu_popup(GTK_MENU(menu), nullptr, nullptr,
                [](GtkMenu* menu, gint* x, gint* y, gboolean* push_in, gpointer data) {
                    auto* coords = static_cast<std::pair<int,int>*>(data);
                    *x = coords->first;
                    *y = coords->second;
                    *push_in = TRUE;
                },
                new std::pair<int,int>(menu_x, menu_y),
                3,  // right mouse button
                gtk_get_current_event_time());
            return true;
        }
    }

    // Fallback: position at (0,0)
    gtk_menu_popup(GTK_MENU(menu), nullptr, nullptr, nullptr, nullptr,
                   3, gtk_get_current_event_time());
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

// ─── Helper ───────────────────────────────────────────────

void BrowserClient::NotifySidebar(const std::string& js) {
    if (chrome_browser_ && chrome_browser_->GetMainFrame()) {
        chrome_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }
}

// ─── CefDownloadHandler ───────────────────────────────────

bool BrowserClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefDownloadItem> download_item,
                                     const CefString& suggested_name,
                                     CefRefPtr<CefBeforeDownloadCallback> callback) {
    CEF_REQUIRE_UI_THREAD();

    // Read download dir from settings, fall back to ~/Downloads
    std::string dir;
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string settings_path = std::string(home) + "/.config/orb-browser/settings.json";
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
                    dir = data.substr(vstart, vend - vstart);
                }
            }
        }
    }
    if (dir.empty()) {
        dir = std::string(home) + "/Downloads";
    }
    if (dir.back() != '/') dir += '/';
    mkdir(dir.c_str(), 0755);
    std::string path = dir + suggested_name.ToString();
    callback->Continue(path, false);

    // Notify sidebar
    NotifySidebar("if(typeof orb !== 'undefined') orb.onDownloadStart(" +
                  std::to_string(download_item->GetId()) + ",'" + JsEscape(suggested_name.ToString()) + "');");
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
