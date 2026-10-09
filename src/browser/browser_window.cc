#include "browser/browser_window.h"
#include "browser/browser_client.h"
#include "include/cef_app.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

BrowserWindow* BrowserWindow::instance_ = nullptr;
int BrowserWindow::kSidebarWidth = 280;

// ─── CefTask helpers ───────────────────────────────────────

class FirstTabTask : public CefTask {
public:
    FirstTabTask(BrowserWindow* w) : w_(w) {}
    void Execute() override { w_->CreateFirstTab(); }
private:
    BrowserWindow* w_;
    IMPLEMENT_REFCOUNTING(FirstTabTask);
};

// ─── Constructor ───────────────────────────────────────────

BrowserWindow::BrowserWindow() { instance_ = this; }

CefRefPtr<BrowserClient> BrowserWindow::GetClient() { return client_; }

// ─── GTK lifecycle callbacks ───────────────────────────────

void BrowserWindow::OnDestroy(GtkWidget* widget, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    win->Close();
    CefQuitMessageLoop();
}

void BrowserWindow::OnSizeAllocate(GtkWidget* widget, GdkRectangle* alloc, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    if (alloc->width == win->width_ && alloc->height == win->height_) return;
    win->width_ = alloc->width;
    win->height_ = alloc->height;
    win->ForceResize();
}

// ─── OSR paint + draw ──────────────────────────────────────

void BrowserWindow::GetOsrRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    if (sidebar_browser_ && browser->GetIdentifier() == sidebar_browser_->GetIdentifier()) {
        int sw = kSidebarWidth > 0 ? kSidebarWidth : kDefaultSidebarWidth;
        rect.Set(0, 0, sw, height_ - kTitleBarHeight);
    } else if (split_view_.active && split_view_.second_tab_id >= 0) {
        // Split view: each panel gets a portion
        int tw = width_ - kSidebarWidth;
        int th = height_ - kTitleBarHeight;
        if (tw < 1) tw = 1;
        auto second_browser = tab_manager_.GetBrowser(split_view_.second_tab_id);
        bool is_second = second_browser &&
            browser->GetIdentifier() == second_browser->GetIdentifier();
        if (split_view_.horizontal) {
            int handle = 4;
            int w1 = (int)(tw * split_view_.ratio) - handle / 2;
            int w2 = tw - w1 - handle;
            if (w1 < 1) w1 = 1;
            if (w2 < 1) w2 = 1;
            rect.Set(0, 0, is_second ? w2 : w1, th);
        } else {
            int handle = 4;
            int h1 = (int)(th * split_view_.ratio) - handle / 2;
            int h2 = th - h1 - handle;
            if (h1 < 1) h1 = 1;
            if (h2 < 1) h2 = 1;
            rect.Set(0, 0, tw, is_second ? h2 : h1);
        }
    } else {
        // Tab fills remaining width after sidebar
        int tw = width_ - kSidebarWidth;
        if (tw < 1) tw = 1;
        rect.Set(0, 0, tw, height_ - kTitleBarHeight);
    }
}

void BrowserWindow::OnOsrPaint(CefRefPtr<CefBrowser> browser,
                               const std::vector<CefRect>& dirtyRects,
                               const void* buffer, int width, int height) {
    bool is_sidebar = sidebar_browser_ &&
        browser->GetIdentifier() == sidebar_browser_->GetIdentifier();

    // Check if this is the split view second panel
    bool is_split_second = false;
    if (!is_sidebar && split_view_.active && split_view_.second_tab_id >= 0) {
        auto second_browser = tab_manager_.GetBrowser(split_view_.second_tab_id);
        if (second_browser && browser->GetIdentifier() == second_browser->GetIdentifier()) {
            is_split_second = true;
        }
    }

    std::lock_guard<std::mutex> lock(paint_mutex_);

    cairo_surface_t*& surface = is_sidebar ? sidebar_surface_ :
                                 is_split_second ? split_surface_ : tab_surface_;
    int& buf_w = is_sidebar ? sidebar_buf_w_ :
                  is_split_second ? split_buf_w_ : tab_buf_w_;
    int& buf_h = is_sidebar ? sidebar_buf_h_ :
                  is_split_second ? split_buf_h_ : tab_buf_h_;

    // Only active tab (and split second tab) gets painted
    if (!is_sidebar && !is_split_second) {
        int active = tab_manager_.GetActiveTabId();
        if (active >= 0) {
            auto active_browser = tab_manager_.GetBrowser(active);
            if (active_browser &&
                active_browser->GetIdentifier() != browser->GetIdentifier()) {
                return;  // Not the active tab, skip
            }
        }
    }

    // Recreate surface if size changed
    bool new_surface = false;
    if (surface && (buf_w != width || buf_h != height)) {
        cairo_surface_destroy(surface);
        surface = nullptr;
    }

    if (!surface) {
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        buf_w = width;
        buf_h = height;
        new_surface = true;
    }

    // CEF gives BGRA, Cairo expects ARGB32 (which is BGRA on little-endian)
    unsigned char* dest = cairo_image_surface_get_data(surface);
    cairo_surface_flush(surface);
    const unsigned char* src = static_cast<const unsigned char*>(buffer);
    const int stride = width * 4;

    if (new_surface || dirtyRects.empty()) {
        // Full copy — force alpha to 255 using uint32_t OR
        uint32_t* d32 = reinterpret_cast<uint32_t*>(dest);
        const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
        int total = width * height;
        for (int i = 0; i < total; i++) {
            d32[i] = s32[i] | 0xFF000000u;
        }
    } else {
        // Dirty rect copy — only copy changed regions
        for (const auto& rect : dirtyRects) {
            int rx = rect.x, ry = rect.y, rw = rect.width, rh = rect.height;
            // Clamp to surface bounds
            if (rx < 0) { rw += rx; rx = 0; }
            if (ry < 0) { rh += ry; ry = 0; }
            if (rx + rw > width) rw = width - rx;
            if (ry + rh > height) rh = height - ry;
            if (rw <= 0 || rh <= 0) continue;

            for (int y = ry; y < ry + rh; y++) {
                uint32_t* drow = reinterpret_cast<uint32_t*>(dest + y * stride) + rx;
                const uint32_t* srow = reinterpret_cast<const uint32_t*>(src + y * stride) + rx;
                for (int x = 0; x < rw; x++) {
                    drow[x] = srow[x] | 0xFF000000u;
                }
            }
        }
    }
    cairo_surface_mark_dirty(surface);

    // Queue redraw only for dirty regions
    if (drawing_area_) {
        int x_offset = is_sidebar ? 0 : kSidebarWidth;
        if (new_surface || dirtyRects.empty()) {
            gtk_widget_queue_draw(drawing_area_);
        } else {
            for (const auto& rect : dirtyRects) {
                gtk_widget_queue_draw_area(drawing_area_,
                    rect.x + x_offset, rect.y + kTitleBarHeight, rect.width, rect.height);
            }
        }
    }
}

void BrowserWindow::SetCursor(cef_cursor_type_t type) {
    if (!main_window_ || !gtk_widget_get_window(main_window_)) return;

    GdkCursorType gdk_type;
    switch (type) {
        case CT_POINTER:     gdk_type = GDK_LEFT_PTR; break;
        case CT_HAND:        gdk_type = GDK_HAND2; break;
        case CT_IBEAM:       gdk_type = GDK_XTERM; break;
        case CT_CROSS:       gdk_type = GDK_CROSSHAIR; break;
        case CT_WAIT:        gdk_type = GDK_WATCH; break;
        case CT_PROGRESS:    gdk_type = GDK_WATCH; break;
        case CT_NORTHRESIZE:
        case CT_SOUTHRESIZE:
        case CT_NORTHSOUTHRESIZE: gdk_type = GDK_SB_V_DOUBLE_ARROW; break;
        case CT_EASTRESIZE:
        case CT_WESTRESIZE:
        case CT_EASTWESTRESIZE: gdk_type = GDK_SB_H_DOUBLE_ARROW; break;
        case CT_MOVE:        gdk_type = GDK_FLEUR; break;
        case CT_GRAB:        gdk_type = GDK_HAND1; break;
        case CT_GRABBING:    gdk_type = GDK_HAND1; break;
        case CT_NOTALLOWED:  gdk_type = GDK_X_CURSOR; break;
        case CT_COLUMNRESIZE: gdk_type = GDK_SB_H_DOUBLE_ARROW; break;
        case CT_ROWRESIZE:   gdk_type = GDK_SB_V_DOUBLE_ARROW; break;
        default:             gdk_type = GDK_LEFT_PTR; break;
    }

    GdkCursor* cursor = gdk_cursor_new_for_display(
        gtk_widget_get_display(main_window_), gdk_type);
    gdk_window_set_cursor(gtk_widget_get_window(main_window_), cursor);
    g_object_unref(cursor);
}

gboolean BrowserWindow::OnDraw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    std::lock_guard<std::mutex> lock(win->paint_mutex_);

    int tbh = kTitleBarHeight;

    // ─── Draw titlebar ───────────────────────────────────
    cairo_set_source_rgb(cr, 0.09, 0.09, 0.13);  // #17171f
    cairo_rectangle(cr, 0, 0, win->width_, tbh);
    cairo_fill(cr);

    // Window title — centered
    cairo_set_source_rgb(cr, 0.5, 0.55, 0.7);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, "Orb Browser", &extents);
    cairo_move_to(cr, (win->width_ - extents.width) / 2, tbh / 2 + 4);
    cairo_show_text(cr, "Orb Browser");

    // Three circular buttons: minimize, maximize, close (right side)
    int dot_r = 7;       // radius
    int dot_gap = 8;     // gap between dots
    int dots_right = win->width_ - 16;  // right margin
    int close_cx = dots_right - dot_r;
    int max_cx = close_cx - dot_r * 2 - dot_gap;
    int min_cx = max_cx - dot_r * 2 - dot_gap;
    int dot_cy = tbh / 2;

    // Minimize dot
    if (win->titlebar_hover_btn_ == 0) {
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.45);
    } else {
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.30);
    }
    cairo_arc(cr, min_cx, dot_cy, dot_r, 0, 2 * 3.14159);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.6, 0.65, 0.8);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, min_cx - 4, dot_cy);
    cairo_line_to(cr, min_cx + 4, dot_cy);
    cairo_stroke(cr);

    // Maximize dot
    if (win->titlebar_hover_btn_ == 1) {
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.45);
    } else {
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.30);
    }
    cairo_arc(cr, max_cx, dot_cy, dot_r, 0, 2 * 3.14159);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.6, 0.65, 0.8);
    cairo_set_line_width(cr, 1.2);
    cairo_rectangle(cr, max_cx - 3.5, dot_cy - 3.5, 7, 7);
    cairo_stroke(cr);

    // Close dot
    if (win->titlebar_hover_btn_ == 2) {
        cairo_set_source_rgb(cr, 0.87, 0.30, 0.30);  // Red hover
    } else {
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.30);
    }
    cairo_arc(cr, close_cx, dot_cy, dot_r, 0, 2 * 3.14159);
    cairo_fill(cr);
    if (win->titlebar_hover_btn_ == 2)
        cairo_set_source_rgb(cr, 1, 1, 1);
    else
        cairo_set_source_rgb(cr, 0.6, 0.65, 0.8);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, close_cx - 3.5, dot_cy - 3.5);
    cairo_line_to(cr, close_cx + 3.5, dot_cy + 3.5);
    cairo_stroke(cr);
    cairo_move_to(cr, close_cx + 3.5, dot_cy - 3.5);
    cairo_line_to(cr, close_cx - 3.5, dot_cy + 3.5);
    cairo_stroke(cr);

    // ─── Draw tab content below titlebar ──
    // When sidebar is overlay (unpinned hover), tab starts at x=0
    int tab_x = kSidebarWidth;
    if (win->split_view_.active && win->split_view_.second_tab_id >= 0) {
        // Split view: draw two panels
        int tw = win->width_ - kSidebarWidth;
        int th = win->height_ - tbh;
        int handle = 4;

        if (win->split_view_.horizontal) {
            int w1 = (int)(tw * win->split_view_.ratio) - handle / 2;
            int w2 = tw - w1 - handle;

            // First panel
            if (win->tab_surface_) {
                cairo_set_source_surface(cr, win->tab_surface_, tab_x, tbh);
                cairo_rectangle(cr, tab_x, tbh, w1, th);
                cairo_fill(cr);
            }

            // Handle
            int hx = tab_x + w1;
            bool hover_handle = win->split_view_.dragging_handle;
            cairo_set_source_rgb(cr, hover_handle ? 0.48 : 0.2, hover_handle ? 0.63 : 0.2, hover_handle ? 0.97 : 0.3);
            cairo_rectangle(cr, hx, tbh, handle, th);
            cairo_fill(cr);

            // Second panel
            if (win->split_surface_) {
                cairo_set_source_surface(cr, win->split_surface_, hx + handle, tbh);
                cairo_rectangle(cr, hx + handle, tbh, w2, th);
                cairo_fill(cr);
            }
        } else {
            int h1 = (int)(th * win->split_view_.ratio) - handle / 2;
            int h2 = th - h1 - handle;

            // First panel
            if (win->tab_surface_) {
                cairo_set_source_surface(cr, win->tab_surface_, tab_x, tbh);
                cairo_rectangle(cr, tab_x, tbh, tw, h1);
                cairo_fill(cr);
            }

            // Handle
            int hy = tbh + h1;
            bool hover_handle = win->split_view_.dragging_handle;
            cairo_set_source_rgb(cr, hover_handle ? 0.48 : 0.2, hover_handle ? 0.63 : 0.2, hover_handle ? 0.97 : 0.3);
            cairo_rectangle(cr, tab_x, hy, tw, handle);
            cairo_fill(cr);

            // Second panel
            if (win->split_surface_) {
                cairo_set_source_surface(cr, win->split_surface_, tab_x, hy + handle);
                cairo_rectangle(cr, tab_x, hy + handle, tw, h2);
                cairo_fill(cr);
            }
        }
    } else {
        // Normal single-tab view
        if (win->tab_surface_) {
            cairo_set_source_surface(cr, win->tab_surface_, tab_x, tbh);
            cairo_rectangle(cr, tab_x, tbh, win->tab_buf_w_, win->tab_buf_h_);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.15);
            cairo_rectangle(cr, tab_x, tbh, win->width_ - tab_x, win->height_ - tbh);
            cairo_fill(cr);
        }
    }

    // ─── Draw sidebar (below titlebar) ──────────────────
    // Pinned: draw at x=0 with kSidebarWidth
    // Unpinned overlay: draw at x=0 with sidebar_overlay_width_ on top of tab
    int draw_sw = kSidebarWidth;
    if (!win->sidebar_pinned_ && win->sidebar_overlay_width_ > 0) {
        draw_sw = win->sidebar_overlay_width_;
    }
    if (win->sidebar_surface_ && draw_sw > 0) {
        cairo_set_source_surface(cr, win->sidebar_surface_, 0, tbh);
        cairo_rectangle(cr, 0, tbh, draw_sw, win->sidebar_buf_h_);
        cairo_fill(cr);
    }

    return TRUE;
}

// ─── Input handling ────────────────────────────────────────

CefRefPtr<CefBrowser> BrowserWindow::GetBrowserAtX(int x) {
    // Check pinned sidebar
    if (x < kSidebarWidth && kSidebarWidth > 0 && sidebar_browser_) {
        return sidebar_browser_;
    }
    // Check overlay sidebar (unpinned hover)
    if (!sidebar_pinned_ && sidebar_overlay_width_ > 0 && x < sidebar_overlay_width_ && sidebar_browser_) {
        return sidebar_browser_;
    }

    // Split view: determine which panel
    if (split_view_.active && split_view_.second_tab_id >= 0) {
        int tab_x = x - kSidebarWidth;
        int tw = width_ - kSidebarWidth;
        int handle = 4;
        if (split_view_.horizontal) {
            int w1 = (int)(tw * split_view_.ratio) - handle / 2;
            if (tab_x >= w1 && tab_x < w1 + handle) {
                // On the handle — return active tab but mark for drag
                int active = tab_manager_.GetActiveTabId();
                return active >= 0 ? tab_manager_.GetBrowser(active) : nullptr;
            }
            if (tab_x >= w1 + handle) {
                return tab_manager_.GetBrowser(split_view_.second_tab_id);
            }
        } else {
            // For vertical split, need Y — handled in OnButtonPress/OnMotionNotify
        }
    }

    int active = tab_manager_.GetActiveTabId();
    if (active >= 0) {
        return tab_manager_.GetBrowser(active);
    }
    return nullptr;
}

int BrowserWindow::AdjustX(int x, CefRefPtr<CefBrowser> browser) {
    // If it's a tab browser, subtract sidebar width
    if (sidebar_browser_ && browser->GetIdentifier() != sidebar_browser_->GetIdentifier()) {
        int adjusted = x - kSidebarWidth;
        // Split view second panel: also subtract first panel + handle width
        if (split_view_.active && split_view_.second_tab_id >= 0 && split_view_.horizontal) {
            auto second = tab_manager_.GetBrowser(split_view_.second_tab_id);
            if (second && browser->GetIdentifier() == second->GetIdentifier()) {
                int tw = width_ - kSidebarWidth;
                int w1 = (int)(tw * split_view_.ratio) - 2;
                adjusted = x - kSidebarWidth - w1 - 4;
            }
        }
        return adjusted;
    }
    return x;
}

uint32_t BrowserWindow::GetCefModifiers(guint state) {
    uint32_t mods = 0;
    if (state & GDK_SHIFT_MASK)   mods |= EVENTFLAG_SHIFT_DOWN;
    if (state & GDK_CONTROL_MASK) mods |= EVENTFLAG_CONTROL_DOWN;
    if (state & GDK_MOD1_MASK)    mods |= EVENTFLAG_ALT_DOWN;
    if (state & GDK_BUTTON1_MASK) mods |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (state & GDK_BUTTON2_MASK) mods |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (state & GDK_BUTTON3_MASK) mods |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return mods;
}

gboolean BrowserWindow::OnButtonPress(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    int x = (int)event->x, y = (int)event->y;

    // ─── Titlebar handling ───────────────────────────
    if (y < kTitleBarHeight && event->button == 1) {
        int dot_r = 7, dot_gap = 8;
        int dots_right = win->width_ - 16;
        int close_cx = dots_right - dot_r;
        int max_cx = close_cx - dot_r * 2 - dot_gap;
        int min_cx = max_cx - dot_r * 2 - dot_gap;
        int dot_cy = kTitleBarHeight / 2;
        int hit_r = dot_r + 2;

        // Check close dot
        int dx = x - close_cx, dy = y - dot_cy;
        if (dx * dx + dy * dy <= hit_r * hit_r) {
            gtk_widget_destroy(win->main_window_);
            return TRUE;
        }
        // Check maximize dot
        dx = x - max_cx; dy = y - dot_cy;
        if (dx * dx + dy * dy <= hit_r * hit_r) {
            GdkWindow* gdk_win = gtk_widget_get_window(win->main_window_);
            GdkWindowState state = gdk_window_get_state(gdk_win);
            if (state & GDK_WINDOW_STATE_MAXIMIZED)
                gtk_window_unmaximize(GTK_WINDOW(win->main_window_));
            else
                gtk_window_maximize(GTK_WINDOW(win->main_window_));
            return TRUE;
        }
        // Check minimize dot
        dx = x - min_cx; dy = y - dot_cy;
        if (dx * dx + dy * dy <= hit_r * hit_r) {
            gtk_window_iconify(GTK_WINDOW(win->main_window_));
            return TRUE;
        }

        // Double-click to maximize
        if (event->type == GDK_2BUTTON_PRESS) {
            GdkWindow* gdk_win = gtk_widget_get_window(win->main_window_);
            GdkWindowState state = gdk_window_get_state(gdk_win);
            if (state & GDK_WINDOW_STATE_MAXIMIZED)
                gtk_window_unmaximize(GTK_WINDOW(win->main_window_));
            else
                gtk_window_maximize(GTK_WINDOW(win->main_window_));
            return TRUE;
        }

        // Start window drag
        win->titlebar_dragging_ = true;
        win->drag_start_x_ = (int)event->x_root;
        win->drag_start_y_ = (int)event->y_root;
        gint wx, wy;
        gtk_window_get_position(GTK_WINDOW(win->main_window_), &wx, &wy);
        win->win_start_x_ = wx;
        win->win_start_y_ = wy;
        return TRUE;
    }

    // ─── Content area (below titlebar) ───────────────
    int cy = y - kTitleBarHeight;
    if (cy < 0) return FALSE;

    // Split view handle drag start
    if (win->split_view_.active && win->split_view_.second_tab_id >= 0 && event->button == 1) {
        int tw = win->width_ - kSidebarWidth;
        int handle = 4;
        int tab_x = x - kSidebarWidth;
        if (win->split_view_.horizontal) {
            int w1 = (int)(tw * win->split_view_.ratio) - handle / 2;
            if (tab_x >= w1 - 2 && tab_x <= w1 + handle + 2) {
                win->split_view_.dragging_handle = true;
                return TRUE;
            }
        } else {
            int th = win->height_ - kTitleBarHeight;
            int h1 = (int)(th * win->split_view_.ratio) - handle / 2;
            if (cy >= h1 - 2 && cy <= h1 + handle + 2) {
                win->split_view_.dragging_handle = true;
                return TRUE;
            }
        }
    }

    auto browser = win->GetBrowserAtX(x);
    if (!browser) return FALSE;

    // Switch focus to clicked browser
    if (win->focused_browser_ && win->focused_browser_->GetIdentifier() != browser->GetIdentifier()) {
        win->focused_browser_->GetHost()->SetFocus(false);
    }
    win->focused_browser_ = browser;
    browser->GetHost()->SetFocus(true);

    CefMouseEvent cef_event;
    cef_event.x = win->AdjustX(x, browser);
    cef_event.y = cy;
    cef_event.modifiers = win->GetCefModifiers(event->state);

    cef_mouse_button_type_t btn = MBT_LEFT;
    if (event->button == 2) btn = MBT_MIDDLE;
    else if (event->button == 3) btn = MBT_RIGHT;

    int click_count = 1;
    if (event->type == GDK_2BUTTON_PRESS) click_count = 2;
    else if (event->type == GDK_3BUTTON_PRESS) click_count = 3;
    browser->GetHost()->SendMouseClickEvent(cef_event, btn, false, click_count);
    return TRUE;
}

gboolean BrowserWindow::OnButtonRelease(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);

    if (win->titlebar_dragging_) {
        win->titlebar_dragging_ = false;
        return TRUE;
    }

    if (win->split_view_.dragging_handle) {
        win->split_view_.dragging_handle = false;
        win->ForceResize();
        gtk_widget_queue_draw(win->drawing_area_);
        return TRUE;
    }

    int y = (int)event->y - kTitleBarHeight;
    if (y < 0) return FALSE;

    auto browser = win->GetBrowserAtX((int)event->x);
    if (!browser) return FALSE;

    CefMouseEvent cef_event;
    cef_event.x = win->AdjustX((int)event->x, browser);
    cef_event.y = y;
    cef_event.modifiers = win->GetCefModifiers(event->state);

    cef_mouse_button_type_t btn = MBT_LEFT;
    if (event->button == 2) btn = MBT_MIDDLE;
    else if (event->button == 3) btn = MBT_RIGHT;

    browser->GetHost()->SendMouseClickEvent(cef_event, btn, true, 1);
    return TRUE;
}

gboolean BrowserWindow::OnMotionNotify(GtkWidget* widget, GdkEventMotion* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    int x = (int)event->x, y = (int)event->y;

    // ─── Window drag ─────────────────────────────────
    if (win->titlebar_dragging_) {
        int dx = (int)event->x_root - win->drag_start_x_;
        int dy = (int)event->y_root - win->drag_start_y_;
        gtk_window_move(GTK_WINDOW(win->main_window_),
                        win->win_start_x_ + dx, win->win_start_y_ + dy);
        return TRUE;
    }

    // ─── Split view handle drag ──────────────────────
    if (win->split_view_.dragging_handle) {
        if (win->split_view_.horizontal) {
            int tw = win->width_ - kSidebarWidth;
            float new_ratio = (float)(x - kSidebarWidth) / tw;
            if (new_ratio < 0.15f) new_ratio = 0.15f;
            if (new_ratio > 0.85f) new_ratio = 0.85f;
            win->split_view_.ratio = new_ratio;
        } else {
            int th = win->height_ - kTitleBarHeight;
            float new_ratio = (float)(y - kTitleBarHeight) / th;
            if (new_ratio < 0.15f) new_ratio = 0.15f;
            if (new_ratio > 0.85f) new_ratio = 0.85f;
            win->split_view_.ratio = new_ratio;
        }
        gtk_widget_queue_draw(win->drawing_area_);
        return TRUE;
    }

    // ─── Titlebar hover detection ────────────────────
    if (y < kTitleBarHeight) {
        int dot_r = 7, dot_gap = 8;
        int dots_right = win->width_ - 16;
        int close_cx = dots_right - dot_r;
        int max_cx = close_cx - dot_r * 2 - dot_gap;
        int min_cx = max_cx - dot_r * 2 - dot_gap;
        int dot_cy = kTitleBarHeight / 2;
        int hit_r = dot_r + 3;

        int old_hover = win->titlebar_hover_btn_;
        win->titlebar_hover_btn_ = -1;

        int dx, dy;
        dx = x - min_cx; dy = y - dot_cy;
        if (dx * dx + dy * dy <= hit_r * hit_r) {
            win->titlebar_hover_btn_ = 0;
        } else {
            dx = x - max_cx; dy = y - dot_cy;
            if (dx * dx + dy * dy <= hit_r * hit_r) {
                win->titlebar_hover_btn_ = 1;
            } else {
                dx = x - close_cx; dy = y - dot_cy;
                if (dx * dx + dy * dy <= hit_r * hit_r) {
                    win->titlebar_hover_btn_ = 2;
                }
            }
        }

        if (old_hover != win->titlebar_hover_btn_ && win->drawing_area_) {
            gtk_widget_queue_draw_area(win->drawing_area_,
                min_cx - dot_r - 4, 0, (close_cx - min_cx) + dot_r * 2 + 8, kTitleBarHeight);
        }
        return TRUE;
    }

    // Clear titlebar hover when leaving titlebar
    if (win->titlebar_hover_btn_ >= 0) {
        win->titlebar_hover_btn_ = -1;
        if (win->drawing_area_) {
            gtk_widget_queue_draw_area(win->drawing_area_, 0, 0, win->width_, kTitleBarHeight);
        }
    }

    // ─── Auto-hide sidebar detection ─────────────────
    int cy = y - kTitleBarHeight;
    if (!win->sidebar_pinned_) {
        int sidebar_w = win->sidebar_overlay_width_ > 0 ? win->sidebar_overlay_width_ : kSidebarWidth;
        if (x <= 4 && !win->sidebar_hovered_) {
            win->sidebar_hovered_ = true;
            if (win->sidebar_hide_timer_) {
                g_source_remove(win->sidebar_hide_timer_);
                win->sidebar_hide_timer_ = 0;
            }
            win->ShowSidebarAnimated();
        } else if (win->sidebar_hovered_ && sidebar_w > 0 && x > sidebar_w + 20) {
            if (!win->sidebar_hide_timer_) {
                win->sidebar_hide_timer_ = g_timeout_add(400, [](gpointer data) -> gboolean {
                    auto* w = static_cast<BrowserWindow*>(data);
                    w->sidebar_hide_timer_ = 0;
                    if (!w->sidebar_pinned_ && w->sidebar_hovered_) {
                        w->sidebar_hovered_ = false;
                        w->HideSidebarAnimated();
                    }
                    return G_SOURCE_REMOVE;
                }, win);
            }
        } else if (x <= sidebar_w && win->sidebar_hovered_) {
            if (win->sidebar_hide_timer_) {
                g_source_remove(win->sidebar_hide_timer_);
                win->sidebar_hide_timer_ = 0;
            }
        }
    }

    // ─── Send to CEF ─────────────────────────────────
    if (cy < 0) return FALSE;

    auto browser = win->GetBrowserAtX(x);
    if (!browser) return FALSE;

    CefMouseEvent cef_event;
    cef_event.x = win->AdjustX(x, browser);
    cef_event.y = cy;
    cef_event.modifiers = win->GetCefModifiers(event->state);

    browser->GetHost()->SendMouseMoveEvent(cef_event, false);
    return TRUE;
}

gboolean BrowserWindow::OnScroll(GtkWidget* widget, GdkEventScroll* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    int y = (int)event->y - kTitleBarHeight;
    if (y < 0) return FALSE;  // Scroll in titlebar — ignore

    auto browser = win->GetBrowserAtX((int)event->x);
    if (!browser) return FALSE;

    CefMouseEvent cef_event;
    cef_event.x = win->AdjustX((int)event->x, browser);
    cef_event.y = y;
    cef_event.modifiers = win->GetCefModifiers(event->state);

    int dx = 0, dy = 0;
    switch (event->direction) {
        case GDK_SCROLL_UP:    dy = 120; break;
        case GDK_SCROLL_DOWN:  dy = -120; break;
        case GDK_SCROLL_LEFT:  dx = 120; break;
        case GDK_SCROLL_RIGHT: dx = -120; break;
        case GDK_SCROLL_SMOOTH: {
            // Accumulate smooth scroll deltas for snappier feel
            static double accum_x = 0, accum_y = 0;
            accum_x += -event->delta_x * 300;
            accum_y += -event->delta_y * 300;
            dx = (int)accum_x;
            dy = (int)accum_y;
            accum_x -= dx;
            accum_y -= dy;
            if (dx == 0 && dy == 0) return TRUE;
            break;
        }
    }

    browser->GetHost()->SendMouseWheelEvent(cef_event, dx, dy);
    return TRUE;
}

// Map X11 hardware keycode to Windows virtual key code
// Hardware keycodes are stable across layouts (they map to physical keys)
static int HardwareKeycodeToVK(unsigned int keycode) {
    // Standard US QWERTY layout hardware keycode mapping
    // Based on evdev keycodes (X11 keycode = evdev + 8)
    static const int kMap[] = {
        // keycode -> VK
        // 8 = ???, 9 = Escape
    };
    // Use a direct switch for known keys
    switch (keycode) {
        case 9:  return 0x1B;  // Escape
        case 10: return '1';   case 11: return '2';   case 12: return '3';
        case 13: return '4';   case 14: return '5';   case 15: return '6';
        case 16: return '7';   case 17: return '8';   case 18: return '9';
        case 19: return '0';
        case 20: return 0xBD;  // - (VK_OEM_MINUS)
        case 21: return 0xBB;  // = (VK_OEM_PLUS)
        case 22: return 0x08;  // Backspace
        case 23: return 0x09;  // Tab
        case 24: return 'Q';   case 25: return 'W';   case 26: return 'E';
        case 27: return 'R';   case 28: return 'T';   case 29: return 'Y';
        case 30: return 'U';   case 31: return 'I';   case 32: return 'O';
        case 33: return 'P';
        case 34: return 0xDB;  // [ (VK_OEM_4)
        case 35: return 0xDD;  // ] (VK_OEM_6)
        case 36: return 0x0D;  // Return
        case 37: return 0x11;  // Left Ctrl
        case 38: return 'A';   case 39: return 'S';   case 40: return 'D';
        case 41: return 'F';   case 42: return 'G';   case 43: return 'H';
        case 44: return 'J';   case 45: return 'K';   case 46: return 'L';
        case 47: return 0xBA;  // ; (VK_OEM_1)
        case 48: return 0xDE;  // ' (VK_OEM_7)
        case 49: return 0xC0;  // ` (VK_OEM_3)
        case 50: return 0x10;  // Left Shift
        case 51: return 0xDC;  // \ (VK_OEM_5)
        case 52: return 'Z';   case 53: return 'X';   case 54: return 'C';
        case 55: return 'V';   case 56: return 'B';   case 57: return 'N';
        case 58: return 'M';
        case 59: return 0xBC;  // , (VK_OEM_COMMA)
        case 60: return 0xBE;  // . (VK_OEM_PERIOD)
        case 61: return 0xBF;  // / (VK_OEM_2)
        case 62: return 0x10;  // Right Shift
        case 63: return 0x6A;  // KP *
        case 64: return 0x12;  // Left Alt
        case 65: return 0x20;  // Space
        case 66: return 0x14;  // Caps Lock
        // F1-F12
        case 67: return 0x70;  case 68: return 0x71;  case 69: return 0x72;
        case 70: return 0x73;  case 71: return 0x74;  case 72: return 0x75;
        case 73: return 0x76;  case 74: return 0x77;  case 75: return 0x78;
        case 76: return 0x79;  case 95: return 0x7A;  case 96: return 0x7B;
        // Navigation
        case 110: return 0x24; // Home
        case 111: return 0x26; // Up
        case 112: return 0x21; // Page Up
        case 113: return 0x25; // Left
        case 114: return 0x27; // Right
        case 115: return 0x23; // End
        case 116: return 0x28; // Down
        case 117: return 0x22; // Page Down
        case 118: return 0x2D; // Insert
        case 119: return 0x2E; // Delete
        // KP Enter
        case 104: return 0x0D;
        // Right Ctrl, Right Alt
        case 105: return 0x11;
        case 108: return 0x12;
        default: return 0;
    }
}

CefKeyEvent BrowserWindow::TranslateKeyEvent(GdkEventKey* event) {
    CefKeyEvent cef_event;
    cef_event.native_key_code = event->hardware_keycode;
    cef_event.modifiers = GetCefModifiers(event->state);
    cef_event.is_system_key = false;

    // Use hardware keycode for reliable VK mapping
    int vk = HardwareKeycodeToVK(event->hardware_keycode);
    if (vk != 0) {
        cef_event.windows_key_code = vk;
    } else {
        // Fallback: use GDK keyval for unmapped keys
        cef_event.windows_key_code = event->keyval & 0xFFFF;
    }

    return cef_event;
}

gboolean BrowserWindow::OnKeyPress(GtkWidget* widget, GdkEventKey* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    auto browser = win->focused_browser_;
    if (!browser) browser = win->sidebar_browser_;
    if (!browser) return FALSE;

    CefKeyEvent cef_event = win->TranslateKeyEvent(event);

    // Send RawKeyDown
    cef_event.type = KEYEVENT_RAWKEYDOWN;
    browser->GetHost()->SendKeyEvent(cef_event);

    // Send Char event for printable characters
    if (event->string && event->string[0] && !(event->state & GDK_CONTROL_MASK)) {
        gunichar uc = gdk_keyval_to_unicode(event->keyval);
        if (uc) {
            CefKeyEvent char_event = cef_event;
            char_event.type = KEYEVENT_CHAR;
            char_event.character = uc;
            char_event.unmodified_character = uc;
            char_event.windows_key_code = uc;
            browser->GetHost()->SendKeyEvent(char_event);
        }
    }

    return TRUE;
}

gboolean BrowserWindow::OnKeyRelease(GtkWidget* widget, GdkEventKey* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    auto browser = win->focused_browser_;
    if (!browser) browser = win->sidebar_browser_;
    if (!browser) return FALSE;

    CefKeyEvent cef_event = win->TranslateKeyEvent(event);
    cef_event.type = KEYEVENT_KEYUP;
    browser->GetHost()->SendKeyEvent(cef_event);
    return TRUE;
}

gboolean BrowserWindow::OnFocusIn(GtkWidget* widget, GdkEventFocus* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    if (win->focused_browser_) {
        win->focused_browser_->GetHost()->SetFocus(true);
    }
    return FALSE;
}

gboolean BrowserWindow::OnFocusOut(GtkWidget* widget, GdkEventFocus* event, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    if (win->focused_browser_) {
        win->focused_browser_->GetHost()->SetFocus(false);
    }
    return FALSE;
}

// ─── ForceResize ───────────────────────────────────────────

void BrowserWindow::ForceResize() {
    if (sidebar_browser_) {
        sidebar_browser_->GetHost()->WasResized();
    }
    // Only the browsers that are actually on screen need a new size. A hidden
    // tab is resized again in ShowTab() when it comes back.
    const int active = tab_manager_.GetActiveTabId();
    if (active >= 0) {
        auto browser = tab_manager_.GetBrowser(active);
        if (browser) browser->GetHost()->WasResized();
    }
    if (split_view_.active && split_view_.second_tab_id >= 0 &&
        split_view_.second_tab_id != active) {
        auto second = tab_manager_.GetBrowser(split_view_.second_tab_id);
        if (second) second->GetHost()->WasResized();
    }
}

// ─── Factory ───────────────────────────────────────────────

BrowserWindow* BrowserWindow::Create(std::shared_ptr<FilterRuleSet> filter_rules) {
    CEF_REQUIRE_UI_THREAD();

    auto* win = new BrowserWindow();
    win->filter_rules_ = filter_rules;
    win->client_ = new BrowserClient(&win->tab_manager_, win, filter_rules);

    win->tab_manager_.SetNotifyCallback(
        [win](const std::string& js) {
            if (win->sidebar_browser_ && win->sidebar_browser_->GetMainFrame())
                win->sidebar_browser_->GetMainFrame()
                    ->ExecuteJavaScript(js, "orb://chrome/", 0);
        });
    win->tab_manager_.SetBrowserWindow(win);

    // Create GTK window
    win->main_window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win->main_window_), "Orb Browser");
    gtk_window_set_default_size(GTK_WINDOW(win->main_window_), 1400, 900);
    gtk_window_set_decorated(GTK_WINDOW(win->main_window_), FALSE);  // Custom titlebar
    g_signal_connect(win->main_window_, "destroy", G_CALLBACK(OnDestroy), win);
    g_signal_connect(win->main_window_, "size-allocate", G_CALLBACK(OnSizeAllocate), win);

    // Create drawing area for OSR compositing
    win->drawing_area_ = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(win->main_window_), win->drawing_area_);

    // Enable events on drawing area
    gtk_widget_set_can_focus(win->drawing_area_, TRUE);
    gtk_widget_add_events(win->drawing_area_,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
        GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
        GDK_FOCUS_CHANGE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

    // Connect drawing area signals
    g_signal_connect(win->drawing_area_, "draw", G_CALLBACK(OnDraw), win);
    g_signal_connect(win->drawing_area_, "button-press-event", G_CALLBACK(OnButtonPress), win);
    g_signal_connect(win->drawing_area_, "button-release-event", G_CALLBACK(OnButtonRelease), win);
    g_signal_connect(win->drawing_area_, "motion-notify-event", G_CALLBACK(OnMotionNotify), win);
    g_signal_connect(win->drawing_area_, "scroll-event", G_CALLBACK(OnScroll), win);
    g_signal_connect(win->drawing_area_, "key-press-event", G_CALLBACK(OnKeyPress), win);
    g_signal_connect(win->drawing_area_, "key-release-event", G_CALLBACK(OnKeyRelease), win);
    g_signal_connect(win->drawing_area_, "focus-in-event", G_CALLBACK(OnFocusIn), win);
    g_signal_connect(win->drawing_area_, "focus-out-event", G_CALLBACK(OnFocusOut), win);

    gtk_widget_show_all(win->main_window_);
    gtk_widget_grab_focus(win->drawing_area_);

    GtkAllocation alloc;
    gtk_widget_get_allocation(win->main_window_, &alloc);
    win->width_ = alloc.width;
    win->height_ = alloc.height;

    // Get X11 window handle for CEF dialogs
    GdkWindow* gdk_win = gtk_widget_get_window(win->main_window_);
    unsigned long parent_xwin = 0;
    if (gdk_win && GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        parent_xwin = GDK_WINDOW_XID(gdk_win);
    }

    // Create sidebar browser (OSR / windowless)
    CefWindowInfo info;
    info.SetAsWindowless(parent_xwin);
    CefBrowserSettings settings;
    // Use monitor refresh rate
    {
        GdkScreen* screen = gtk_widget_get_screen(win->main_window_);
        GdkMonitor* monitor = gdk_display_get_primary_monitor(gdk_screen_get_display(screen));
        int rate = monitor ? gdk_monitor_get_refresh_rate(monitor) / 1000 : 60;
        settings.windowless_frame_rate = rate > 0 ? rate : 60;
    }
    settings.background_color = CefColorSetARGB(255, 30, 30, 30);
    CefBrowserHost::CreateBrowser(info, win->GetClient(),
        "orb://chrome/index.html", settings, nullptr, nullptr);

    // Create first tab after sidebar is ready
    CefPostDelayedTask(TID_UI, new FirstTabTask(win), 1000);

    return win;
}

// ─── Tab management ────────────────────────────────────────

void BrowserWindow::CreateFirstTab() {
    const char* home = getenv("HOME");

    // Try to restore previous session
    if (home) {
        std::string session_path = std::string(home) + "/.config/orb-browser/session.json";
        std::ifstream sf(session_path);
        if (sf.good()) {
            std::string content((std::istreambuf_iterator<char>(sf)),
                                 std::istreambuf_iterator<char>());
            // Parse JSON array of {url, title} objects
            // Find all "url":"..." entries
            bool restored = false;
            bool first_tab = true;
            size_t pos = 0;
            while ((pos = content.find("\"url\":\"", pos)) != std::string::npos) {
                pos += 7; // skip "url":"
                auto end = content.find('"', pos);
                if (end == std::string::npos) break;
                std::string url = content.substr(pos, end - pos);
                // Unescape
                size_t p = 0;
                while ((p = url.find("\\\"", p)) != std::string::npos) { url.replace(p, 2, "\""); p++; }
                p = 0;
                while ((p = url.find("\\\\", p)) != std::string::npos) { url.replace(p, 2, "\\"); p++; }

                if (!url.empty()) {
                    int tab_id = tab_manager_.CreateTab(url);
                    if (first_tab) {
                        // Only load the first tab immediately
                        CreateTabBrowser(tab_id, url);
                        first_tab = false;
                    }
                    // Other tabs are lazy — created in UI but no browser yet
                    // They'll load when user clicks (switchTab triggers CreateTabBrowser)
                    restored = true;
                }
                pos = end + 1;
            }
            if (restored) {
                // Delete session file after restore (one-time use)
                std::remove(session_path.c_str());
                return;
            }
        }
    }

    // No session to restore — open newtab page
    std::string engine = "google";
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
    int tab_id = tab_manager_.CreateTab(url);
    CreateTabBrowser(tab_id, url);
}

void BrowserWindow::CreateTabBrowser(int tab_id, const std::string& url, bool incognito) {
    CEF_REQUIRE_UI_THREAD();
    pending_tab_ids_.push(tab_id);

    GdkWindow* gdk_win = gtk_widget_get_window(main_window_);
    unsigned long parent_xwin = 0;
    if (gdk_win && GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        parent_xwin = GDK_WINDOW_XID(gdk_win);
    }

    CefWindowInfo info;
    info.SetAsWindowless(parent_xwin);
    CefBrowserSettings settings;
    {
        GdkScreen* screen = gtk_widget_get_screen(main_window_);
        GdkMonitor* monitor = gdk_display_get_primary_monitor(gdk_screen_get_display(screen));
        int rate = monitor ? gdk_monitor_get_refresh_rate(monitor) / 1000 : 60;
        settings.windowless_frame_rate = rate > 0 ? rate : 60;
    }
    settings.background_color = CefColorSetARGB(255, 255, 255, 255);

    CefRefPtr<CefRequestContext> request_context = nullptr;
    if (incognito) {
        // Incognito: use an in-memory request context (no cache, no cookies from main)
        CefRequestContextSettings ctx_settings;
        request_context = CefRequestContext::CreateContext(ctx_settings, nullptr);
    }

    CefBrowserHost::CreateBrowser(info, client_, url, settings, nullptr, request_context);
}

void BrowserWindow::ShowTab(int tab_id) {
    CEF_REQUIRE_UI_THREAD();

    // Clear old tab surface so stale pixels from previous tab don't persist
    {
        std::lock_guard<std::mutex> lock(paint_mutex_);
        if (tab_surface_) {
            cairo_surface_destroy(tab_surface_);
            tab_surface_ = nullptr;
            tab_buf_w_ = 0;
            tab_buf_h_ = 0;
        }
    }

    // Hide every other tab: an OSR browser that is never told it is hidden
    // keeps producing frames we immediately throw away.
    for (int id : tab_manager_.GetTabIds()) {
        if (id == tab_id || id == split_view_.second_tab_id) continue;
        auto other = tab_manager_.GetBrowser(id);
        if (other) other->GetHost()->WasHidden(true);
    }

    // Tell the new tab's browser to repaint
    auto browser = tab_manager_.GetBrowser(tab_id);
    if (browser) {
        browser->GetHost()->WasHidden(false);
        browser->GetHost()->WasResized();
        browser->GetHost()->Invalidate(PET_VIEW);
    }
    if (drawing_area_) {
        gtk_widget_queue_draw(drawing_area_);
    }
}

struct SidebarAnimData {
    BrowserWindow* win;
    int target_width;
    bool is_show;  // true = showing, false = hiding
};

void BrowserWindow::ToggleSidebar() {
    CEF_REQUIRE_UI_THREAD();
    // Toggle pinned state
    SetSidebarPinned(!sidebar_pinned_);
}

void BrowserWindow::SetSidebarPinned(bool pinned) {
    CEF_REQUIRE_UI_THREAD();
    sidebar_pinned_ = pinned;

    if (pinned) {
        // Pin: show sidebar, cancel timers, reset overlay
        sidebar_hovered_ = false;
        sidebar_overlay_width_ = 0;
        if (sidebar_hide_timer_) {
            g_source_remove(sidebar_hide_timer_);
            sidebar_hide_timer_ = 0;
        }
        ShowSidebarAnimated();
    } else {
        // Unpin: collapse kSidebarWidth directly and resize
        sidebar_overlay_width_ = 0;
        auto* anim = new SidebarAnimData{this, 0, false};
        g_timeout_add(12, [](gpointer data) -> gboolean {
            auto* a = static_cast<SidebarAnimData*>(data);
            auto* w = a->win;
            if (w->sidebar_pinned_) { delete a; return G_SOURCE_REMOVE; }
            int remaining = kSidebarWidth;
            int step = remaining / 3;
            if (step < 12) step = 12;
            if (step > remaining) step = remaining;
            kSidebarWidth -= step;
            if (kSidebarWidth <= 0) kSidebarWidth = 0;
            w->ForceResize();
            if (w->drawing_area_) gtk_widget_queue_draw(w->drawing_area_);
            if (kSidebarWidth <= 0) { w->sidebar_visible_ = false; delete a; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, anim);
    }

    // Notify sidebar JS about pin state
    if (sidebar_browser_ && sidebar_browser_->GetMainFrame()) {
        std::string js = "if(typeof orb !== 'undefined') orb.onSidebarPinChanged(" +
                         std::string(pinned ? "true" : "false") + ");";
        sidebar_browser_->GetMainFrame()->ExecuteJavaScript(js, "orb://chrome/", 0);
    }
}

void BrowserWindow::ShowSidebarAnimated() {
    if (sidebar_pinned_) {
        // Pinned: animate kSidebarWidth with ForceResize
        if (kSidebarWidth >= kDefaultSidebarWidth) return;
        auto* anim = new SidebarAnimData{this, kDefaultSidebarWidth, true};
        g_timeout_add(12, [](gpointer data) -> gboolean {
            auto* a = static_cast<SidebarAnimData*>(data);
            auto* w = a->win;
            if (!w->sidebar_pinned_) { delete a; return G_SOURCE_REMOVE; }
            int remaining = a->target_width - kSidebarWidth;
            int step = remaining / 3;
            if (step < 12) step = 12;
            if (step > remaining) step = remaining;
            kSidebarWidth += step;
            if (kSidebarWidth >= a->target_width) kSidebarWidth = a->target_width;
            w->sidebar_visible_ = true;
            w->ForceResize();
            if (w->drawing_area_) gtk_widget_queue_draw(w->drawing_area_);
            if (kSidebarWidth >= a->target_width) { delete a; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, anim);
    } else {
        // Unpinned hover: animate overlay width only (no resize)
        if (sidebar_overlay_width_ >= kDefaultSidebarWidth) return;
        auto* anim = new SidebarAnimData{this, kDefaultSidebarWidth, true};
        g_timeout_add(12, [](gpointer data) -> gboolean {
            auto* a = static_cast<SidebarAnimData*>(data);
            auto* w = a->win;
            if (!w->sidebar_hovered_) { delete a; return G_SOURCE_REMOVE; }
            int remaining = a->target_width - w->sidebar_overlay_width_;
            int step = remaining / 3;
            if (step < 12) step = 12;
            if (step > remaining) step = remaining;
            w->sidebar_overlay_width_ += step;
            if (w->sidebar_overlay_width_ >= a->target_width) w->sidebar_overlay_width_ = a->target_width;
            w->sidebar_visible_ = true;
            // No ForceResize — just redraw overlay
            if (w->drawing_area_) gtk_widget_queue_draw(w->drawing_area_);
            if (w->sidebar_overlay_width_ >= a->target_width) { delete a; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, anim);
    }
}

void BrowserWindow::HideSidebarAnimated() {
    if (sidebar_pinned_) {
        // Pinned hide: animate kSidebarWidth
        if (kSidebarWidth <= 0) return;
        auto* anim = new SidebarAnimData{this, 0, false};
        g_timeout_add(12, [](gpointer data) -> gboolean {
            auto* a = static_cast<SidebarAnimData*>(data);
            auto* w = a->win;
            if (w->sidebar_pinned_ || w->sidebar_hovered_) { delete a; return G_SOURCE_REMOVE; }
            int remaining = kSidebarWidth;
            int step = remaining / 3;
            if (step < 12) step = 12;
            if (step > remaining) step = remaining;
            kSidebarWidth -= step;
            if (kSidebarWidth <= 0) kSidebarWidth = 0;
            w->ForceResize();
            if (w->drawing_area_) gtk_widget_queue_draw(w->drawing_area_);
            if (kSidebarWidth <= 0) { w->sidebar_visible_ = false; delete a; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, anim);
    } else {
        // Unpinned hover hide: animate overlay width (no resize)
        if (sidebar_overlay_width_ <= 0) return;
        auto* anim = new SidebarAnimData{this, 0, false};
        g_timeout_add(12, [](gpointer data) -> gboolean {
            auto* a = static_cast<SidebarAnimData*>(data);
            auto* w = a->win;
            if (w->sidebar_pinned_ || w->sidebar_hovered_) { delete a; return G_SOURCE_REMOVE; }
            int remaining = w->sidebar_overlay_width_;
            int step = remaining / 3;
            if (step < 12) step = 12;
            if (step > remaining) step = remaining;
            w->sidebar_overlay_width_ -= step;
            if (w->sidebar_overlay_width_ <= 0) w->sidebar_overlay_width_ = 0;
            if (w->drawing_area_) gtk_widget_queue_draw(w->drawing_area_);
            if (w->sidebar_overlay_width_ <= 0) { w->sidebar_visible_ = false; delete a; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, anim);
    }
}

void BrowserWindow::OnBrowserCreated(CefRefPtr<CefBrowser> browser) {
    int bid = browser->GetIdentifier();
    bool is_sidebar = !sidebar_created_;

    std::cerr << "[ORB] OnBrowserCreated: id=" << bid << " sidebar=" << is_sidebar << std::endl;

    if (!sidebar_created_) {
        sidebar_browser_ = browser;
        sidebar_created_ = true;
        client_->SetChromeBrowser(browser);
        focused_browser_ = browser;
    } else if (!pending_tab_ids_.empty()) {
        int tab_id = pending_tab_ids_.front();
        pending_tab_ids_.pop();
        tab_manager_.OnBrowserCreated(tab_id, browser);
        tab_manager_.SwitchTab(tab_id);
    }

    // Trigger initial paint cycle for OSR
    browser->GetHost()->WasResized();
    browser->GetHost()->SetFocus(true);
    browser->GetHost()->Invalidate(PET_VIEW);
}

void BrowserWindow::OnBrowserClosed(CefRefPtr<CefBrowser> browser) {
    if (!browser) return;
    const int bid = browser->GetIdentifier();

    if (focused_browser_ && focused_browser_->GetIdentifier() == bid) {
        focused_browser_ = nullptr;
    }
    if (sidebar_browser_ && sidebar_browser_->GetIdentifier() == bid) {
        sidebar_browser_ = nullptr;
    }
    // The closing tab may be the split partner; drop the panel with it.
    if (split_view_.active && split_view_.second_tab_id >= 0) {
        auto second = tab_manager_.GetBrowser(split_view_.second_tab_id);
        if (second && second->GetIdentifier() == bid) {
            CloseSplitView();
        }
    }
}

void BrowserWindow::Close() {
    // Save session (tab URLs) before closing
    {
        const char* home = getenv("HOME");
        if (home) {
            std::string dir = std::string(home) + "/.config/orb-browser";
            mkdir(dir.c_str(), 0755);
            std::string path = dir + "/session.json";
            std::ofstream f(path);
            f << "[";
            auto ids = tab_manager_.GetTabIds();
            bool first = true;
            for (int id : ids) {
                const TabInfo* info = tab_manager_.GetTab(id);
                if (info && !info->pinned && !info->url.empty() && info->url != "about:blank" && info->url.find("orb://") != 0) {
                    if (!first) f << ",";
                    // Simple JSON escape
                    std::string url = info->url;
                    std::string title = info->title;
                    for (auto* s : {&url, &title}) {
                        std::string out;
                        for (char c : *s) {
                            if (c == '"') out += "\\\"";
                            else if (c == '\\') out += "\\\\";
                            else out += c;
                        }
                        *s = out;
                    }
                    f << "{\"url\":\"" << url << "\",\"title\":\"" << title << "\"}";
                    first = false;
                }
            }
            f << "]";
        }
    }

    auto ids = tab_manager_.GetTabIds();
    for (int id : ids) {
        auto browser = tab_manager_.GetBrowser(id);
        if (browser) browser->GetHost()->CloseBrowser(true);
    }
    if (sidebar_browser_) {
        sidebar_browser_->GetHost()->CloseBrowser(true);
        sidebar_browser_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(paint_mutex_);
    if (sidebar_surface_) { cairo_surface_destroy(sidebar_surface_); sidebar_surface_ = nullptr; }
    if (tab_surface_) { cairo_surface_destroy(tab_surface_); tab_surface_ = nullptr; }
    if (split_surface_) { cairo_surface_destroy(split_surface_); split_surface_ = nullptr; }
}

void BrowserWindow::SetSplitView(int second_tab_id, bool horizontal) {
    if (second_tab_id < 0) return;
    auto browser = tab_manager_.GetBrowser(second_tab_id);
    if (!browser) return;

    split_view_.active = true;
    split_view_.horizontal = horizontal;
    split_view_.second_tab_id = second_tab_id;
    split_view_.ratio = 0.5f;
    split_view_.dragging_handle = false;

    // The partner panel is on screen now.
    browser->GetHost()->WasHidden(false);

    // Notify both browsers to resize
    ForceResize();

    // Tell sidebar about split state
    if (client_) {
        std::string js = "if(typeof orb!=='undefined') orb.onSplitViewChanged(true, " +
            std::to_string(second_tab_id) + ");";
        auto sidebar = sidebar_browser_;
        if (sidebar && sidebar->GetMainFrame()) {
            sidebar->GetMainFrame()->ExecuteJavaScript(js, "orb://internal", 0);
        }
    }
}

void BrowserWindow::CloseSplitView() {
    if (!split_view_.active) return;

    const int second_tab_id = split_view_.second_tab_id;
    split_view_.active = false;
    split_view_.second_tab_id = -1;
    split_view_.dragging_handle = false;

    // The partner is off screen again unless it is the active tab.
    if (second_tab_id >= 0 && second_tab_id != tab_manager_.GetActiveTabId()) {
        auto second = tab_manager_.GetBrowser(second_tab_id);
        if (second) second->GetHost()->WasHidden(true);
    }

    // Clean up split surface
    {
        std::lock_guard<std::mutex> lock(paint_mutex_);
        if (split_surface_) {
            cairo_surface_destroy(split_surface_);
            split_surface_ = nullptr;
        }
        split_buf_w_ = split_buf_h_ = 0;
    }

    ForceResize();

    // Notify sidebar
    if (sidebar_browser_ && sidebar_browser_->GetMainFrame()) {
        sidebar_browser_->GetMainFrame()->ExecuteJavaScript(
            "if(typeof orb!=='undefined') orb.onSplitViewChanged(false, -1);",
            "orb://internal", 0);
    }
}
