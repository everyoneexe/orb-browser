#pragma once

#include "browser/tab_manager.h"
#include "include/wrapper/cef_message_router.h"

class BrowserWindow;

class OrbQueryHandler : public CefMessageRouterBrowserSide::Handler {
public:
    OrbQueryHandler(TabManager* tab_manager, BrowserWindow* window);

    bool OnQuery(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int64_t query_id,
                 const CefString& request,
                 bool persistent,
                 CefRefPtr<Callback> callback) override;

    void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         int64_t query_id) override;

private:
    TabManager* tab_manager_;
    BrowserWindow* window_;
};
