#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include "Config.h"
#include "ClipManager.h"
#include "AudioCapture.h"
#include "OpenReplay.h"

namespace fs = std::filesystem;

extern OpenReplay::OpenReplayEngine* g_engine;
extern OpenReplay::RecorderConfig g_config;
extern std::mutex g_configMutex;
extern std::atomic<bool> g_recording;
extern std::atomic<bool> g_saving;
extern std::atomic<float> g_saveProgress;
extern std::atomic<bool> g_saveSuccess;
extern std::atomic<bool> g_engineOk;
extern bool g_isHdrActive;
extern bool g_dotEnabled;
extern std::vector<OpenReplay::AudioCapture::DeviceInfo> g_audioDevices;
extern std::vector<OpenReplay::AudioCapture::DeviceInfo> g_micDevices;
extern UINT g_hotkeySaveMod, g_hotkeySaveKey;
extern UINT g_hotkeyToggleMod, g_hotkeyToggleKey;
extern UINT g_hotkeyPanelMod, g_hotkeyPanelKey;
extern UINT g_hotkeyScreenshotMod, g_hotkeyScreenshotKey;
extern int g_screenshotFormat;
extern int g_screenshotQuality;
extern HWND g_hwnd;
extern std::string g_saveDirectory;
extern int g_defaultClipDuration;
extern int g_audioOutputFormatIdx;
extern std::string g_rtmpUrl;
extern ClipManager g_clipMgr;
extern std::vector<fs::path> g_clipList;
extern std::vector<std::string> g_clipDisplayTexts;
extern int g_selectedClip;
extern OpenReplay::ProfileManager g_profiles;
extern std::vector<std::string> g_profileNames;
extern int g_selectedProfile;
extern int g_qualityPreset;
extern int g_prevQualityPreset;
extern bool g_showProfileDialog;
extern char g_profileNameBuf[256];
extern int g_capturingHotkeyFor;
extern bool g_panelActive;
extern bool g_panelAutoScaleY;
extern int g_panelWidth;
extern int g_panelHeightPct;
extern std::string g_lastSaveResult;
extern std::thread g_saveThread;
extern std::string g_threadResultMsg;

extern const UINT WM_APP_SAVE_DONE;

void saveClip(HWND hwnd);
void closePanel(HWND hwnd);
void showToast(const wchar_t* title, const wchar_t* message);
bool streamToUrl();
void applyProfile(const std::string& name);
void saveCurrentProfile(const std::string& name);

#ifdef HAS_IMGUI
void renderUI();
#endif
void refreshClipList();
void refreshProfileCombo();
