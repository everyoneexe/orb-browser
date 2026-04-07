#pragma once

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/wrapper/cef_message_router.h"

class BrowserApp : public CefApp,
                   public CefBrowserProcessHandler,
                   public CefRenderProcessHandler {
public:
    BrowserApp();

    // CefApp
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;

    // CefBrowserProcessHandler
    void OnContextInitialized() override;

    // CefRenderProcessHandler
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;
    void OnContextReleased(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

private:
    CefRefPtr<CefMessageRouterRendererSide> renderer_message_router_;

    IMPLEMENT_REFCOUNTING(BrowserApp);
    DISALLOW_COPY_AND_ASSIGN(BrowserApp);
};
