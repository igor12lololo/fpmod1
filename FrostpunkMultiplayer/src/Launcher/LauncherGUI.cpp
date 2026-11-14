#include "Common.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <vector>
#include "resource.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

// Forward declaration from Launcher.cpp
namespace FrostMP::Launcher {
    int Run(int argc, wchar_t* argv[]);
}

namespace FrostMP::LauncherGUI {

// Forward declaration
bool ForceInjectIntoRunningGame(HWND hDlg, bool isHost, u64 localSteamID, u64 hostSteamID);

struct Config {
    bool isHost = true;
    u64 localSteamID = 0;
    u64 hostSteamID = 0;
    std::wstring gamePath;
    bool saveConfig = true;
};

static Config g_Config;
static HWND g_hDialog = nullptr;
static HINSTANCE g_hInstance = nullptr;

std::wstring GetConfigPath() {
    auto dir = GetModuleDirectory();
    return (dir / L"multiplayer.ini").wstring();
}

bool LoadConfig(Config& cfg) {
    auto path = GetConfigPath();
    wchar_t buf[256];

    GetPrivateProfileStringW(L"Multiplayer", L"Role", L"Host", buf, 256, path.c_str());
    cfg.isHost = (wcscmp(buf, L"Host") == 0);

    GetPrivateProfileStringW(L"Multiplayer", L"LocalSteamID", L"", buf, 256, path.c_str());
    ParseSteamID64(buf, cfg.localSteamID);

    GetPrivateProfileStringW(L"Multiplayer", L"HostSteamID", L"", buf, 256, path.c_str());
    ParseSteamID64(buf, cfg.hostSteamID);

    GetPrivateProfileStringW(L"Game", L"Path", L"", buf, 256, path.c_str());
    cfg.gamePath = buf;

    return true;
}

void SaveConfig(const Config& cfg) {
    auto path = GetConfigPath();
    WritePrivateProfileStringW(L"Multiplayer", L"Role", cfg.isHost ? L"Host" : L"Client", path.c_str());

    wchar_t buf[64];
    if (cfg.localSteamID) {
        swprintf_s(buf, L"%llu", cfg.localSteamID);
        WritePrivateProfileStringW(L"Multiplayer", L"LocalSteamID", buf, path.c_str());
    }
    if (cfg.hostSteamID) {
        swprintf_s(buf, L"%llu", cfg.hostSteamID);
        WritePrivateProfileStringW(L"Multiplayer", L"HostSteamID", buf, path.c_str());
    }
    if (!cfg.gamePath.empty()) {
        WritePrivateProfileStringW(L"Game", L"Path", cfg.gamePath.c_str(), path.c_str());
    }
}

void SetControlText(HWND hDlg, int id, const std::wstring& text) {
    SetDlgItemTextW(hDlg, id, text.c_str());
}

std::wstring GetControlText(HWND hDlg, int id) {
    wchar_t buf[256];
    GetDlgItemTextW(hDlg, id, buf, 256);
    return buf;
}

bool ValidateSteamID(const std::wstring& text, u64& out) {
    if (text.empty()) return false;
    return ParseSteamID64(text, out);
}

void UpdateUIState(HWND hDlg) {
    bool isHost = IsDlgButtonChecked(hDlg, IDC_RADIO_HOST) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_HOST_STEAMID), !isHost);
    SetDlgItemTextW(hDlg, IDC_STATIC_HOST_LABEL, isHost ? L"Host SteamID (not used):" : L"Host SteamID (required):");
}

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_hDialog = hDlg;
            LoadConfig(g_Config);

            CheckRadioButton(hDlg, IDC_RADIO_HOST, IDC_RADIO_CLIENT, g_Config.isHost ? IDC_RADIO_HOST : IDC_RADIO_CLIENT);

            if (g_Config.localSteamID) {
                wchar_t buf[64];
                swprintf_s(buf, L"%llu", g_Config.localSteamID);
                SetControlText(hDlg, IDC_EDIT_LOCAL_STEAMID, buf);
            }
            if (g_Config.hostSteamID) {
                wchar_t buf[64];
                swprintf_s(buf, L"%llu", g_Config.hostSteamID);
                SetControlText(hDlg, IDC_EDIT_HOST_STEAMID, buf);
            }
            if (!g_Config.gamePath.empty()) {
                SetControlText(hDlg, IDC_EDIT_GAME_PATH, g_Config.gamePath);
            }

            UpdateUIState(hDlg);
            return TRUE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDC_RADIO_HOST || id == IDC_RADIO_CLIENT) {
                UpdateUIState(hDlg);
                return TRUE;
            }

            if (id == IDC_BUTTON_BROWSE) {
                OPENFILENAMEW ofn{};
                wchar_t file[32768] = L"";
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hDlg;
                ofn.lpstrFilter = L"Frostpunk.exe\0Frostpunk.exe\0All Files\0*.*\0\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = 32768;
                ofn.lpstrTitle = L"Select Frostpunk.exe";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameW(&ofn)) {
                    SetControlText(hDlg, IDC_EDIT_GAME_PATH, file);
                }
                return TRUE;
            }

            if (id == IDC_BUTTON_FORCE_INVITE) {
                // Force inject into already running game
                std::wstring localStr = GetControlText(hDlg, IDC_EDIT_LOCAL_STEAMID);
                u64 localSteamID = 0;
                if (!ValidateSteamID(localStr, localSteamID)) {
                    MessageBoxW(hDlg, L"Enter your valid SteamID64 (17 digits, starts with 7656119...)", L"Error", MB_OK | MB_ICONERROR);
                    SetFocus(GetDlgItem(hDlg, IDC_EDIT_LOCAL_STEAMID));
                    return TRUE;
                }

                bool isHost = (IsDlgButtonChecked(hDlg, IDC_RADIO_HOST) == BST_CHECKED);
                u64 hostSteamID = 0;
                if (!isHost) {
                    std::wstring hostStr = GetControlText(hDlg, IDC_EDIT_HOST_STEAMID);
                    if (!ValidateSteamID(hostStr, hostSteamID)) {
                        MessageBoxW(hDlg, L"Enter host SteamID64 (your friend's)", L"Error", MB_OK | MB_ICONERROR);
                        SetFocus(GetDlgItem(hDlg, IDC_EDIT_HOST_STEAMID));
                        return TRUE;
                    }
                }

                g_Config.gamePath = GetControlText(hDlg, IDC_EDIT_GAME_PATH);
                if (g_Config.saveConfig) SaveConfig(g_Config);

                if (ForceInjectIntoRunningGame(hDlg, isHost, localSteamID, hostSteamID)) {
                    MessageBoxW(hDlg, L"Mod successfully injected into running game!\nF1 - Chat, F2 - Trade, F3 - Save, F10 - Toggle overlay", L"Success", MB_OK | MB_ICONINFORMATION);
                }
                return TRUE;
            }

            if (id == IDOK) {
                std::wstring localStr = GetControlText(hDlg, IDC_EDIT_LOCAL_STEAMID);
                if (!ValidateSteamID(localStr, g_Config.localSteamID)) {
                    MessageBoxW(hDlg, L"Enter your valid SteamID64 (17 digits, starts with 7656119...)", L"Error", MB_OK | MB_ICONERROR);
                    SetFocus(GetDlgItem(hDlg, IDC_EDIT_LOCAL_STEAMID));
                    return TRUE;
                }

                g_Config.isHost = (IsDlgButtonChecked(hDlg, IDC_RADIO_HOST) == BST_CHECKED);

                if (!g_Config.isHost) {
                    std::wstring hostStr = GetControlText(hDlg, IDC_EDIT_HOST_STEAMID);
                    if (!ValidateSteamID(hostStr, g_Config.hostSteamID)) {
                        MessageBoxW(hDlg, L"Enter host SteamID64 (your friend's)", L"Error", MB_OK | MB_ICONERROR);
                        SetFocus(GetDlgItem(hDlg, IDC_EDIT_HOST_STEAMID));
                        return TRUE;
                    }
                } else {
                    g_Config.hostSteamID = 0;
                }

                g_Config.gamePath = GetControlText(hDlg, IDC_EDIT_GAME_PATH);
                g_Config.saveConfig = (IsDlgButtonChecked(hDlg, IDC_CHECK_SAVE) == BST_CHECKED);

                if (g_Config.saveConfig) SaveConfig(g_Config);

                EndDialog(hDlg, IDOK);
                return TRUE;
            }

            if (id == IDCANCEL) {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

bool ShowConfigDialog(HINSTANCE hInstance) {
    g_hInstance = hInstance;
    INT_PTR result = DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DialogProc);
    return result == IDOK;
}

// Force inject into already running game
bool ForceInjectIntoRunningGame(HWND hDlg, bool isHost, u64 localSteamID, u64 hostSteamID) {
    try {
        // Find running Frostpunk process
        ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snap) {
            MessageBoxW(hDlg, L"Failed to get process list", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;

        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"Frostpunk.exe") == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }

        if (!pid) {
            MessageBoxW(hDlg, L"Frostpunk is not running!\nStart the game first, then click Force Invite.", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // Open process with required permissions
        ScopedHandle proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, pid));
        if (!proc) {
            DWORD err = GetLastError();
            std::string msg = "Failed to open game process (code: " + std::to_string(err) + ").\nRun launcher as Administrator.";
            MessageBoxW(hDlg, Widen(msg).c_str(), L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // Get DLL path
        auto dllPath = std::filesystem::weakly_canonical(GetModuleDirectory() / L"FrostMenuMod.dll");
        if (!std::filesystem::is_regular_file(dllPath)) {
            MessageBoxW(hDlg, L"FrostMenuMod.dll not found next to launcher", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // Inject DLL
        std::wstring wpath = dllPath.wstring();
        SIZE_T bytes = (wpath.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) {
            MessageBoxW(hDlg, L"VirtualAllocEx failed", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        SIZE_T written = 0;
        if (!WriteProcessMemory(proc, remote, wpath.c_str(), bytes, &written) || written != bytes) {
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"WriteProcessMemory failed", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
        std::uintptr_t localLoadLib = (std::uintptr_t)GetProcAddress(localK32, "LoadLibraryW");

        // Get kernel32 base in target process
        ScopedHandle snapMod(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        std::uintptr_t remoteK32 = 0;
        if (snapMod) {
            MODULEENTRY32W me{}; me.dwSize = sizeof(me);
            if (Module32FirstW(snapMod, &me)) {
                do {
                    if (_wcsicmp(me.szModule, L"kernel32.dll") == 0) {
                        remoteK32 = (std::uintptr_t)me.modBaseAddr;
                        break;
                    }
                } while (Module32NextW(snapMod, &me));
            }
        }

        if (!localLoadLib || !remoteK32) {
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"Could not resolve LoadLibraryW", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        std::uintptr_t rva = localLoadLib - (std::uintptr_t)localK32;
        auto remoteLoadLib = (LPTHREAD_START_ROUTINE)(remoteK32 + rva);

        ScopedHandle thread(CreateRemoteThread(proc, nullptr, 0, remoteLoadLib, remote, 0, nullptr));
        if (!thread) {
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"CreateRemoteThread failed", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        DWORD wait = WaitForSingleObject(thread, 30000);
        if (wait != WAIT_OBJECT_0) {
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"DLL load timed out (30s)", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        Sleep(500); // Give DLL time to initialize

        // Get the base address of the injected DLL in the target process
        std::uintptr_t remoteDllBase = 0;
        {
            ScopedHandle snapMod(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
            if (snapMod) {
                MODULEENTRY32W me{}; me.dwSize = sizeof(me);
                if (Module32FirstW(snapMod, &me)) {
                    do {
                        if (_wcsicmp(me.szModule, L"FrostMenuMod.dll") == 0) {
                            remoteDllBase = (std::uintptr_t)me.modBaseAddr;
                            break;
                        }
                    } while (Module32NextW(snapMod, &me));
                }
            }
        }

        if (!remoteDllBase) {
            MessageBoxW(hDlg, L"Could not find injected DLL in target process", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // Get the exported function address in the target process
        HMODULE localMod = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!localMod) {
            MessageBoxW(hDlg, L"Could not load DLL locally to get exports", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        using InitHostFn = void(*)(u64);
        using InitClientFn = void(*)(u64, u64);

        std::uintptr_t remoteFuncAddr = 0;
        if (isHost) {
            auto fn = (InitHostFn)GetProcAddress(localMod, "FrostMP_InitializeHost");
            if (!fn) {
                FreeLibrary(localMod);
                MessageBoxW(hDlg, L"FrostMP_InitializeHost not found in DLL", L"Error", MB_OK | MB_ICONERROR);
                return false;
            }
            remoteFuncAddr = remoteDllBase + ((std::uintptr_t)fn - (std::uintptr_t)localMod);
        } else {
            auto fn = (InitClientFn)GetProcAddress(localMod, "FrostMP_InitializeClient");
            if (!fn) {
                FreeLibrary(localMod);
                MessageBoxW(hDlg, L"FrostMP_InitializeClient not found in DLL", L"Error", MB_OK | MB_ICONERROR);
                return false;
            }
            remoteFuncAddr = remoteDllBase + ((std::uintptr_t)fn - (std::uintptr_t)localMod);
        }
        FreeLibrary(localMod);

        // Create shellcode to call the function with correct number of arguments
        // x64 calling convention: RCX = 1st arg, RDX = 2nd arg, R8 = 3rd arg, R9 = 4th arg
        std::vector<u8> shellcode;
        shellcode.reserve(64);

        // Shellcode for x64:
        // mov rcx, imm64 (localSteamID = 1st arg)
        // [if client] mov rdx, imm64 (hostSteamID = 2nd arg)
        // mov rax, imm64 (remoteFuncAddr)
        // call rax
        // xor rcx, rcx            ; ExitCode = 0
        // mov rax, imm64 (remoteExitThread)
        // call rax                ; ExitThread(0) — no 'ret' on a bare remote thread stack
        shellcode.reserve(64);

        // mov rcx, imm64 (1st arg = localSteamID)
        shellcode.insert(shellcode.end(), {0x48, 0xB9}); // mov rcx, imm64
        {
            size_t argOffset = shellcode.size();
            shellcode.resize(shellcode.size() + 8);
            *reinterpret_cast<std::uintptr_t*>(&shellcode[argOffset]) = localSteamID;
        }

        if (!isHost) {
            // mov rdx, imm64 (2nd arg = hostSteamID) — only for InitializeClient
            shellcode.insert(shellcode.end(), {0x48, 0xBA}); // mov rdx, imm64
            {
                size_t argOffset = shellcode.size();
                shellcode.resize(shellcode.size() + 8);
                *reinterpret_cast<std::uintptr_t*>(&shellcode[argOffset]) = hostSteamID;
            }
        }

        // mov rax, imm64 (remoteFuncAddr)
        shellcode.insert(shellcode.end(), {0x48, 0xB8}); // mov rax, imm64
        {
            size_t argOffset = shellcode.size();
            shellcode.resize(shellcode.size() + 8);
            *reinterpret_cast<std::uintptr_t*>(&shellcode[argOffset]) = remoteFuncAddr;
        }

        // x64 ABI: align stack to 16 bytes before call. CreateRemoteThread does not
        // guarantee 16-byte RSP alignment, and misaligned stack causes 0xC0000005
        // when the target function uses aligned SSE (movaps).
        shellcode.insert(shellcode.end(), {0x48, 0x83, 0xEC, 0x28}); // sub rsp, 0x28
        shellcode.insert(shellcode.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16

        // call rax
        shellcode.insert(shellcode.end(), {0xFF, 0xD0});

        // xor rcx, rcx (ExitCode = 0)
        shellcode.insert(shellcode.end(), {0x48, 0x31, 0xC9});

        // call remoteExitThread (resolve ExitThread in target process)
        std::uintptr_t remoteExitThread = 0;
        {
            std::uintptr_t remoteK32 = GetRemoteModuleBase(pid, L"kernel32.dll");
            HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
            if (remoteK32 && localK32) {
                auto localExitThread = (std::uintptr_t)GetProcAddress(localK32, "ExitThread");
                if (localExitThread) {
                    std::uintptr_t rva = localExitThread - (std::uintptr_t)localK32;
                    remoteExitThread = remoteK32 + rva;
                }
            }
        }

        if (remoteExitThread) {
            shellcode.insert(shellcode.end(), {0x48, 0xB8}); // mov rax, imm64
            {
                size_t argOffset = shellcode.size();
                shellcode.resize(shellcode.size() + 8);
                *reinterpret_cast<std::uintptr_t*>(&shellcode[argOffset]) = remoteExitThread;
            }
            shellcode.insert(shellcode.end(), {0xFF, 0xD0}); // call rax
        } else {
            shellcode.push_back(0xCC); // int3 fallback
        }

        // Allocate and write shellcode
        void* remoteShellcode = VirtualAllocEx(proc, nullptr, shellcode.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!remoteShellcode) {
            MessageBoxW(hDlg, L"VirtualAllocEx for shellcode failed", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        SIZE_T writtenShellcode = 0;
        if (!WriteProcessMemory(proc, remoteShellcode, shellcode.data(), shellcode.size(), &writtenShellcode) || writtenShellcode != shellcode.size()) {
            VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"WriteProcessMemory for shellcode failed", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // Execute shellcode
        ScopedHandle initThread(CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)remoteShellcode, nullptr, 0, nullptr));
        if (!initThread) {
            VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"CreateRemoteThread for init failed", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        wait = WaitForSingleObject(initThread, 10000);
        if (wait != WAIT_OBJECT_0) {
            VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);
            MessageBoxW(hDlg, L"Init function timed out", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }

        VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);

        return true;
    } catch (const std::exception& e) {
        std::string msg = "Error: " + std::string(e.what());
        MessageBoxW(hDlg, Widen(msg).c_str(), L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
}

void LaunchGame(const Config& cfg) {
    std::wstring launcherPath = GetModuleDirectory() / L"FrostpunkMultiplayerLauncher.exe";

    std::wstring args = cfg.isHost ? L"--new-instance" : L"--new-instance";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = launcherPath.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOW;
    sei.lpDirectory = GetModuleDirectory().c_str();

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        std::string msg = "Failed to launch: " + GetWin32Error(err);
        MessageBoxW(nullptr, Widen(msg).c_str(), L"Error", MB_OK | MB_ICONERROR);
    }
}

int RunGUI(HINSTANCE hInstance) {
    if (ShowConfigDialog(hInstance)) {
        LaunchGame(g_Config);
        return 0;
    }
    return 1;
}

} // namespace FrostMP::LauncherGUI

// Resource IDs
#define IDD_MAIN                    101
#define IDC_RADIO_HOST              1001
#define IDC_RADIO_CLIENT            1002
#define IDC_EDIT_LOCAL_STEAMID      1003
#define IDC_EDIT_HOST_STEAMID       1004
#define IDC_EDIT_GAME_PATH          1005
#define IDC_BUTTON_BROWSE           1006
#define IDC_CHECK_SAVE              1007
#define IDC_STATIC_HOST_LABEL       1008
#define IDC_BUTTON_FORCE_INVITE     1009

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Check for --new-instance flag (launched from GUI after config)
    bool newInstance = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--new-instance") == 0) {
            newInstance = true;
            break;
        }
    }

    if (newInstance) {
        // Delegate to console launcher logic (argv still valid here)
        int result = FrostMP::Launcher::Run(argc, argv);
        if (argv) LocalFree(argv);
        return result;
    }

    if (argv) LocalFree(argv);

    // Normal GUI mode
    return FrostMP::LauncherGUI::RunGUI(hInstance);
}