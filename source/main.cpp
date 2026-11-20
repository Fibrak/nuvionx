// NuvioNX - unofficial native Nintendo Switch port of NuvioTV.
// Licensed under the GNU GPL v3.0 (see LICENSE).
#include <switch.h>

#include <cstdio>

#include "app.hpp"

static void showFatal(const char* msg) {
    consoleInit(nullptr);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\n  NuvioNX failed to start.\n\n  %s\n\n", msg);
    printf("  Tip: launch from a game's title (hold R while starting a game)\n");
    printf("  rather than from the Album, so the app gets full memory.\n\n");
    printf("  Press + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // Applet (Album) mode has too little memory for video playback.
    bool appletMode = appletGetAppletType() != AppletType_Application &&
                      appletGetAppletType() != AppletType_SystemApplication;

    App& app = App::get();
    bool ok = app.init();
    if (ok && appletMode) app.toast("Album mode detected: launch via a game (hold R) for reliable playback");
    if (ok) app.run();
    app.shutdown();
    if (!ok) showFatal("Graphics initialisation failed (SDL / system font).");
    return 0;
}
