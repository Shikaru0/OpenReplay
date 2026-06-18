#include "UI.h"
#include "Config.h"
#include "Locale.h"
#include "Overlay.h"
#include "Muxer.h"
#include "ScreenCapture.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>

#ifdef HAS_IMGUI
#include "ImGuiLayer.h"
#include <imgui.h>
#endif

namespace fs = std::filesystem;

static std::string modStr(UINT m) {
    std::string r;
    if (m & MOD_CONTROL) r += "Ctrl+";
    if (m & MOD_SHIFT) r += "Shift+";
    if (m & MOD_ALT) r += "Alt+";
    return r;
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

void refreshClipList() {
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

void refreshProfileCombo() {
    g_profileNames = g_profiles.list();
    if (g_selectedProfile >= (int)g_profileNames.size())
        g_selectedProfile = -1;
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
            bool ok = false;
            __try {
                ok = streamToUrl();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                g_threadResultMsg = "Stream failed: unexpected error";
            }
            g_saving.store(false);
            g_lastSaveResult = g_threadResultMsg.c_str();
            PostMessageA(g_hwnd, WM_APP_SAVE_DONE, (WPARAM)ok, 0);
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
        int fps = (int)s.currentFps.load();
        if (g_dotEnabled) overlaySetFpsValue(fps);
        std::ostringstream ss;
        ss << tr("Frames") << ": " << s.framesCaptured.load()
           << "  |  FPS: " << fps
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
    if (ImGui::BeginChild("customizeScroll", ImVec2(0, 0), false, 0)) {
        float cw = ImGui::GetContentRegionAvail().x - 160;

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

        ImGui::Separator(); ImGui::Text("%s", tr("OUTPUT"));
        const char* videoFormats[] = {"MP4 (H.264/HEVC)", "MKV (H.264/HEVC)", "WEBM (VP9/AV1)",
                                       "MOV (H.264/HEVC)", "AVI (Xvid/H.264)", "WAV (PCM)",
                                       "FLAC", "MP3", "OGG (Vorbis)"};
        ImGui::SetNextItemWidth(cw);
        int fmtIdx = OpenReplay::formatToIndex(g_config.outputFormat);
        if (ImGui::Combo(tr("Format"), &fmtIdx, videoFormats, 9))
            g_config.outputFormat = OpenReplay::formatFromIndex(fmtIdx);

        const char* audioFormats[] = {"WAV (PCM)", "FLAC", "MP3", "OGG (Vorbis)"};
        ImGui::SetNextItemWidth(cw);
        ImGui::Combo(tr("Audio-only fmt"), &g_audioOutputFormatIdx, audioFormats, 4);

        static char g_saveDirBuf[MAX_PATH] = {};
        if (g_saveDirBuf[0] == '\0')
            strncpy_s(g_saveDirBuf, g_saveDirectory.c_str(), sizeof(g_saveDirBuf) - 1);
        ImGui::SetNextItemWidth(cw);
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

        ImGui::SetNextItemWidth(cw);
        int clipMinutes = g_defaultClipDuration / 60;
        if (ImGui::InputInt(tr("Clip (min)"), &clipMinutes))
            g_defaultClipDuration = std::max(1, clipMinutes) * 60;

        ImGui::SetNextItemWidth(cw);
        int bufMB = (int)g_config.bufferSizeMB;
        if (ImGui::InputInt(tr("Buffer (MB)"), &bufMB))
            g_config.bufferSizeMB = std::max(64, bufMB);

        static char rtmpUrlBuf[512] = {};
        if (rtmpUrlBuf[0] == '\0')
            strncpy_s(rtmpUrlBuf, g_rtmpUrl.c_str(), sizeof(rtmpUrlBuf) - 1);
        ImGui::SetNextItemWidth(cw);
        if (ImGui::InputText(tr("RTMP URL"), rtmpUrlBuf, sizeof(rtmpUrlBuf)))
            g_rtmpUrl = rtmpUrlBuf;

        ImGui::Separator(); ImGui::Text("%s", tr("MONITOR"));
        ImGui::SetNextItemWidth(cw);
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
            ImGui::SetNextItemWidth(cw);
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
            ImGui::SetNextItemWidth(cw);
            if (ImGui::Combo(tr("Microphone"), &micSel, micPtrs.data(), (int)micPtrs.size())) {
                g_config.micDeviceId = micSel > 0 ? g_micDevices[micSel - 1].id : "";
            }
        }

        ImGui::Separator(); ImGui::Text("%s", tr("PROFILES"));
        {
            std::vector<const char*> profPtrs;
            for (auto& n : g_profileNames) profPtrs.push_back(n.c_str());
            ImGui::SetNextItemWidth(cw);
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
        ImGui::SetNextItemWidth(cw);
        if (ImGui::Combo(tr("Corner"), &corner, corners, 4))
            overlaySetCorner(corner);
        int ovSize = overlaySize();
        ImGui::SetNextItemWidth(cw);
        if (ImGui::InputInt(tr("Size"), &ovSize)) {
            if (ovSize < 4) ovSize = 4;
            overlaySetSize(ovSize);
        }
        int ovAlpha = overlayAlpha();
        ImGui::SetNextItemWidth(cw);
        if (ImGui::InputInt(tr("Opacity"), &ovAlpha)) {
            if (ovAlpha < 0) ovAlpha = 0;
            if (ovAlpha > 255) ovAlpha = 255;
            overlaySetAlpha(ovAlpha);
        }

        ImGui::Separator(); ImGui::Text("%s", tr("PANEL"));
        ImGui::Checkbox(tr("Auto-scale Y"), &g_panelAutoScaleY);
        ImGui::SetNextItemWidth(cw);
        ImGui::InputInt(tr("Width"), &g_panelWidth);
        ImGui::SameLine(); ImGui::TextDisabled("px");
        if (!g_panelAutoScaleY) {
            ImGui::SetNextItemWidth(cw);
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
        auto hotkeyRow = [&](const char* label, int id, UINT& mod, UINT& key) {
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(cw);
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
        hotkeyRow(tr("Screenshot"), 4, g_hotkeyScreenshotMod, g_hotkeyScreenshotKey);
        ImGui::Spacing();
        ImGui::Text("%s", tr("Screenshot"));
        ImGui::SetNextItemWidth(cw);
        static const char* fmtNames[] = {"PNG", "JPEG"};
        ImGui::Combo(tr("Format"), &g_screenshotFormat, fmtNames, 2);
        if (g_screenshotFormat == 1) {
            ImGui::SetNextItemWidth(cw);
            ImGui::SliderInt(tr("Quality"), &g_screenshotQuality, 1, 100);
        }
    }
    ImGui::EndChild();
}


void renderUI() {
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
        bool saving = g_saving.load();

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
