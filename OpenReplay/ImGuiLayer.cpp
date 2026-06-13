#include "ImGuiLayer.h"
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

static bool createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;
    hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

bool ImGuiLayer::init(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT width = rc.right - rc.left;
    UINT height = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, featureLevels, 1,
        D3D11_SDK_VERSION, &scd, &g_swapChain,
        &g_device, &featureLevel, &g_context);
    if (FAILED(hr)) return false;

    if (!createRenderTarget()) {
        shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    if (!ImGui_ImplWin32_Init(hwnd)) {
        shutdown();
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        shutdown();
        return false;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]  = ImVec4(0.29f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_TitleBg]        = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBgActive]  = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_MenuBarBg]      = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_Header]         = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_HeaderHovered]  = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_HeaderActive]   = ImVec4(0.29f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_Button]         = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_ButtonActive]   = ImVec4(0.29f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_Tab]            = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered]     = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    colors[ImGuiCol_TabActive]      = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TabUnfocused]   = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_Text]           = ImVec4(0.91f, 0.91f, 0.93f, 1.00f);
    colors[ImGuiCol_TextDisabled]   = ImVec4(0.53f, 0.53f, 0.59f, 1.00f);
    colors[ImGuiCol_Border]         = ImVec4(0.23f, 0.23f, 0.27f, 1.00f);
    colors[ImGuiCol_Separator]      = ImVec4(0.23f, 0.23f, 0.27f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]    = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]  = ImVec4(0.23f, 0.23f, 0.27f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.29f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.40f, 0.40f, 0.46f, 1.00f);
    colors[ImGuiCol_PopupBg]        = ImVec4(0.12f, 0.12f, 0.14f, 0.94f);
    colors[ImGuiCol_CheckMark]      = ImVec4(0.29f, 0.62f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]     = ImVec4(0.29f, 0.62f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.42f, 0.70f, 1.00f, 1.00f);

    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowRounding = 6.0f;
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 12.0f;
    style.ScrollbarSize = 14.0f;

    return true;
}

void ImGuiLayer::shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render() {
    ImGui::Render();
    float clearColor[4] = { 0.08f, 0.08f, 0.09f, 1.00f };
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(1, 0);
}

void ImGuiLayer::onResize(int width, int height) {
    if (!g_swapChain) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

bool ImGuiLayer::handleEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
}
