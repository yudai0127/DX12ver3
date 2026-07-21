#include <windows.h>
#include <crtdbg.h>
#include <time.h>
#include "Framework/Framework.h"
#include <combaseapi.h>

int WINAPI WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev_instance,
    _In_ LPSTR cmd_line, _In_ int cmd_show)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    srand(static_cast<unsigned int>(time(nullptr)));

#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    //_CrtSetBreakAlloc(####);
#endif

    FrameworkConfig config;
    config.title = L"DX12";
    config.width = 1280;
    config.height = 720;
    config.frameCount = 2;
    config.vsync = true;

    // Žö‹Æ‚Ì framework framework(hwnd); framework.run(); ‚Æ“¯‚¶Š´Šo
    Framework framework;
    int result = framework.Run(config);

    CoUninitialize();
    return result;
}