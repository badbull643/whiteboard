#include "App.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // Optional first argument: a .wbd file to open (double-click / "Open with").
    std::wstring file;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1)
            file = argv[1];
        LocalFree(argv);
    }

    App app;
    if (!app.Init(hInstance, nCmdShow, file))
        return 1;
    return app.Run();
}
