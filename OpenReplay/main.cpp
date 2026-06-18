#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <ctime>
#include <cstring>
#include <vector>
#include <cstdint>

#include "OpenReplay.h"
#include "Muxer.h"
#include "Locale.h"
#include "ClipManager.h"
#include "Overlay.h"
#include "UI.h"
#include "json.hpp"

#ifdef HAS_IMGUI
#include "ImGuiLayer.h"
#endif

#ifdef HAS_IMGUI
#include <imgui.h>
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

using json = nlohmann::json;
namespace fs = std::filesystem;

OpenReplay::OpenReplayEngine* g_engine = nullptr;
OpenReplay::RecorderConfig g_config;
std::mutex g_configMutex;
json g_cfgJson;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_saving{false};
std::string g_threadResultMsg;
std::atomic<float> g_saveProgress{0.0f};
std::atomic<bool> g_saveSuccess{false};
std::atomic<bool> g_engineOk{false};
bool g_isHdrActive = false;
bool g_dotEnabled = true;
std::vector<OpenReplay::AudioCapture::DeviceInfo> g_audioDevices, g_micDevices;

int g_hotkeySaveId = 1;
int g_hotkeyToggleId = 2;
int g_hotkeyPanelId = 3;
int g_hotkeyScreenshotId = 4;
UINT g_hotkeySaveMod = MOD_CONTROL | MOD_SHIFT;
UINT g_hotkeySaveKey = 'R';
UINT g_hotkeyToggleMod = MOD_CONTROL | MOD_SHIFT;
UINT g_hotkeyToggleKey = 'S';
UINT g_hotkeyPanelMod = MOD_ALT;
UINT g_hotkeyPanelKey = 'G';
UINT g_hotkeyScreenshotMod = MOD_CONTROL | MOD_SHIFT;
UINT g_hotkeyScreenshotKey = 'X';

int g_screenshotFormat = 0; // 0=PNG, 1=JPEG
int g_screenshotQuality = 90;

HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_trayIcon = {};
bool g_trayAdded = false;
extern const UINT WM_TRAY_NOTIFY = WM_APP + 10;
extern const UINT WM_APP_SAVE_DONE = WM_APP + 1;
extern const UINT WM_APP_UPDATE_STATS = WM_APP + 2;

std::string g_saveDirectory = ".";
int g_defaultClipDuration = 300;
int g_audioOutputFormatIdx = 0;

std::string g_rtmpUrl;

ClipManager g_clipMgr;
std::vector<fs::path> g_clipList;
std::vector<std::string> g_clipDisplayTexts;
int g_selectedClip = -1;

std::thread g_saveThread;
int g_clipsSaved = 0;
std::string g_lastSaveResult;

OpenReplay::ProfileManager g_profiles;
std::vector<std::string> g_profileNames;
int g_selectedProfile = -1;

int g_qualityPreset = 2;
int g_prevQualityPreset = 2;

bool g_showProfileDialog = false;
char g_profileNameBuf[256] = {};

int g_capturingHotkeyFor = 0;
bool g_panelActive = false;
RECT g_savedWindowRect = {};
int g_panelWidth = 120;
int g_panelHeightPct = 100;
bool g_panelAutoScaleY = true;

void closePanel(HWND hwnd) {
    if (!g_panelActive) return;
    g_panelActive = false;
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style |= WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
    SetWindowLong(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void saveCfg();

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"

void takeScreenshot() {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, vw, vh);
    SelectObject(hdcMem, hbm);
    BitBlt(hdcMem, 0, 0, vw, vh, hdcScreen, vx, vy, SRCCOPY);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(vw * vh * 4);
    GetDIBits(hdcMem, hbm, 0, vh, pixels.data(), &bmi, DIB_RGB_COLORS);

    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    for (int i = 0; i < vw * vh; i++) {
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm t;
    localtime_s(&t, &now);
    char name[256];
    snprintf(name, sizeof(name), "%s/screenshot_%04d%02d%02d_%02d%02d%02d.%s",
             g_saveDirectory.c_str(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec,
             g_screenshotFormat == 1 ? "jpg" : "png");

    if (g_screenshotFormat == 1) {
        std::vector<uint8_t> rgb(vw * vh * 3);
        for (int i = 0; i < vw * vh; i++) {
            rgb[i * 3 + 0] = pixels[i * 4 + 0];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 2];
        }
        stbi_write_jpg(name, vw, vh, 3, rgb.data(), g_screenshotQuality);
    } else {
        stbi_write_png(name, vw, vh, 4, pixels.data(), vw * 4);
    }
    showToast(L"Screenshot Saved", L"");
}

void applyHotkeyBindings(HWND hwnd) {
    UnregisterHotKey(hwnd, g_hotkeySaveId);
    UnregisterHotKey(hwnd, g_hotkeyToggleId);
    UnregisterHotKey(hwnd, g_hotkeyPanelId);
    UnregisterHotKey(hwnd, g_hotkeyScreenshotId);
    RegisterHotKey(hwnd, g_hotkeySaveId, g_hotkeySaveMod, g_hotkeySaveKey);
    RegisterHotKey(hwnd, g_hotkeyToggleId, g_hotkeyToggleMod, g_hotkeyToggleKey);
    RegisterHotKey(hwnd, g_hotkeyPanelId, g_hotkeyPanelMod, g_hotkeyPanelKey);
    RegisterHotKey(hwnd, g_hotkeyScreenshotId, g_hotkeyScreenshotMod, g_hotkeyScreenshotKey);
    saveCfg();
}

static void loadCfg() {
    std::ifstream cf("config.json");
    if (!cf.is_open()) return;
    try {
        g_cfgJson = json::parse(cf);
        OpenReplay::configFromJson(g_cfgJson, g_config);
        g_dotEnabled = g_config.showRecordingDot;
        overlaySetCorner(g_cfgJson.value("overlay_corner", 0));
        overlaySetSize(g_cfgJson.value("overlay_size", 14));
        overlaySetAlpha(g_cfgJson.value("overlay_alpha", 200));
        overlaySetShowFps(g_cfgJson.value("overlay_show_fps", false));
        overlaySetColor((COLORREF)g_cfgJson.value("overlay_color", (unsigned int)RGB(0xe6, 0x4a, 0x4a)));
        g_hotkeySaveMod = g_cfgJson.value("hotkey_save_mod", MOD_CONTROL | MOD_SHIFT);
        g_hotkeySaveKey = g_cfgJson.value("hotkey_save_key", 'R');
        g_hotkeyToggleMod = g_cfgJson.value("hotkey_toggle_mod", MOD_CONTROL | MOD_SHIFT);
        g_hotkeyToggleKey = g_cfgJson.value("hotkey_toggle_key", 'S');
        g_hotkeyPanelMod = g_cfgJson.value("hotkey_panel_mod", MOD_ALT);
        g_hotkeyPanelKey = g_cfgJson.value("hotkey_panel_key", 'G');
        g_hotkeyScreenshotMod = g_cfgJson.value("hotkey_screenshot_mod", MOD_CONTROL | MOD_SHIFT);
        g_hotkeyScreenshotKey = g_cfgJson.value("hotkey_screenshot_key", 'X');
        g_screenshotFormat = g_cfgJson.value("screenshot_format", 0);
        g_screenshotQuality = g_cfgJson.value("screenshot_quality", 90);
        g_saveDirectory = g_cfgJson.value("save_directory", ".");
        g_defaultClipDuration = g_cfgJson.value("default_clip_duration", 300);
        g_audioOutputFormatIdx = g_cfgJson.value("audio_output_format", 0);
        g_rtmpUrl = g_cfgJson.value("rtmp_url", "");
        g_panelWidth = g_cfgJson.value("panel_width", 120);
        g_panelHeightPct = g_cfgJson.value("panel_height_pct", 100);
        g_panelAutoScaleY = g_cfgJson.value("panel_autoscale_y", true);
    } catch (const std::exception&) {
        std::cerr << "[Config] parse failed\n";
    }
}

static void saveCfg() {
    OpenReplay::configToJson(g_config, g_cfgJson);
    g_cfgJson["save_directory"] = g_saveDirectory;
    g_cfgJson["default_clip_duration"] = g_defaultClipDuration;
    g_cfgJson["buffer_size_mb"] = g_config.bufferSizeMB;
    g_cfgJson["output_format"] = (int)g_config.outputFormat;

    g_cfgJson["audio_output_format"] = g_audioOutputFormatIdx;
    g_cfgJson["capture_monitor"] = g_config.captureMonitor;
    g_cfgJson["rtmp_url"] = g_rtmpUrl;
    g_cfgJson["overlay_corner"] = overlayCorner();
    g_cfgJson["overlay_size"] = overlaySize();
    g_cfgJson["overlay_alpha"] = overlayAlpha();
    g_cfgJson["overlay_show_fps"] = overlayShowFps();
    g_cfgJson["overlay_color"] = (unsigned int)overlayColor();
    g_cfgJson["hotkey_save_mod"] = (int)g_hotkeySaveMod;
    g_cfgJson["hotkey_save_key"] = (int)g_hotkeySaveKey;
    g_cfgJson["hotkey_toggle_mod"] = (int)g_hotkeyToggleMod;
    g_cfgJson["hotkey_toggle_key"] = (int)g_hotkeyToggleKey;
    g_cfgJson["hotkey_panel_mod"] = (int)g_hotkeyPanelMod;
    g_cfgJson["hotkey_panel_key"] = (int)g_hotkeyPanelKey;
    g_cfgJson["hotkey_screenshot_mod"] = (int)g_hotkeyScreenshotMod;
    g_cfgJson["hotkey_screenshot_key"] = (int)g_hotkeyScreenshotKey;
    g_cfgJson["screenshot_format"] = g_screenshotFormat;
    g_cfgJson["screenshot_quality"] = g_screenshotQuality;
    g_cfgJson["panel_width"] = g_panelWidth;
    g_cfgJson["panel_height_pct"] = g_panelHeightPct;
    g_cfgJson["panel_autoscale_y"] = g_panelAutoScaleY;
    std::ofstream cf("config.json");
    if (cf.is_open()) cf << g_cfgJson.dump(4);
}

using OpenReplay::audioFormatFromIndex;

static void loadProfiles() { g_profiles.load("profiles.json"); }
static void saveProfiles() { g_profiles.save("profiles.json"); }
void applyProfile(const std::string& name) { g_profiles.apply(name, g_config); }
void saveCurrentProfile(const std::string& name) { g_profiles.saveCurrent(name, g_config); saveProfiles(); }

static void playSaveSound() {
    MessageBeep(MB_ICONINFORMATION);
}

void showToast(const wchar_t* title, const wchar_t* message) {
    if (!g_trayAdded) return;
    memset(&g_trayIcon, 0, sizeof(g_trayIcon));
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = g_hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_INFO;
    wcscpy_s(g_trayIcon.szInfoTitle, title);
    wcscpy_s(g_trayIcon.szInfo, message);
    g_trayIcon.dwInfoFlags = NIIF_INFO;
    g_trayIcon.uTimeout = 4000;
    Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
}

static void initTrayIcon(HWND hwnd) {
    memset(&g_trayIcon, 0, sizeof(g_trayIcon));
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_trayIcon.uCallbackMessage = WM_TRAY_NOTIFY;
    g_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"OpenReplay");
    g_trayIcon.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    g_trayAdded = true;
}

static bool saveToFile(HWND hwnd) {
    bool wasRecording = g_recording.load();
    if (wasRecording) {
        g_engine->stopCapture();
        g_recording.store(false);
        overlayShow(false);
    }

    OpenReplay::RecorderConfig cfg;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        cfg = g_config;
    }

    if (!cfg.enableVideo) {
        cfg.outputFormat = audioFormatFromIndex(g_audioOutputFormatIdx);
        if (!g_engine->hasPendingData()) {
            if (wasRecording && g_engineOk) {
                g_engine->startCapture();
                g_recording.store(true);
                overlayShow(true);
            }
            g_threadResultMsg = "No audio data captured";
            return false;
        }
    }

    g_engine->setConfig(cfg);
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = cfg;
    }

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm; localtime_s(&tm, &tt);
    char fn[64]; strftime(fn, sizeof(fn), "OpenReplay_%Y%m%d_%H%M%S", &tm);
    std::string ext = OpenReplay::Muxer::extension(g_config.outputFormat);
    std::string fname = std::string(fn) + "." + ext;
    fs::path out = fs::path(g_saveDirectory) / fname;
    int clipDur = std::max(1, g_defaultClipDuration);

    bool ok = g_engine->saveLastMoments(out.string().c_str(), clipDur,
        [](float p) {
            g_saveProgress.store(p);
            if (g_hwnd) PostMessageA(g_hwnd, WM_APP_UPDATE_STATS, 0, 0);
        });

    g_saveSuccess.store(ok);
    if (ok) g_clipsSaved++;

    if (wasRecording && g_engineOk) {
        g_engine->startCapture();
        g_recording.store(true);
        overlayShow(true);
    }

    g_threadResultMsg = out.string();
    if (ok) g_threadResultMsg += " (clip #" + std::to_string(g_clipsSaved) + ")";
    return ok;
}

bool streamToUrl() {
    bool wasRecording = g_recording.load();
    if (wasRecording) {
        g_engine->stopCapture();
        g_recording.store(false);
        overlayShow(false);
    }
    bool ok = g_engine->saveLastMoments(g_rtmpUrl.c_str(),
        std::max(1, g_defaultClipDuration),
        [](float p) {
            g_saveProgress.store(p);
            if (g_hwnd) PostMessageA(g_hwnd, WM_APP_UPDATE_STATS, 0, 0);
        });
    g_saveSuccess.store(ok);
    if (wasRecording && g_engineOk) {
        g_engine->startCapture();
        g_recording.store(true);
        overlayShow(true);
    }
    g_threadResultMsg = ok
        ? std::string("Streamed to: ") + g_rtmpUrl
        : "Stream failed";
    return ok;
}

void saveClip(HWND hwnd) {
    if (g_saving.load() || !g_engine || !g_engineOk) return;

    if (!g_config.enableVideo && !g_recording.load()) {
        if (!g_engine->hasPendingData()) {
            showToast(L"Nothing to Save", L"Enable audio, start recording, then try again.");
            return;
        }
    }
    g_saving.store(true);
    g_saveProgress.store(0.0f);
    g_saveSuccess.store(false);
    if (g_saveThread.joinable()) g_saveThread.join();
    g_saveThread = std::thread([hwnd]() {
        bool ok = false;
        __try {
            ok = saveToFile(hwnd);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_threadResultMsg = "Save failed: unexpected error";
        }
        g_saving.store(false);
        g_lastSaveResult = g_threadResultMsg.c_str();
        PostMessageA(hwnd, WM_APP_SAVE_DONE, (WPARAM)ok, 0);
    });
}


static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
#ifdef HAS_IMGUI
    if (ImGuiLayer::handleEvent(hwnd, msg, wParam, lParam))
        return 1;
#endif

    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
        RegisterHotKey(hwnd, g_hotkeySaveId, g_hotkeySaveMod, g_hotkeySaveKey);
        RegisterHotKey(hwnd, g_hotkeyToggleId, g_hotkeyToggleMod, g_hotkeyToggleKey);
        RegisterHotKey(hwnd, g_hotkeyPanelId, g_hotkeyPanelMod, g_hotkeyPanelKey);
        RegisterHotKey(hwnd, g_hotkeyScreenshotId, g_hotkeyScreenshotMod, g_hotkeyScreenshotKey);
        overlayCreate();
        initTrayIcon(hwnd);
#ifdef HAS_IMGUI
        if (!ImGuiLayer::init(hwnd)) {
            std::cerr << "ImGuiLayer::init failed\n";
        }
#endif
        refreshClipList();
        refreshProfileCombo();
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        HDC mdc = GetDC(hwnd);
        int mdpi = GetDeviceCaps(mdc, LOGPIXELSY);
        ReleaseDC(hwnd, mdc);
        mmi->ptMinTrackSize.x = 580 * mdpi / 96;
        mmi->ptMinTrackSize.y = 600 * mdpi / 96;
        break;
    }

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED && g_config.minimizeToTray) {
            ShowWindow(hwnd, SW_HIDE);
        }
        if (wParam != SIZE_MINIMIZED) {
            int cw = LOWORD(lParam), ch = HIWORD(lParam);
            if (cw > 0 && ch > 0) {
#ifdef HAS_IMGUI
                ImGuiLayer::onResize(cw, ch);
#endif
            }
        }
        break;
    }

    case WM_HOTKEY:
        if (wParam == g_hotkeySaveId) {
            saveClip(hwnd);
        } else if (wParam == g_hotkeyScreenshotId) {
            takeScreenshot();
        } else if (wParam == g_hotkeyPanelId) {
            if (g_panelActive) {
                closePanel(hwnd);
                ShowWindow(hwnd, SW_HIDE);
            } else {
                g_panelActive = true;
                GetWindowRect(hwnd, &g_savedWindowRect);
                HDC sdc = GetDC(nullptr);
                int dpiScale = GetDeviceCaps(sdc, LOGPIXELSY);
                ReleaseDC(nullptr, sdc);
                int pw = g_panelWidth * dpiScale / 96;
                HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfoW(mon, &mi);
                int ph, py;
                if (g_panelAutoScaleY) {
                    ph = (mi.rcWork.bottom - mi.rcWork.top);
                    py = mi.rcWork.top;
                } else {
                    ph = (mi.rcWork.bottom - mi.rcWork.top) * g_panelHeightPct / 100;
                    py = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - ph) / 2;
                }
                LONG style = GetWindowLong(hwnd, GWL_STYLE);
                style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
                SetWindowLong(hwnd, GWL_STYLE, style);
                SetWindowPos(hwnd, HWND_TOPMOST, mi.rcWork.left, py, pw, ph,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
            }
        } else if (wParam == g_hotkeyToggleId) {
            if (g_recording.load()) {
                g_engine->stopCapture(); g_recording.store(false);
                overlayShow(false);
            } else {
                if (!g_engineOk) break;
                g_engine->setConfig(g_config);
                g_engine->startCapture();
                g_recording.store(true);
                overlayShow(true);
            }
        }
        break;

    case WM_TRAY_NOTIFY:
        if (lParam == WM_LBUTTONDBLCLK) {
            if (g_panelActive) closePanel(hwnd);
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU popup = CreatePopupMenu();
            if (g_recording.load()) {
                AppendMenuA(popup, MF_STRING, 101, "Stop Recording");
            } else {
                AppendMenuA(popup, MF_STRING, 100, "Start Recording");
            }
            AppendMenuA(popup, MF_STRING, 102, "Save Last Clip");
            AppendMenuA(popup, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(popup, MF_STRING, 999, "Show Window");
            AppendMenuA(popup, MF_STRING, 1000, "Quit");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(popup);
            if (cmd == 100) {
                if (!g_engineOk || g_recording.load()) break;
                g_engine->setConfig(g_config);
                g_engine->startCapture();
                g_recording.store(true);
                overlayShow(true);
            } else if (cmd == 101) {
                if (g_engineOk && g_recording.load()) {
                    g_engine->stopCapture(); g_recording.store(false);
                    overlayShow(false);
                }
            } else if (cmd == 102) {
                saveClip(hwnd);
            } else if (cmd == 999) {
                ShowWindow(hwnd, SW_SHOW);
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            } else if (cmd == 1000) {
                PostMessageA(hwnd, WM_CLOSE, 0, 0);
            }
        }
        break;

    case WM_APP_SAVE_DONE: {
        g_saveProgress.store(wParam ? 1.0f : 0.0f);
        if (wParam) {
            playSaveSound();
            int wn = MultiByteToWideChar(CP_UTF8, 0, g_lastSaveResult.c_str(), -1, nullptr, 0);
            std::wstring wpath(wn, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, g_lastSaveResult.c_str(), -1, wpath.data(), wn);
            size_t pos = wpath.find_last_of(L"\\/");
            std::wstring fname = (pos != std::wstring::npos) ? wpath.substr(pos + 1) : wpath;
            showToast(L"Clip Saved", fname.c_str());
        } else {
            std::wstring errMsg = L"Save failed";
            int wn = MultiByteToWideChar(CP_UTF8, 0, g_lastSaveResult.c_str(), -1, nullptr, 0);
            if (wn > 1) {
                std::wstring werr(wn, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, g_lastSaveResult.c_str(), -1, werr.data(), wn);
                errMsg = werr;
            }
            showToast(L"Save Failed", errMsg.c_str());
            MessageBoxA(hwnd, g_lastSaveResult.c_str(), "Save Error", MB_ICONERROR);
        }
        refreshClipList();
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case WM_DESTROY:
        overlayShow(false);
        if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        if (g_saveThread.joinable()) g_saveThread.join();
        if (g_recording.load()) {
            if (g_engine) g_engine->stopCapture();
        }
#ifdef HAS_IMGUI
        ImGuiLayer::shutdown();
#endif
        overlayDestroy();
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

class TeeBuf : public std::streambuf {
    std::ofstream m_file;
    std::streambuf* m_cerrOrig;
    std::streambuf* m_coutOrig;
public:
    TeeBuf(const char* path)
        : m_cerrOrig(std::cerr.rdbuf())
        , m_coutOrig(std::cout.rdbuf())
    {
        m_file.open(path, std::ios::app);
        if (m_file.is_open()) {
            auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm; localtime_s(&tm, &t);
            char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            m_file << "\n=== OpenReplay log started " << ts << " ===\n" << std::flush;
        }
        std::cerr.rdbuf(this);
        std::cout.rdbuf(this);
    }
    ~TeeBuf() {
        sync();
        std::cerr.rdbuf(m_cerrOrig);
        std::cout.rdbuf(m_coutOrig);
    }
protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (m_file.is_open()) m_file.write(s, n);
        m_coutOrig->sputn(s, n);
        return n;
    }
    int overflow(int c) override {
        if (m_file.is_open()) m_file.put((char)c);
        return m_coutOrig->sputc(c);
    }
    int sync() override {
        if (m_file.is_open()) m_file.flush();
        m_coutOrig->pubsync();
        return 0;
    }
};

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int show) {
    SetProcessDPIAware();

    char logPath[MAX_PATH];
    GetEnvironmentVariableA("APPDATA", logPath, MAX_PATH);
    strcat_s(logPath, "\\OpenReplay");
    CreateDirectoryA(logPath, nullptr);
    strcat_s(logPath, "\\log.txt");
    TeeBuf logTee(logPath);

    loadLocale("en.json");
    loadCfg();
    loadProfiles();

    if (g_config.enableVideo) {
        g_config.captureWidth = GetSystemMetrics(SM_CXSCREEN);
        g_config.captureHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    {
        OpenReplay::OpenReplayEngine engine;
        g_engine = &engine;
        g_engineOk = engine.init(g_config);

        g_audioDevices = OpenReplay::AudioCapture::enumerateDevices(OpenReplay::AudioCapture::Loopback);
        g_micDevices = OpenReplay::AudioCapture::enumerateDevices(OpenReplay::AudioCapture::Microphone);

        WNDCLASSA wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "OpenReplayWindow";
        wc.hbrBackground = nullptr;
        RegisterClassA(&wc);

        HDC sdc = GetDC(nullptr);
        int dpiScale = GetDeviceCaps(sdc, LOGPIXELSY);
        ReleaseDC(nullptr, sdc);
        int baseW = 520 * dpiScale / 96;
        int baseH = 480 * dpiScale / 96;
        RECT wr = { 0, 0, baseW, baseH };
        AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME, FALSE, 0);

        HWND hwnd = CreateWindowExA(0, "OpenReplayWindow", "OpenReplay",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            nullptr, nullptr, hInst, nullptr);
        if (!hwnd) return 1;

        ShowWindow(hwnd, show);

#ifdef HAS_IMGUI
        bool running = true;
        while (running) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = false; break; }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (!running) break;

            if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) {
                WaitMessage();
                continue;
            }

            renderUI();

            if (g_capturingHotkeyFor > 0) {
                ImGuiIO& io = ImGui::GetIO();
                int captured = 0;
                for (int kc = 'A'; kc <= 'Z'; kc++) {
                    int ik = (int)ImGuiKey_A + (kc - 'A');
                    if (ImGui::IsKeyPressed((ImGuiKey)ik, false)) { captured = kc; break; }
                }
                if (!captured) {
                    for (int kc = '0'; kc <= '9'; kc++) {
                        int ik = (int)ImGuiKey_0 + (kc - '0');
                        if (ImGui::IsKeyPressed((ImGuiKey)ik, false)) { captured = kc; break; }
                    }
                }
                if (captured) {
                    UINT mods = 0;
                    if (io.KeyCtrl) mods |= MOD_CONTROL;
                    if (io.KeyShift) mods |= MOD_SHIFT;
                    if (io.KeyAlt) mods |= MOD_ALT;
                    if (g_capturingHotkeyFor == 1) {
                        g_hotkeySaveMod = mods; g_hotkeySaveKey = (UINT)captured;
                    } else if (g_capturingHotkeyFor == 2) {
                        g_hotkeyToggleMod = mods; g_hotkeyToggleKey = (UINT)captured;
                    } else if (g_capturingHotkeyFor == 3) {
                        g_hotkeyPanelMod = mods; g_hotkeyPanelKey = (UINT)captured;
                    } else if (g_capturingHotkeyFor == 4) {
                        g_hotkeyScreenshotMod = mods; g_hotkeyScreenshotKey = (UINT)captured;
                    }
                    g_capturingHotkeyFor = 0;
                    applyHotkeyBindings(hwnd);
                }
            }
        }
#else
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
#endif

        if (g_recording.load()) engine.stopCapture();
        if (g_saveThread.joinable()) g_saveThread.join();
        UnregisterHotKey(hwnd, g_hotkeySaveId);
        UnregisterHotKey(hwnd, g_hotkeyToggleId);
        UnregisterHotKey(hwnd, g_hotkeyPanelId);
        UnregisterHotKey(hwnd, g_hotkeyScreenshotId);
    }

    return 0;
}
