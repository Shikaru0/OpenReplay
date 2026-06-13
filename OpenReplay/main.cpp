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

#include "OpenReplay.h"
#include "Muxer.h"
#include "Locale.h"
#include "ClipManager.h"
#include "Overlay.h"
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

static OpenReplay::OpenReplayEngine* g_engine = nullptr;
static OpenReplay::RecorderConfig g_config;
static std::mutex g_configMutex;
static json g_cfgJson;
static std::atomic<bool> g_recording{false};
static std::atomic<bool> g_saving{false};
static std::atomic<float> g_saveProgress{0.0f};
static std::atomic<bool> g_saveSuccess{false};
static std::atomic<bool> g_engineOk{false};
static bool g_isHdrActive = false;
static bool g_dotEnabled = true;
static std::vector<OpenReplay::AudioCapture::DeviceInfo> g_audioDevices, g_micDevices;

static int g_hotkeySaveId = 1;
static int g_hotkeyToggleId = 2;
static int g_hotkeyPanelId = 3;
static UINT g_hotkeySaveMod = MOD_CONTROL | MOD_SHIFT;
static UINT g_hotkeySaveKey = 'R';
static UINT g_hotkeyToggleMod = MOD_CONTROL | MOD_SHIFT;
static UINT g_hotkeyToggleKey = 'S';
static UINT g_hotkeyPanelMod = MOD_ALT;
static UINT g_hotkeyPanelKey = 'G';

static HWND g_hwnd = nullptr;
static NOTIFYICONDATAW g_trayIcon = {};
static bool g_trayAdded = false;
static const UINT WM_TRAY_NOTIFY = WM_APP + 10;
static const UINT WM_APP_SAVE_DONE = WM_APP + 1;
static const UINT WM_APP_UPDATE_STATS = WM_APP + 2;

static std::string g_saveDirectory = ".";
static int g_defaultClipDuration = 300;
static int g_audioOutputFormatIdx = 0;
static std::string g_rtmpUrl;

static ClipManager g_clipMgr;
static std::vector<fs::path> g_clipList;
static std::vector<std::string> g_clipDisplayTexts;
static int g_selectedClip = -1;

static std::thread g_saveThread;
static int g_clipsSaved = 0;
static std::string g_lastSaveResult;

static OpenReplay::ProfileManager g_profiles;
static std::vector<std::string> g_profileNames;
static int g_selectedProfile = -1;

static int g_qualityPreset = 2;
static int g_prevQualityPreset = 2;

static bool g_showProfileDialog = false;
static char g_profileNameBuf[256] = {};

static int g_capturingHotkeyFor = 0;
static bool g_panelActive = false;
static RECT g_savedWindowRect = {};
static int g_panelWidth = 120;
static int g_panelHeightPct = 100;
static bool g_panelAutoScaleY = true;

static std::string modStr(UINT m) {
    std::string r;
    if (m & MOD_CONTROL) r += "Ctrl+";
    if (m & MOD_SHIFT) r += "Shift+";
    if (m & MOD_ALT) r += "Alt+";
    return r;
}

static void saveCfg();

static void applyHotkeyBindings(HWND hwnd) {
    UnregisterHotKey(hwnd, g_hotkeySaveId);
    UnregisterHotKey(hwnd, g_hotkeyToggleId);
    UnregisterHotKey(hwnd, g_hotkeyPanelId);
    RegisterHotKey(hwnd, g_hotkeySaveId, g_hotkeySaveMod, g_hotkeySaveKey);
    RegisterHotKey(hwnd, g_hotkeyToggleId, g_hotkeyToggleMod, g_hotkeyToggleKey);
    RegisterHotKey(hwnd, g_hotkeyPanelId, g_hotkeyPanelMod, g_hotkeyPanelKey);
    saveCfg();
}

static void closePanel(HWND hwnd) {
    if (!g_panelActive) return;
    g_panelActive = false;
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style |= WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
    SetWindowLong(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void refreshClipList();
static void refreshProfileCombo();

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
    g_cfgJson["panel_width"] = g_panelWidth;
    g_cfgJson["panel_height_pct"] = g_panelHeightPct;
    g_cfgJson["panel_autoscale_y"] = g_panelAutoScaleY;
    std::ofstream cf("config.json");
    if (cf.is_open()) cf << g_cfgJson.dump(4);
}

using OpenReplay::formatFromIndex;
using OpenReplay::audioFormatFromIndex;
using OpenReplay::formatToIndex;

static void loadProfiles() { g_profiles.load("profiles.json"); }
static void saveProfiles() { g_profiles.save("profiles.json"); }
static void applyProfile(const std::string& name) { g_profiles.apply(name, g_config); }
static void saveCurrentProfile(const std::string& name) { g_profiles.saveCurrent(name, g_config); saveProfiles(); }

static void refreshProfileCombo() {
    g_profileNames = g_profiles.list();
    if (g_selectedProfile >= (int)g_profileNames.size())
        g_selectedProfile = -1;
}

static std::string statusText() {
    std::ostringstream ss;
    if (!g_engineOk && !g_recording.load()) return tr("Engine init failed");
    if (g_saving.load()) {
        ss << tr("Saving...") << " " << (int)(g_saveProgress.load() * 100) << "%";
        return ss.str();
    }
    if (g_recording.load()) {
        ss << "REC";
        auto s = g_engine ? g_engine->getStats() : OpenReplay::CaptureStats{};
        if (s.durationMs > 0)
            ss << "  " << (s.durationMs / 1000) << "s";
    } else {
        ss << tr("Ready");
    }
    ss << "  |  " << g_config.captureWidth << "x" << g_config.captureHeight
       << " @" << g_config.maxFPS << "fps"
       << "  |  " << (g_config.videoBitrate / 1000000) << "."
       << ((g_config.videoBitrate / 100000) % 10) << " Mbps"
       << "  |  " << (g_engine ? g_engine->getCodecName() : "none");
    if (g_isHdrActive) ss << "  HDR";
    return ss.str();
}

static std::string statsText() {
    if (!g_engine || !g_recording.load()) return "";
    auto s = g_engine->getStats();
    std::ostringstream ss;
    ss << tr("Frames") << ": " << s.framesCaptured.load()
       << "  |  FPS: " << (int)s.currentFps.load()
       << "  |  " << (s.durationMs / 1000) << "s"
       << "  |  " << tr("Audio pkts") << ": " << s.audioPackets.load();
    auto drops = s.framesDropped.load();
    if (drops > 0)
        ss << "  |  " << tr("Frame drops") << ": " << drops;
    return ss.str();
}

static void refreshClipList() {
    auto clips = g_clipMgr.enumerate(g_saveDirectory);
    g_clipList.clear();
    g_clipDisplayTexts.clear();
    for (auto& ci : clips) {
        g_clipList.push_back(ci.path);
        char item[512];
        snprintf(item, sizeof(item), "%s  [%s]  %s",
                 ci.dateStr.c_str(), ci.sizeStr.c_str(), ci.displayName.c_str());
        g_clipDisplayTexts.push_back(item);
    }
    if (g_selectedClip >= (int)g_clipList.size())
        g_selectedClip = -1;
}

static void playSaveSound() {
    Beep(880, 150);
    Sleep(100);
    Beep(1100, 200);
}

static void showToast(const wchar_t* title, const wchar_t* message) {
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

static void saveClip(HWND hwnd) {
    if (g_saving.load() || !g_engine || !g_engineOk) return;
    g_saving.store(true);
    g_saveProgress.store(0.0f);
    g_saveSuccess.store(false);
    if (g_saveThread.joinable()) g_saveThread.join();
    g_saveThread = std::thread([hwnd]() {
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

        g_saving.store(false);
        g_saveSuccess.store(ok);
        if (ok) g_clipsSaved++;

        if (wasRecording && g_engineOk) {
            g_engine->startCapture();
            g_recording.store(true);
            overlayShow(true);
        }

        g_lastSaveResult = out.string();
        if (ok) g_lastSaveResult += " (clip #" + std::to_string(g_clipsSaved) + ")";
        PostMessageA(hwnd, WM_APP_SAVE_DONE, (WPARAM)ok, 0);
    });
}

static void addGpuInfo(std::string& text) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            int w = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
            if (w > 0) {
                std::string gpuName(w, '\0');
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpuName.data(), w, nullptr, nullptr);
                gpuName.resize(w - 1);
                const char* vendor = "";
                if (desc.VendorId == 0x1002) vendor = "AMD";
                else if (desc.VendorId == 0x10DE) vendor = "NVIDIA";
                else if (desc.VendorId == 0x8086) vendor = "Intel";
                else if (desc.VendorId == 0x1414) vendor = "Microsoft";
                text += "GPU " + std::to_string(i) + ": " + vendor + " " + gpuName;
                text += " (" + std::to_string(desc.DedicatedVideoMemory / 1024 / 1024) + " MB)\n";
            }
        }
        adapter->Release();
    }
    factory->Release();
}

#ifdef HAS_IMGUI

static void applyQualityPreset(const char* name) {
    OpenReplay::applyQualityPreset(g_config, name);
}

static void drawRecordingTab() {
    if (ImGui::BeginChild("recordingScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 180);

    bool rec = g_recording.load();
    bool saving = g_saving.load();
    bool ok = g_engineOk.load();

    if (!ok) ImGui::BeginDisabled();

    if (saving) {
        ImGui::Button(tr("Saving..."), ImVec2(-1, 36));
    } else if (rec) {
        if (ImGui::Button(tr("Stop Recording"), ImVec2(-1, 36))) {
            if (g_config.autoSaveOnStop) {
                saveClip(g_hwnd);
            } else {
                g_engine->stopCapture(); g_recording.store(false);
                overlayShow(false);
            }
        }
    } else {
        if (ImGui::Button(tr("Start Recording"), ImVec2(-1, 36))) {
            g_engine->setConfig(g_config);
            g_engine->startCapture();
            g_recording.store(true);
            overlayShow(true);
        }
    }

    if (ImGui::Button(tr("Save Last Clip"), ImVec2(-1, 36)))
        saveClip(g_hwnd);

    if (ImGui::Button(tr("Stream to URL"), ImVec2(-1, 36)) && !saving) {
        g_saving.store(true);
        g_saveProgress.store(0.0f);
        g_saveSuccess.store(false);
        if (g_saveThread.joinable()) g_saveThread.join();
        g_saveThread = std::thread([]() {
            bool wasRecording = g_recording.load();
            if (wasRecording) {
                g_engine->stopCapture();
                g_recording.store(false);
                overlayShow(false);
            }
            bool streamOk = g_engine->saveLastMoments(g_rtmpUrl.c_str(), std::max(1, g_defaultClipDuration),
                [](float p) {
                    g_saveProgress.store(p);
                    if (g_hwnd) PostMessageA(g_hwnd, WM_APP_UPDATE_STATS, 0, 0);
                });
            g_saving.store(false);
            g_saveSuccess.store(streamOk);
            if (wasRecording && g_engineOk) {
                g_engine->startCapture();
                g_recording.store(true);
                overlayShow(true);
            }
            g_lastSaveResult = streamOk ? std::string("Streamed to: ") + g_rtmpUrl : "Stream failed";
            PostMessageA(g_hwnd, WM_APP_SAVE_DONE, (WPARAM)streamOk, 0);
        });
    }

    if (!ok) ImGui::EndDisabled();

    if (ImGui::Button(tr("Open Clips Folder"), ImVec2(-1, 36))) {
        std::string dir = g_saveDirectory;
        if (dir.empty() || dir == ".") {
            char cwd[MAX_PATH];
            GetCurrentDirectoryA(MAX_PATH, cwd);
            dir = cwd;
        }
        ShellExecuteA(g_hwnd, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
    }

    {
        ImGui::TextDisabled("%s: %s%c", tr("Save"), modStr(g_hotkeySaveMod).c_str(), g_hotkeySaveKey);
        ImGui::TextDisabled("%s: %s%c", tr("Rec"), modStr(g_hotkeyToggleMod).c_str(), g_hotkeyToggleKey);
        ImGui::TextDisabled("%s: %s%c", tr("Panel"), modStr(g_hotkeyPanelMod).c_str(), g_hotkeyPanelKey);
    }

    if (!g_lastSaveResult.empty()) {
        ImGui::Text("%s", g_lastSaveResult.c_str());
    }

    if (rec && g_engine) {
        auto s = g_engine->getStats();
        g_isHdrActive = s.isHdr.load();
        std::ostringstream ss;
        ss << tr("Frames") << ": " << s.framesCaptured.load()
           << "  |  FPS: " << (int)s.currentFps.load()
           << "  |  " << (s.durationMs / 1000) << "s"
           << "  |  " << tr("Audio pkts") << ": " << s.audioPackets.load();
        auto drops = s.framesDropped.load();
        if (drops > 0) ss << "  |  " << tr("Frame drops") << ": " << drops;
        ImGui::TextDisabled("%s", ss.str().c_str());
    }

    ImGui::NextColumn();

    ImGui::Text("%s", tr("Clips"));
    if (ImGui::BeginListBox("##clips", ImVec2(-1, -60))) {
        for (int i = 0; i < (int)g_clipDisplayTexts.size(); i++) {
            if (ImGui::Selectable(g_clipDisplayTexts[i].c_str(), g_selectedClip == i))
                g_selectedClip = i;
        }
        ImGui::EndListBox();
    }
    if (g_selectedClip >= 0 && g_selectedClip < (int)g_clipList.size()) {
        if (ImGui::Button(tr("Open"))) {
            ShellExecuteW(g_hwnd, L"open", g_clipList[g_selectedClip].wstring().c_str(), nullptr, nullptr, SW_SHOW);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Delete"))) {
            std::wstring ws = g_clipList[g_selectedClip].wstring();
            std::wstring msg = L"Delete this clip?\n" + ws;
            if (MessageBoxW(g_hwnd, msg.c_str(), L"Confirm Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
                fs::remove(g_clipList[g_selectedClip]);
                refreshClipList();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Folder"))) {
            std::string dir = g_clipList[g_selectedClip].parent_path().string();
            ShellExecuteA(g_hwnd, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
        }
    }

    ImGui::Columns(1);
    }
    ImGui::EndChild();
}

static void drawCustomizeTab() {
    if (ImGui::BeginChild("customizeScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 280);
        float cw = ImGui::GetColumnWidth() - 100;

        ImGui::Separator(); ImGui::Text("%s", tr("QUALITY"));
        ImGui::RadioButton(tr("Lossless"), &g_qualityPreset, 0); ImGui::SameLine();
        ImGui::RadioButton(tr("High"), &g_qualityPreset, 1); ImGui::SameLine();
        ImGui::RadioButton(tr("Standard"), &g_qualityPreset, 2); ImGui::SameLine();
        ImGui::RadioButton(tr("Stream"), &g_qualityPreset, 3);
        if (g_qualityPreset != g_prevQualityPreset) {
            g_prevQualityPreset = g_qualityPreset;
            const char* names[] = {"lossless", "high", "standard", "stream"};
            applyQualityPreset(names[g_qualityPreset]);
        }

        ImGui::Separator(); ImGui::Text("%s", tr("VIDEO"));
        ImGui::SetNextItemWidth(cw);
        ImGui::InputInt(tr("FPS"), &g_config.maxFPS);
        if (g_config.maxFPS < 1) g_config.maxFPS = 60;

        const char* resolutions[] = {"1920x1080", "2560x1440", "3840x2160", "1280x720", "Custom"};
        std::string curRes = std::to_string(g_config.captureWidth) + "x" + std::to_string(g_config.captureHeight);
        int resIdx = 0;
        for (int i = 0; i < 5; i++) {
            if (curRes == resolutions[i]) { resIdx = i; break; }
        }
        ImGui::SetNextItemWidth(cw);
        if (ImGui::Combo(tr("Resolution"), &resIdx, resolutions, 5)) {
            if (resIdx < 4) {
                sscanf_s(resolutions[resIdx], "%dx%d", &g_config.captureWidth, &g_config.captureHeight);
            }
        }

        int bitrateKbps = g_config.videoBitrate / 1000;
        ImGui::SetNextItemWidth(cw);
        if (ImGui::InputInt(tr("Video bitrate"), &bitrateKbps))
            g_config.videoBitrate = std::max(100, bitrateKbps) * 1000;

        ImGui::SetNextItemWidth(cw);
        ImGui::InputInt(tr("Keyframe (s)"), &g_config.keyframeIntervalSec);
        if (g_config.keyframeIntervalSec < 1) g_config.keyframeIntervalSec = 1;

        ImGui::SetNextItemWidth(cw);
        ImGui::InputInt(tr("VBV buffer (ms)"), &g_config.vbvBufferMs);
        if (g_config.vbvBufferMs < 100) g_config.vbvBufferMs = 100;

        ImGui::Separator(); ImGui::Text("%s", tr("ENCODER"));
        const char* presets[] = {"ultralowlatency", "lowlatency", "highquality", "transcoding"};
        int presetIdx = 0;
        for (int i = 0; i < 4; i++) {
            if (g_config.encoderPreset == presets[i]) { presetIdx = i; break; }
        }
        ImGui::SetNextItemWidth(cw);
        if (ImGui::Combo(tr("AMF preset"), &presetIdx, presets, 4))
            g_config.encoderPreset = presets[presetIdx];

        ImGui::Checkbox(tr("Enable pre-analysis (AMF)"), &g_config.enablePreAnalysis);
        ImGui::Checkbox(tr("Force constant framerate"), &g_config.forceCfr);

        ImGui::Separator(); ImGui::Text("%s", tr("AUDIO"));
        int audioKbps = g_config.audioBitrate / 1000;
        ImGui::SetNextItemWidth(cw);
        if (ImGui::InputInt(tr("Audio bitrate"), &audioKbps))
            g_config.audioBitrate = std::max(32, audioKbps) * 1000;

        ImGui::Checkbox(tr("Enable audio (loopback)"), &g_config.enableAudio);
        ImGui::Checkbox(tr("Enable microphone (requires restart)"), &g_config.enableMic);

        const char* audioCodecNames[] = {"MP3 (WMP compatible)", "AAC (better quality)"};
        ImGui::SetNextItemWidth(cw);
        ImGui::Combo(tr("Audio codec"), &g_config.audioCodec, audioCodecNames, 2);

        ImGui::NextColumn();

        ImGui::Separator(); ImGui::Text("%s", tr("OUTPUT"));
        float cw2 = ImGui::GetColumnWidth() - 100;

        const char* videoFormats[] = {"MP4 (H.264/HEVC)", "MKV (H.264/HEVC)", "WEBM (VP9/AV1)",
                                       "MOV (H.264/HEVC)", "AVI (Xvid/H.264)", "WAV (PCM)",
                                       "FLAC", "MP3", "OGG (Vorbis)"};
        int fmtIdx = formatToIndex(g_config.outputFormat);
        ImGui::SetNextItemWidth(cw2);
        if (ImGui::Combo(tr("Format"), &fmtIdx, videoFormats, 9))
            g_config.outputFormat = formatFromIndex(fmtIdx);

        const char* audioFormats[] = {"WAV (PCM)", "FLAC", "MP3", "OGG (Vorbis)"};
        ImGui::SetNextItemWidth(cw2);
        ImGui::Combo(tr("Audio-only fmt"), &g_audioOutputFormatIdx, audioFormats, 4);

        static char g_saveDirBuf[MAX_PATH] = {};
        if (g_saveDirBuf[0] == '\0')
            strncpy_s(g_saveDirBuf, g_saveDirectory.c_str(), sizeof(g_saveDirBuf) - 1);
        ImGui::SetNextItemWidth(cw2);
        ImGui::InputText(tr("Save to"), g_saveDirBuf, sizeof(g_saveDirBuf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("...")) {
            char dir[MAX_PATH] = {};
            BROWSEINFOA bi = {};
            bi.hwndOwner = g_hwnd;
            bi.pszDisplayName = dir;
            bi.lpszTitle = "Select clip save folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
            if (pidl) {
                if (SHGetPathFromIDListA(pidl, dir)) {
                    g_saveDirectory = dir;
                    strncpy_s(g_saveDirBuf, dir, sizeof(g_saveDirBuf) - 1);
                }
            }
        }

        ImGui::Checkbox(tr("Enable video"), &g_config.enableVideo);

        ImGui::SetNextItemWidth(cw2);
        int clipMinutes = g_defaultClipDuration / 60;
        if (ImGui::InputInt(tr("Clip (min)"), &clipMinutes))
            g_defaultClipDuration = std::max(1, clipMinutes) * 60;

        ImGui::SetNextItemWidth(cw2);
        int bufMB = (int)g_config.bufferSizeMB;
        if (ImGui::InputInt(tr("Buffer (MB)"), &bufMB))
            g_config.bufferSizeMB = std::max(64, bufMB);

        static char rtmpUrlBuf[512] = {};
        if (rtmpUrlBuf[0] == '\0')
            strncpy_s(rtmpUrlBuf, g_rtmpUrl.c_str(), sizeof(rtmpUrlBuf) - 1);
        ImGui::SetNextItemWidth(cw2 + 100);
        if (ImGui::InputText(tr("RTMP URL"), rtmpUrlBuf, sizeof(rtmpUrlBuf)))
            g_rtmpUrl = rtmpUrlBuf;

        ImGui::Separator(); ImGui::Text("%s", tr("MONITOR"));
        ImGui::SetNextItemWidth(cw2);
        int monCount = OpenReplay::ScreenCapture::enumerateMonitors();
        if (g_config.captureMonitor >= monCount) g_config.captureMonitor = 0;
        {
            std::vector<std::string> monNames;
            for (int i = 0; i < monCount; i++) {
                const char* mn = OpenReplay::ScreenCapture::getMonitorName(i);
                monNames.push_back(mn ? mn : ("Monitor " + std::to_string(i)));
            }
            if (monNames.empty()) monNames.push_back("Default");
            std::vector<const char*> monPtrs;
            for (auto& n : monNames) monPtrs.push_back(n.c_str());
            ImGui::Combo(tr("Capture"), &g_config.captureMonitor, monPtrs.data(), (int)monPtrs.size());
        }

        ImGui::Checkbox(tr("Capture cursor"), &g_config.captureCursor);

        ImGui::Separator(); ImGui::Text("%s", tr("AUDIO DEVICES"));
        {
            std::vector<const char*> audioPtrs = {"Default"};
            for (auto& d : g_audioDevices) audioPtrs.push_back(d.name.c_str());
            int audioSel = 0;
            for (size_t i = 0; i < g_audioDevices.size(); i++)
                if (g_audioDevices[i].id == g_config.audioDeviceId) { audioSel = (int)i + 1; break; }
            ImGui::SetNextItemWidth(cw2);
            if (ImGui::Combo(tr("Loopback"), &audioSel, audioPtrs.data(), (int)audioPtrs.size())) {
                g_config.audioDeviceId = audioSel > 0 ? g_audioDevices[audioSel - 1].id : "";
            }
        }
        {
            std::vector<const char*> micPtrs = {"Default"};
            for (auto& d : g_micDevices) micPtrs.push_back(d.name.c_str());
            int micSel = 0;
            for (size_t i = 0; i < g_micDevices.size(); i++)
                if (g_micDevices[i].id == g_config.micDeviceId) { micSel = (int)i + 1; break; }
            ImGui::SetNextItemWidth(cw2);
            if (ImGui::Combo(tr("Microphone"), &micSel, micPtrs.data(), (int)micPtrs.size())) {
                g_config.micDeviceId = micSel > 0 ? g_micDevices[micSel - 1].id : "";
            }
        }

        ImGui::Separator(); ImGui::Text("%s", tr("PROFILES"));
        {
            std::vector<const char*> profPtrs;
            for (auto& n : g_profileNames) profPtrs.push_back(n.c_str());
            ImGui::SetNextItemWidth(cw2);
            ImGui::Combo("##profile", &g_selectedProfile, profPtrs.data(), (int)profPtrs.size());
            ImGui::SameLine();
            if (ImGui::Button(tr("Save"))) {
                g_showProfileDialog = true;
                g_profileNameBuf[0] = '\0';
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Load")) && g_selectedProfile >= 0 && g_selectedProfile < (int)g_profileNames.size()) {
                applyProfile(g_profileNames[g_selectedProfile]);
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Export")) && g_selectedProfile >= 0 && g_selectedProfile < (int)g_profileNames.size()) {
                char path[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = g_hwnd;
                ofn.lpstrFile = path;
                ofn.nMaxFile = sizeof(path);
                ofn.lpstrDefExt = "json";
                ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                if (GetSaveFileNameA(&ofn)) {
                    if (g_profiles.exportProfile(g_profileNames[g_selectedProfile], path))
                        showToast(L"Profile Exported", L"Profile exported successfully.");
                    else
                        MessageBoxA(g_hwnd, "Failed to export profile.", "Export Error", MB_ICONERROR);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Import"))) {
                char path[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = g_hwnd;
                ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = sizeof(path);
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                if (GetOpenFileNameA(&ofn)) {
                    if (g_profiles.importProfile(path)) {
                        refreshProfileCombo();
                        showToast(L"Profile Imported", L"Profile imported successfully.");
                    } else {
                        MessageBoxA(g_hwnd, "Failed to import profile.", "Import Error", MB_ICONERROR);
                    }
                }
            }
        }

        if (g_showProfileDialog) {
            ImGui::OpenPopup(tr("Save Profile As"));
            g_showProfileDialog = false;
        }
        if (ImGui::BeginPopupModal(tr("Save Profile As"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText(tr("Name"), g_profileNameBuf, sizeof(g_profileNameBuf));
            if (ImGui::Button("OK")) {
                if (g_profileNameBuf[0]) {
                    saveCurrentProfile(g_profileNameBuf);
                    refreshProfileCombo();
                    for (size_t i = 0; i < g_profileNames.size(); i++)
                        if (g_profileNames[i] == g_profileNameBuf) { g_selectedProfile = (int)i; break; }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Separator(); ImGui::Text("%s", tr("OVERLAY"));
        const char* corners[] = {"Top-Right", "Top-Left", "Bottom-Right", "Bottom-Left"};
        int corner = overlayCorner();
        ImGui::SetNextItemWidth(cw2);
        if (ImGui::Combo(tr("Corner"), &corner, corners, 4))
            overlaySetCorner(corner);
        int ovSize = overlaySize();
        ImGui::SetNextItemWidth(cw2);
        if (ImGui::InputInt(tr("Size"), &ovSize)) {
            if (ovSize < 4) ovSize = 4;
            overlaySetSize(ovSize);
        }
        int ovAlpha = overlayAlpha();
        ImGui::SetNextItemWidth(cw2);
        if (ImGui::InputInt(tr("Opacity"), &ovAlpha)) {
            if (ovAlpha < 0) ovAlpha = 0;
            if (ovAlpha > 255) ovAlpha = 255;
            overlaySetAlpha(ovAlpha);
        }

        ImGui::Separator(); ImGui::Text("%s", tr("PANEL"));
        ImGui::Checkbox(tr("Auto-scale Y"), &g_panelAutoScaleY);
        ImGui::SetNextItemWidth(cw2);
        ImGui::InputInt(tr("Width"), &g_panelWidth);
        ImGui::SameLine(); ImGui::TextDisabled("px");
        if (!g_panelAutoScaleY) {
            ImGui::SetNextItemWidth(cw2);
            ImGui::InputInt(tr("Height %"), &g_panelHeightPct);
        }
        if (g_panelWidth < 60) g_panelWidth = 60;

        ImGui::Separator(); ImGui::Text("%s", tr("USER EXPERIENCE"));
        if (ImGui::Checkbox(tr("Show recording indicator dot"), &g_dotEnabled)) {
            g_config.showRecordingDot = g_dotEnabled;
            if (g_recording.load() && !g_dotEnabled)
                overlayShow(false);
            else if (g_recording.load() && g_dotEnabled)
                overlayShow(true);
        }
        ImGui::Checkbox(tr("Auto-save clip on stop"), &g_config.autoSaveOnStop);
        ImGui::Checkbox(tr("Minimize to system tray"), &g_config.minimizeToTray);

        ImGui::Separator(); ImGui::Text("%s", tr("HOTKEYS"));
        float hkCw = ImGui::GetColumnWidth() - 100;
        auto hotkeyRow = [&](const char* label, int id, UINT& mod, UINT& key) {
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(hkCw);
            std::string cur = modStr(mod) + (char)key;
            if (g_capturingHotkeyFor == id) {
                ImGui::TextDisabled("> %s ...", label);
            } else {
                ImGui::Text("%s: %s", label, cur.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button(tr(g_capturingHotkeyFor == id ? "Cancel" : "Rebind"), ImVec2(60, 0))) {
                if (g_capturingHotkeyFor == id)
                    g_capturingHotkeyFor = 0;
                else
                    g_capturingHotkeyFor = id;
            }
            ImGui::PopID();
        };
        hotkeyRow(tr("Save"), 1, g_hotkeySaveMod, g_hotkeySaveKey);
        hotkeyRow(tr("Rec"), 2, g_hotkeyToggleMod, g_hotkeyToggleKey);
        hotkeyRow(tr("Panel"), 3, g_hotkeyPanelMod, g_hotkeyPanelKey);

        ImGui::Columns(1);
    }
    ImGui::EndChild();
}

static void drawAboutTab() {
    if (ImGui::BeginChild("aboutScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
    std::string text = "OpenReplay v2.1\n\n"
        "Optimized screen recorder for Windows.\n"
        "Low-latency AMF capture, memory-mapped buffer,\n"
        "streaming mux with progress reporting.\n\n"
        "Pipeline:\n"
        "  DXGI Desktop Duplication (3-buffer)\n"
        "  WASAPI loopback + silence detection\n"
        "  AMF CBR ultralowlatency (hevc_amf / h264_amf)\n"
        "  Memory-mapped circular buffer\n"
        "  MP4 / MKV / WEBM / MOV / AVI / WAV / FLAC / MP3 / OGG\n\n";
    addGpuInfo(text);
    text += "\ngithub.com/Shikaru0/OpenReplay";
    ImGui::TextWrapped("%s", text.c_str());
    }
    ImGui::EndChild();
}

static void renderUI() {
    ImGuiLayer::beginFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

    if (g_panelActive)
        mainFlags |= ImGuiWindowFlags_NoScrollbar;

    ImGui::Begin("Main", nullptr, mainFlags);

    if (g_panelActive) {
        // ── NVIDIA-style left-side overlay panel ──
        float pw = ImGui::GetContentRegionAvail().x;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.80f, 0.85f, 1));
        ImGui::Text("OpenReplay");
        ImGui::PopStyleColor();
        ImGui::SameLine(pw - 30);
        if (ImGui::SmallButton("X"))
            { closePanel(g_hwnd); ShowWindow(g_hwnd, SW_HIDE); }

        ImGui::Separator();
        ImGui::Spacing();

        if (g_recording.load()) {
            ImGui::TextColored(ImVec4(1,0.15f,0.15f,1), "%s", "\xe2\x97\x8f Recording");
        } else {
            ImGui::TextDisabled("%s", "\xe2\x97\x8b Idle");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float bh = 42.0f;
        bool saving = g_saveThread.joinable();

        if (g_recording.load()) {
            if (ImGui::Button(tr("Stop Recording"), ImVec2(-1, bh))) {
                if (g_engineOk) { g_engine->stopCapture(); g_recording.store(false); overlayShow(false); }
            }
        } else {
            if (ImGui::Button(tr("Start Recording"), ImVec2(-1, bh))) {
                if (g_engineOk && !g_recording.load()) {
                    g_engine->setConfig(g_config); g_engine->startCapture(); g_recording.store(true); overlayShow(true);
                }
            }
        }

        if (saving) ImGui::BeginDisabled();
        if (ImGui::Button(tr("Save Last Clip"), ImVec2(-1, bh))) { saveClip(g_hwnd); }
        if (saving) ImGui::EndDisabled();

        if (ImGui::Button(tr("Open Clips"), ImVec2(-1, bh))) {
            std::string dir = g_saveDirectory.empty() ? "." : g_saveDirectory;
            ShellExecuteA(g_hwnd, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
        }

        ImGui::Spacing();
        float avail = ImGui::GetContentRegionAvail().y;
        if (avail > 100) ImGui::Dummy(ImVec2(0, avail - 90));

        char hkBuf[128];
        snprintf(hkBuf, sizeof(hkBuf), "%s%c  %s%c",
                 modStr(g_hotkeySaveMod).c_str(), (char)g_hotkeySaveKey,
                 modStr(g_hotkeyToggleMod).c_str(), (char)g_hotkeyToggleKey);
        ImGui::TextDisabled("%s", hkBuf);
        ImGui::Separator();

        if (ImGui::Button(tr("Full Settings"), ImVec2(-1, bh))) {
            closePanel(g_hwnd);
            ShowWindow(g_hwnd, SW_RESTORE);
            SetForegroundWindow(g_hwnd);
        }

        ImGui::PopStyleVar();
    } else {
        if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_NoTooltip | ImGuiTabBarFlags_FittingPolicyScroll)) {
            if (ImGui::BeginTabItem(tr("Recording"))) {
                drawRecordingTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("Customize"))) {
                drawCustomizeTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("About"))) {
                drawAboutTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::Text("%s", statusText().c_str());
    }

    ImGui::End();

    if (g_panelActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        closePanel(g_hwnd);
        ShowWindow(g_hwnd, SW_HIDE);
    } else if (!g_panelActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && g_config.minimizeToTray) {
        ShowWindow(g_hwnd, SW_HIDE);
    }

    ImGuiLayer::render();
}

#endif

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
                if (g_engineOk && g_recording.load()) {
                    g_engine->stopCapture(); g_recording.store(false);
                    overlayShow(false);
                }
            } else {
                if (!g_engineOk || g_recording.load()) break;
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
            showToast(L"Save Failed", L"Could not save the clip. Check disk space.");
            MessageBoxA(hwnd, "Failed to save clip. Check disk space and output directory.", "Save Error", MB_ICONERROR);
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
                if (!ImGuiLayer::handleEvent(msg.hwnd, msg.message, msg.wParam, msg.lParam)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
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
    }

    return 0;
}
