#include "browser/browser_app.h"
#include "include/cef_app.h"

#include <gtk/gtk.h>
#include <cstdlib>
#include <string>

int main(int argc, char* argv[]) {
    // Force X11 backend — CEF requires X11 on Linux
    setenv("GDK_BACKEND", "x11", 1);

    CefMainArgs main_args(argc, argv);
    CefRefPtr<BrowserApp> app(new BrowserApp);

    // Subprocesses (renderer, utility, GPU) must exit here.
    // GTK init MUST come after this — subprocesses hang on gtk_init.
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    // Only the browser process reaches here — safe to init GTK
    gtk_init(&argc, &argv);

    // Persistent cache path for Widevine registration etc.
    std::string cache_path;
    const char* home = getenv("HOME");
    if (home) {
        cache_path = std::string(home) + "/.cache/orb-browser";
    } else {
        cache_path = "/tmp/orb-browser-cache";
    }

    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = false;
    settings.windowless_rendering_enabled = true;
    CefString(&settings.locale).FromASCII("en-US");
    CefString(&settings.root_cache_path).FromString(cache_path);
    CefString(&settings.cache_path).FromString(cache_path + "/Default");

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        return 1;
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}
