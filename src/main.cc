#include "browser/browser_app.h"
#include "include/cef_app.h"

#include <gtk/gtk.h>

int main(int argc, char* argv[]) {
    // Force X11 backend — CEF requires X11 on Linux
    setenv("GDK_BACKEND", "x11", 1);

    // Init GTK
    gtk_init(&argc, &argv);

    CefMainArgs main_args(argc, argv);
    CefRefPtr<BrowserApp> app(new BrowserApp);

    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = false;
    settings.windowless_rendering_enabled = true;
    CefString(&settings.locale).FromASCII("en-US");
    CefString(&settings.root_cache_path).FromASCII("/tmp/orb-browser-cache");

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        return 1;
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}
