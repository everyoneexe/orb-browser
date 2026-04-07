#include "browser/browser_app.h"
#include "browser/browser_client.h"
#include "browser/browser_window.h"
#include "browser/scheme_handler.h"
#include "blocker/filter_rules.h"
#include "include/cef_browser.h"
#include "include/cef_path_util.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_helpers.h"

#include "include/cef_command_line.h"

#include <string>

BrowserApp::BrowserApp() {}

void BrowserApp::OnBeforeCommandLineProcessing(
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line) {
    // OSR handles its own compositing, but allow GPU for WebGL etc.
    command_line->AppendSwitch("disable-gpu-compositing");
    command_line->AppendSwitch("in-process-gpu");
    // Single-process: renderer runs in-process (avoids subprocess crashes)
    command_line->AppendSwitch("single-process");
}

void BrowserApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
    registrar->AddCustomScheme("orb",
        CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
        CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}

void BrowserApp::OnContextInitialized() {
    CEF_REQUIRE_UI_THREAD();

    // Get executable directory for resource paths
    std::string exe_dir;
    CefString cef_exe_dir;
    if (CefGetPath(PK_DIR_EXE, cef_exe_dir)) {
        exe_dir = cef_exe_dir.ToString();
    } else {
        exe_dir = ".";
    }

    // Register orb:// scheme handlers
    auto* factory = new OrbSchemeHandlerFactory(exe_dir + "/ui");
    CefRegisterSchemeHandlerFactory("orb", "chrome", factory);
    CefRegisterSchemeHandlerFactory("orb", "newtab", factory);
    CefRegisterSchemeHandlerFactory("orb", "history", factory);
    CefRegisterSchemeHandlerFactory("orb", "downloads", factory);
    CefRegisterSchemeHandlerFactory("orb", "gpu", factory);

    // Load ad blocker filter rules
    auto filter_rules = std::make_shared<FilterRuleSet>();
    filter_rules->Load(exe_dir + "/resources/easylist.txt");
    filter_rules->Load(exe_dir + "/resources/easyprivacy.txt");

    // Create browser window
    BrowserWindow::Create(filter_rules);
}

void BrowserApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> context) {
    if (!renderer_message_router_) {
        CefMessageRouterConfig config;
        config.js_query_function = "cefQuery";
        config.js_cancel_function = "cefQueryCancel";
        renderer_message_router_ = CefMessageRouterRendererSide::Create(config);
    }
    renderer_message_router_->OnContextCreated(browser, frame, context);
}

void BrowserApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
    if (renderer_message_router_) {
        renderer_message_router_->OnContextReleased(browser, frame, context);
    }
}

bool BrowserApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                          CefRefPtr<CefFrame> frame,
                                          CefProcessId source_process,
                                          CefRefPtr<CefProcessMessage> message) {
    if (renderer_message_router_) {
        return renderer_message_router_->OnProcessMessageReceived(browser, frame,
                                                                   source_process, message);
    }
    return false;
}
