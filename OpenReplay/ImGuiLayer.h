#pragma once
#include <windows.h>

namespace ImGuiLayer {
    bool init(HWND hwnd);
    void shutdown();
    void beginFrame();
    void render();
    void onResize(int width, int height);
    bool handleEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
}
