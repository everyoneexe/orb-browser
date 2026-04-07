#pragma once

#include "browser/tab_manager.h"
#include "blocker/filter_rules.h"
#include "include/cef_browser.h"
#include "include/cef_render_handler.h"

#include <gtk/gtk.h>
#include <cairo/cairo.h>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

class BrowserClient;

class BrowserWindow {
public:
    static BrowserWindow* Create(std::shared_ptr<FilterRuleSet> filter_rules);
    static BrowserWindow* GetInstance() { return instance_; }

    void CreateTabBrowser(int tab_id, const std::string& url, bool incognito = false);
    void ShowTab(int tab_id);
    void Close();
    void ForceResize();
    void ToggleSidebar();
    void SetSidebarPinned(bool pinned);
    void ShowSidebarAnimated();
    void HideSidebarAnimated();

    TabManager* GetTabManager() { return &tab_manager_; }
    CefRefPtr<BrowserClient> GetClient();
    void OnBrowserCreated(CefRefPtr<CefBrowser> browser);
    GtkWidget* GetMainWindow() { return main_window_; }

    // OSR interface — called from BrowserClient's CefRenderHandler
    void GetOsrRect(CefRefPtr<CefBrowser> browser, CefRect& rect);
    void OnOsrPaint(CefRefPtr<CefBrowser> browser,
                    const std::vector<CefRect>& dirtyRects,
                    const void* buffer,
                    int width, int height);

    static const int kDefaultSidebarWidth = 280;
    static const int kTitleBarHeight = 32;
    static int kSidebarWidth;
    bool sidebar_visible_ = true;
    bool sidebar_pinned_ = true;
    bool sidebar_hovered_ = false;
    guint sidebar_hide_timer_ = 0;
    int width_ = 1400;
    int height_ = 900;

    // Titlebar button hover state
    int titlebar_hover_btn_ = -1;  // -1=none, 0=minimize, 1=maximize, 2=close
    bool titlebar_dragging_ = false;
    int drag_start_x_ = 0, drag_start_y_ = 0;
    int win_start_x_ = 0, win_start_y_ = 0;

private:
    BrowserWindow();
    void CreateFirstTab();

    friend class FirstTabTask;

    // GTK callbacks
    static gboolean OnDraw(GtkWidget* widget, cairo_t* cr, gpointer data);
    static gboolean OnButtonPress(GtkWidget* widget, GdkEventButton* event, gpointer data);
    static gboolean OnButtonRelease(GtkWidget* widget, GdkEventButton* event, gpointer data);
    static gboolean OnMotionNotify(GtkWidget* widget, GdkEventMotion* event, gpointer data);
    static gboolean OnScroll(GtkWidget* widget, GdkEventScroll* event, gpointer data);
    static gboolean OnKeyPress(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static gboolean OnKeyRelease(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static gboolean OnFocusIn(GtkWidget* widget, GdkEventFocus* event, gpointer data);
    static gboolean OnFocusOut(GtkWidget* widget, GdkEventFocus* event, gpointer data);
    static void OnDestroy(GtkWidget* widget, gpointer data);
    static void OnSizeAllocate(GtkWidget* widget, GdkRectangle* alloc, gpointer data);

    // Input helpers
    CefRefPtr<CefBrowser> GetBrowserAtX(int x);
    int AdjustX(int x, CefRefPtr<CefBrowser> browser);
    uint32_t GetCefModifiers(guint state);
    CefKeyEvent TranslateKeyEvent(GdkEventKey* event);

    static BrowserWindow* instance_;

    TabManager tab_manager_;
    CefRefPtr<BrowserClient> client_;
    std::shared_ptr<FilterRuleSet> filter_rules_;

    GtkWidget* main_window_ = nullptr;
    GtkWidget* drawing_area_ = nullptr;

    CefRefPtr<CefBrowser> sidebar_browser_;
    bool sidebar_created_ = false;
    std::queue<int> pending_tab_ids_;

    // OSR surfaces — protected by paint_mutex_
    std::mutex paint_mutex_;
    cairo_surface_t* sidebar_surface_ = nullptr;
    cairo_surface_t* tab_surface_ = nullptr;
    int sidebar_buf_w_ = 0, sidebar_buf_h_ = 0;
    int tab_buf_w_ = 0, tab_buf_h_ = 0;

    // Track which browser has keyboard focus
    CefRefPtr<CefBrowser> focused_browser_;
};
