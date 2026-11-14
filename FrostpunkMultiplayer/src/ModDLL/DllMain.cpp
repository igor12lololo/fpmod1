#include "ModMain.h"
#include "../Common/Common.h"
#include <windows.h>
#include <cstdio>

using FrostMP::u64;
using FrostMP::DebugLog;
using FrostMP::TraceToFile;

namespace FrostMP::Mod {

static std::unique_ptr<FrostpunkMultiplayerMod> g_Mod;
static HANDLE g_UpdateThread = nullptr;
static std::atomic<bool> g_Running{false};
static std::atomic<bool> g_DeferredInitRequested{false};
static std::atomic<bool> g_DeferredInitComplete{false};

// Forward declarations
void InitializeMod(u64 localSteamID, Network::NetworkManager::Role role, u64 hostSteamID);
void ShutdownMod();

// SEH-safe wrapper around g_Mod->Update().
// Must be a separate function: MSVC does not allow __try/__except in
// functions that require C++ stack unwinding (error C2712/C2713).
static void UpdateWithSEH() {
    __try {
        g_Mod->Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("UPDATE THREAD: SEH exception (0xC0000005 or similar)");
    }
}

DWORD WINAPI UpdateThread(LPVOID) {
    TraceToFile("UPDATE THREAD: started");
    DebugLog("Update thread started");
    int updateCounter = 0;
    while (g_Running.load()) {
        if (g_Mod) {
            try {
                UpdateWithSEH();
                if ((++updateCounter % 600) == 0) { // every ~10 seconds at 60fps
                    char msg[128];
                    snprintf(msg, sizeof(msg), "UPDATE THREAD: alive, counter=%d", updateCounter);
                    TraceToFile(msg);
                }
            } catch (const std::exception& e) {
                TraceToFile("UPDATE THREAD: C++ exception");
                DebugLog("Update exception: %s", e.what());
            } catch (...) {
                TraceToFile("UPDATE THREAD: unknown C++ exception");
                DebugLog("Update unknown exception");
            }
        }
        Sleep(16); // ~60 FPS update rate
    }
    TraceToFile("UPDATE THREAD: ended");
    DebugLog("Update thread ended");
    return 0;
}

// Deferred initialization - runs on background thread after DLL load
DWORD WINAPI DeferredInitThread(LPVOID param) {
    // Small delay to let loader lock fully release
    Sleep(100);

    TraceToFile("DEFERRED INIT: started");
    DebugLog("DeferredInitThread: Starting deferred initialization");

    // Read config and initialize
    try {
        HMODULE hModule = static_cast<HMODULE>(param);
        auto modDir = GetModuleDirectory(hModule);
        auto configPath = modDir / L"multiplayer.ini";

        DebugLog("DeferredInitThread: modDir=%s, configPath=%s",
                 modDir.string().c_str(), configPath.string().c_str());

        if (!std::filesystem::exists(configPath)) {
            DebugLog("DeferredInitThread: config file NOT found at %s, waiting for launcher init",
                     configPath.string().c_str());
            g_DeferredInitComplete.store(true);
            return 0;
        }

        DebugLog("DeferredInitThread: config file found");

        wchar_t roleStr[32] = {};
        GetPrivateProfileStringW(L"Multiplayer", L"Role", L"", roleStr, 32, configPath.c_str());
        DebugLog("DeferredInitThread: Role=%ls", roleStr);

        if (wcscmp(roleStr, L"Host") == 0) {
            wchar_t steamIdStr[64] = {};
            GetPrivateProfileStringW(L"Multiplayer", L"LocalSteamID", L"", steamIdStr, 64, configPath.c_str());
            u64 localSteamID = 0;
            if (!steamIdStr[0] || !ParseSteamID64(steamIdStr, localSteamID)) {
                localSteamID = 76561198000000000ULL; // Placeholder
            }
            DebugLog("DeferredInitThread: Initializing as Host, localSteamID=%llu", localSteamID);
            InitializeMod(localSteamID, Network::NetworkManager::Role::Host, 0);
        } else if (wcscmp(roleStr, L"Client") == 0) {
            wchar_t steamIdStr[64] = {};
            wchar_t hostSteamIdStr[64] = {};
            GetPrivateProfileStringW(L"Multiplayer", L"LocalSteamID", L"", steamIdStr, 64, configPath.c_str());
            GetPrivateProfileStringW(L"Multiplayer", L"HostSteamID", L"", hostSteamIdStr, 64, configPath.c_str());

            u64 localSteamID = 0, hostSteamID = 0;
            if (!steamIdStr[0] || !ParseSteamID64(steamIdStr, localSteamID)) {
                localSteamID = 76561198000000001ULL;
            }
            if (!hostSteamIdStr[0] || !ParseSteamID64(hostSteamIdStr, hostSteamID)) {
                DebugLog("DeferredInitThread: Invalid host SteamID");
                g_DeferredInitComplete.store(true);
                return 0;
            }
            DebugLog("DeferredInitThread: Initializing as Client, localSteamID=%llu, hostSteamID=%llu", localSteamID, hostSteamID);
            InitializeMod(localSteamID, Network::NetworkManager::Role::Client, hostSteamID);
        } else {
            DebugLog("DeferredInitThread: Unknown role: %ls", roleStr);
        }
    } catch (const std::exception& e) {
        TraceToFile("DEFERRED INIT: exception");
        DebugLog("DeferredInitThread: Exception: %s", e.what());
    } catch (...) {
        TraceToFile("DEFERRED INIT: unknown exception");
        DebugLog("DeferredInitThread: Unknown exception");
    }

    TraceToFile("DEFERRED INIT: completed");
    g_DeferredInitComplete.store(true);
    return 0;
}

void InitializeMod(u64 localSteamID, Network::NetworkManager::Role role, u64 hostSteamID) {
    if (g_Mod) {
        DebugLog("InitializeMod: already initialized");
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "INITIALIZE MOD: localSteamID=%llu, role=%d, hostSteamID=%llu",
             localSteamID, (int)role, hostSteamID);
    TraceToFile(msg);
    DebugLog("InitializeMod: localSteamID=%llu, role=%d, hostSteamID=%llu",
             localSteamID, (int)role, hostSteamID);

    g_Mod = std::make_unique<FrostpunkMultiplayerMod>();
    if (g_Mod->Initialize(localSteamID, role, hostSteamID)) {
        TraceToFile("INITIALIZE MOD: mod initialized successfully, starting update thread");
        DebugLog("InitializeMod: mod initialized successfully, starting update thread");
        g_Running.store(true);
        g_UpdateThread = CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);
        if (!g_UpdateThread) {
            TraceToFile("INITIALIZE MOD: FAILED to create update thread");
            DebugLog("InitializeMod: FAILED to create update thread! Error: %lu", GetLastError());
        }
    } else {
        TraceToFile("INITIALIZE MOD: mod initialization FAILED");
        DebugLog("InitializeMod: mod initialization FAILED");
        g_Mod.reset();
    }
}

void ShutdownMod() {
    TraceToFile("SHUTDOWN MOD: started");
    g_Running.store(false);
    if (g_UpdateThread) {
        WaitForSingleObject(g_UpdateThread, 5000);
        CloseHandle(g_UpdateThread);
        g_UpdateThread = nullptr;
    }
    if (g_Mod) {
        g_Mod->Shutdown();
        g_Mod.reset();
    }
    TraceToFile("SHUTDOWN MOD: completed");
}

// Auto-initialization from config file - now DEFERRED
void TryAutoInitialize(HMODULE hModule) {
    TraceToFile("TRY AUTO INIT: scheduling deferred init");
    DebugLog("TryAutoInitialize: Scheduling deferred initialization");
    g_DeferredInitRequested.store(true);

    // Create background thread for deferred init (minimal work in DllMain)
    HANDLE thread = CreateThread(nullptr, 0, DeferredInitThread, hModule, 0, nullptr);
    if (thread) {
        CloseHandle(thread); // Fire and forget - thread manages itself
    } else {
        TraceToFile("TRY AUTO INIT: FAILED to create deferred init thread");
        DebugLog("TryAutoInitialize: FAILED to create deferred init thread! Error: %lu", GetLastError());
        g_DeferredInitComplete.store(true); // Don't block forever
    }
}

// DLL Entry Point - MINIMAL work only
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            TraceToFile("DLLMAIN: PROCESS_ATTACH");
            DebugLog("DllMain: DLL_PROCESS_ATTACH");
            DisableThreadLibraryCalls(hModule);
            // ONLY schedule deferred init - NO heavy work here
            TryAutoInitialize(hModule);
            TraceToFile("DLLMAIN: Deferred init scheduled");
            DebugLog("DllMain: Deferred init scheduled");
            break;
        case DLL_PROCESS_DETACH:
            TraceToFile("DLLMAIN: PROCESS_DETACH");
            DebugLog("DllMain: DLL_PROCESS_DETACH");
            if (lpReserved == nullptr) { // Not process termination
                // Give deferred init time to complete or cancel
                if (g_DeferredInitRequested.load() && !g_DeferredInitComplete.load()) {
                    Sleep(200); // Brief wait for deferred init
                }
                FrostMP::Mod::ShutdownMod();
            }
            break;
    }
    return TRUE;
}

} // namespace FrostMP::Mod