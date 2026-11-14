#include "Common.h"
#include <commdlg.h>
#include <shellapi.h>
#include <array>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <iostream>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace FrostMP::Launcher {

constexpr wchar_t PROCESS_NAME[] = L"Frostpunk.exe";
// Frostpunk 1.6.1 SHA256
constexpr char EXPECTED_SHA256_V161[] = "719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4";
// Frostpunk 1.6.2 SHA256 - verified 2026-11-13
constexpr char EXPECTED_SHA256_V162[] = "031dbfe529256b7bf532aa11bfaf8ef75ef0618aa0492a970e532566342ba19d";
constexpr wchar_t CONFIG_FILE[] = L"multiplayer.ini";
constexpr wchar_t MOD_DLL[] = L"FrostMenuMod.dll";

enum class GameVersion {
    Unknown = 0,
    V1_6_1 = 1,
    V1_6_2 = 2
};

inline GameVersion DetectGameVersion(const std::string& sha256) {
    if (sha256 == EXPECTED_SHA256_V161) return GameVersion::V1_6_1;
    if (sha256 == EXPECTED_SHA256_V162) return GameVersion::V1_6_2;
    return GameVersion::Unknown;
}

inline const char* GameVersionToString(GameVersion v) {
    switch (v) {
        case GameVersion::V1_6_1: return "1.6.1";
        case GameVersion::V1_6_2: return "1.6.2";
        default: return "Unknown";
    }
}

struct ScopedHandle {
    HANDLE h = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : h(handle) {}
    ~ScopedHandle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    operator HANDLE() const { return h; }
    HANDLE* operator&() { return &h; }
};

std::string GetError(DWORD err = GetLastError()) {
    LPSTR buf = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = len && buf ? std::string(buf, len) : "Error " + std::to_string(err);
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();
    return msg;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), len);
    return r;
}

std::filesystem::path GetLauncherDir() {
    std::wstring path(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    if (!len) throw std::runtime_error("GetModuleFileName failed");
    path.resize(len);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path GetGamePath() {
    auto config = GetLauncherDir() / CONFIG_FILE;
    wchar_t saved[32768]{};
    GetPrivateProfileStringW(L"Game", L"Path", L"", saved, 32768, config.c_str());
    if (std::filesystem::is_regular_file(saved)) return saved;

    const std::array<std::filesystem::path, 3> candidates = {
        std::filesystem::path(L"D:\\Frostpunk\\Frostpunk.exe"),
        std::filesystem::path(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe"),
        std::filesystem::path(L"C:\\Program Files\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe")
    };

    for (auto& c : candidates) if (std::filesystem::is_regular_file(c)) return c;

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Frostpunk.exe\0Frostpunk.exe\0\0";
    ofn.lpstrFile = saved;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = L"Select Frostpunk.exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) throw std::runtime_error("Game selection cancelled");

    WritePrivateProfileStringW(L"Game", L"Path", saved, config.c_str());
    return saved;
}

std::string SHA256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    DWORD objLen = 0, resLen = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &resLen, 0) < 0) { BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("BCryptGetProperty failed"); }
    std::vector<u8> obj(objLen);
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("BCryptCreateHash failed"); }
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("Open file failed: " + GetError()); }
    std::vector<u8> buf(1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buf.data(), (DWORD)buf.size(), &read, nullptr)) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("Read failed: " + GetError()); }
        if (!read) break;
        if (BCryptHashData(hash, buf.data(), read, 0) < 0) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("HashData failed"); }
    }
    std::array<u8, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), 32, 0) < 0) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("FinishHash failed"); }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    std::ostringstream os; os << std::hex << std::setfill('0');
    for (u8 b : digest) os << std::setw(2) << (int)b;
    return os.str();
}

DWORD FindGameProcess() {
    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) throw std::runtime_error("CreateToolhelp32Snapshot failed: " + GetError());
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, PROCESS_NAME) == 0) {
                if (found) throw std::runtime_error("Multiple Frostpunk processes running");
                found = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    return found;
}

std::filesystem::path GetProcessPath(HANDLE proc) {
    std::wstring path(32768, L'\0');
    DWORD sz = (DWORD)path.size();
    if (!QueryFullProcessImageNameW(proc, 0, path.data(), &sz)) throw std::runtime_error("QueryFullProcessImageName failed: " + GetError());
    path.resize(sz);
    return path;
}

std::uintptr_t GetRemoteModuleBase(DWORD pid, const wchar_t* name) {
    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap) return 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) return (std::uintptr_t)me.modBaseAddr;
        } while (Module32NextW(snap, &me));
    }
    return 0;
}

bool IsModuleLoaded(DWORD pid, const std::filesystem::path& path) {
    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap) return false;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            std::error_code ec;
            if (std::filesystem::equivalent(me.szExePath, path, ec) && !ec) return true;
        } while (Module32NextW(snap, &me));
    }
    return false;
}

void WaitForKernel32(DWORD pid, HANDLE proc) {
    for (int i = 0; i < 200; ++i) {
        if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) throw std::runtime_error("Game exited before injection");
        if (GetRemoteModuleBase(pid, L"kernel32.dll")) return;
        Sleep(25);
    }
    throw std::runtime_error("Timeout waiting for game initialization");
}

void InjectDLL(DWORD pid, HANDLE proc, const std::filesystem::path& dllPath) {
    if (IsModuleLoaded(pid, dllPath)) return;

    std::wstring wpath = dllPath.wstring();
    SIZE_T bytes = (wpath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) throw std::runtime_error("VirtualAllocEx failed: " + GetError());
    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote, wpath.c_str(), bytes, &written) || written != bytes) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        throw std::runtime_error("WriteProcessMemory failed: " + GetError());
    }
    HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
    std::uintptr_t localLoadLib = (std::uintptr_t)GetProcAddress(localK32, "LoadLibraryW");
    std::uintptr_t remoteK32 = GetRemoteModuleBase(pid, L"kernel32.dll");
    if (!localLoadLib || !remoteK32) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); throw std::runtime_error("Could not resolve LoadLibraryW"); }
    std::uintptr_t rva = localLoadLib - (std::uintptr_t)localK32;
    auto remoteLoadLib = (LPTHREAD_START_ROUTINE)(remoteK32 + rva);
    ScopedHandle thread(CreateRemoteThread(proc, nullptr, 0, remoteLoadLib, remote, 0, nullptr));
    if (!thread) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); throw std::runtime_error("CreateRemoteThread failed: " + GetError()); }
    DWORD wait = WaitForSingleObject(thread, 30000);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("DLL load timed out (30s)");
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    Sleep(200);
    if (!IsModuleLoaded(pid, dllPath)) throw std::runtime_error("DLL did not appear in target process");
}

void VerifyGame(const std::filesystem::path& path) {
    if (_wcsicmp(path.filename().c_str(), PROCESS_NAME) != 0) throw std::runtime_error("Must select Frostpunk.exe");
    DWORD type = 0;
    if (!GetBinaryTypeW(path.c_str(), &type) || type != SCS_64BIT_BINARY) throw std::runtime_error("Not a valid 64-bit executable");
}

GameVersion VerifyGameVersion(const std::filesystem::path& path) {
    VerifyGame(path);
    std::string sha256 = SHA256(path);
    DebugLog("Game SHA256: %s", sha256.c_str());
    GameVersion version = DetectGameVersion(sha256);
    DebugLog("Detected game version: %s", GameVersionToString(version));
    if (version == GameVersion::Unknown) {
        throw std::runtime_error(
            "Unsupported Frostpunk version.\n"
            "Expected 1.6.1 or 1.6.2.\n"
            "Got SHA256: " + sha256
        );
    }
    return version;
}

void CallModInitFunction(DWORD pid, HANDLE proc, const std::filesystem::path& dllPath, bool isHost, u64 localSteamID, u64 hostSteamID) {
    DebugLog("CallModInitFunction: isHost=%d, localSteamID=%llu, hostSteamID=%llu", isHost, localSteamID, hostSteamID);

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
        DebugLog("CallModInitFunction: Could not find injected DLL in target process");
        return; // Non-fatal - deferred init will handle it
    }

    // Get the exported function address in the target process
    HMODULE localMod = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!localMod) {
        DebugLog("CallModInitFunction: Could not load DLL locally to get exports");
        return;
    }

    using InitHostFn = void(*)(u64);
    using InitClientFn = void(*)(u64, u64);

    std::uintptr_t remoteFuncAddr = 0;
    if (isHost) {
        auto fn = (InitHostFn)GetProcAddress(localMod, "FrostMP_InitializeHost");
        if (!fn) {
            DebugLog("CallModInitFunction: FrostMP_InitializeHost not found in DLL");
            FreeLibrary(localMod);
            return;
        }
        remoteFuncAddr = remoteDllBase + ((std::uintptr_t)fn - (std::uintptr_t)localMod);
    } else {
        auto fn = (InitClientFn)GetProcAddress(localMod, "FrostMP_InitializeClient");
        if (!fn) {
            DebugLog("CallModInitFunction: FrostMP_InitializeClient not found in DLL");
            FreeLibrary(localMod);
            return;
        }
        remoteFuncAddr = remoteDllBase + ((std::uintptr_t)fn - (std::uintptr_t)localMod);
    }
    FreeLibrary(localMod);

    DebugLog("CallModInitFunction: remoteDllBase=0x%llX, remoteFuncAddr=0x%llX", remoteDllBase, remoteFuncAddr);

    // Resolve ExitThread in the remote process so we can cleanly terminate
    // the remote thread after calling the init function.
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

    // Build shellcode for x64:
    // mov rcx, imm64 (localSteamID = 1st arg)
    // [if client] mov rdx, imm64 (hostSteamID = 2nd arg)
    // mov rax, imm64 (remoteFuncAddr)
    // call rax
    // xor rcx, rcx            ; ExitCode = 0
    // mov rax, imm64 (remoteExitThread)
    // call rax                ; ExitThread(0) — no 'ret' on a bare remote thread stack
    std::vector<u8> shellcode;
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
    // sub rsp, 0x28  ; reserve 40 bytes (>= 32 shadow space + alignment slack)
    // and rsp, -16   ; round down to 16-byte boundary
    shellcode.insert(shellcode.end(), {0x48, 0x83, 0xEC, 0x28}); // sub rsp, 0x28
    shellcode.insert(shellcode.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16

    // call rax
    shellcode.insert(shellcode.end(), {0xFF, 0xD0});

    // xor rcx, rcx (ExitCode = 0)
    shellcode.insert(shellcode.end(), {0x48, 0x31, 0xC9});

    // call remoteExitThread
    if (remoteExitThread) {
        shellcode.insert(shellcode.end(), {0x48, 0xB8}); // mov rax, imm64
        {
            size_t argOffset = shellcode.size();
            shellcode.resize(shellcode.size() + 8);
            *reinterpret_cast<std::uintptr_t*>(&shellcode[argOffset]) = remoteExitThread;
        }
        shellcode.insert(shellcode.end(), {0xFF, 0xD0}); // call rax
    } else {
        // Fallback: int3 (breakpoint) if we can't resolve ExitThread
        shellcode.push_back(0xCC);
    }

    // Allocate and write shellcode in target process
    void* remoteShellcode = VirtualAllocEx(proc, nullptr, shellcode.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteShellcode) {
        DebugLog("CallModInitFunction: VirtualAllocEx for shellcode failed");
        return;
    }

    SIZE_T writtenShellcode = 0;
    if (!WriteProcessMemory(proc, remoteShellcode, shellcode.data(), shellcode.size(), &writtenShellcode) || writtenShellcode != shellcode.size()) {
        VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);
        DebugLog("CallModInitFunction: WriteProcessMemory for shellcode failed");
        return;
    }

    // Execute shellcode via CreateRemoteThread. lpParameter is unused (shellcode has immediates).
    DebugLog("CallModInitFunction: Creating remote thread at shellcode, isHost=%d, localSteamID=%llu, hostSteamID=%llu, remoteFuncAddr=0x%llX",
             isHost, localSteamID, hostSteamID, remoteFuncAddr);
    ScopedHandle initThread(CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)remoteShellcode, nullptr, 0, nullptr));
    if (!initThread) {
        VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);
        DebugLog("CallModInitFunction: CreateRemoteThread for shellcode failed: %s", GetError().c_str());
        return;
    }

    // Wait for init to complete. Give generous timeout - init does hooks, network, overlay.
    DWORD wait = WaitForSingleObject(initThread, 30000);
    DWORD threadExitCode = 0;
    GetExitCodeThread(initThread, &threadExitCode);
    DebugLog("CallModInitFunction: remote thread wait=%lu, exitCode=0x%lX", wait, threadExitCode);

    if (wait != WAIT_OBJECT_0) {
        DebugLog("CallModInitFunction: Init function timed out (thread may still be running)");
        // Do NOT free shellcode - thread may still be executing it. Leak is safer than crash.
        return;
    }

    // Safe to free shellcode now that the remote thread has finished.
    VirtualFreeEx(proc, remoteShellcode, 0, MEM_RELEASE);

    DebugLog("CallModInitFunction: Init function called successfully (thread exitCode=0x%lX)", threadExitCode);
}

void LaunchOrAttach(const std::filesystem::path& dllPath, bool newInstance, DWORD attachPid) {
    DebugLog("LaunchOrAttach: newInstance=%d, attachPid=%lu", newInstance, attachPid);
    DWORD pid = attachPid ? attachPid : (newInstance ? 0 : FindGameProcess());
    DebugLog("LaunchOrAttach: target PID=%lu", pid);

    // Read config for role and Steam IDs
    auto config = GetLauncherDir() / CONFIG_FILE;
    wchar_t roleStr[32] = {};
    GetPrivateProfileStringW(L"Multiplayer", L"Role", L"Host", roleStr, 32, config.c_str());
    bool isHost = (wcscmp(roleStr, L"Host") == 0);

    wchar_t localSteamIdStr[64] = {};
    GetPrivateProfileStringW(L"Multiplayer", L"LocalSteamID", L"", localSteamIdStr, 64, config.c_str());
    u64 localSteamID = 0;
    if (localSteamIdStr[0]) {
        ParseSteamID64(localSteamIdStr, localSteamID);
    }

    wchar_t hostSteamIdStr[64] = {};
    GetPrivateProfileStringW(L"Multiplayer", L"HostSteamID", L"", hostSteamIdStr, 64, config.c_str());
    u64 hostSteamID = 0;
    if (hostSteamIdStr[0]) {
        ParseSteamID64(hostSteamIdStr, hostSteamID);
    }

    if (pid) {
        DebugLog("LaunchOrAttach: Attaching to existing process");
        ScopedHandle proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, pid));
        if (!proc) throw std::runtime_error("OpenProcess failed: " + GetError());
        auto gamePath = GetProcessPath(proc);
        GameVersion version = VerifyGameVersion(gamePath);
        WaitForKernel32(pid, proc);
        InjectDLL(pid, proc, dllPath);
        // Call exported init function after injection
        Sleep(500); // Give DLL time to initialize
        CallModInitFunction(pid, proc, dllPath, isHost, localSteamID, hostSteamID);
        return;
    }
    DebugLog("LaunchOrAttach: Starting new instance");
    auto gamePath = std::filesystem::weakly_canonical(GetGamePath());
    DebugLog("LaunchOrAttach: gamePath=%s", gamePath.string().c_str());
    GameVersion version = VerifyGameVersion(gamePath);
    std::wstring wd = gamePath.parent_path().wstring();
    DebugLog("LaunchOrAttach: workingDir=%s", Narrow(wd).c_str());
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DebugLog("LaunchOrAttach: Calling CreateProcessW");
    // Normal process creation (not suspended) - we'll wait for kernel32 then inject
    if (!CreateProcessW(gamePath.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, wd.c_str(), &si, &pi))
        throw std::runtime_error("CreateProcess failed: " + GetError());
    DebugLog("LaunchOrAttach: CreateProcessW succeeded, PID=%lu", pi.dwProcessId);
    std::wcout << L"Game process started, PID=" << pi.dwProcessId << std::endl;
    ScopedHandle proc(pi.hProcess), thread(pi.hThread);

    // Wait for kernel32 to load, then inject
    DebugLog("LaunchOrAttach: Waiting for kernel32 in new process");
    WaitForKernel32(pi.dwProcessId, proc);

    // Inject DLL
    DebugLog("LaunchOrAttach: Injecting DLL");
    InjectDLL(pi.dwProcessId, proc, dllPath);
    DebugLog("LaunchOrAttach: Injection complete");
    std::wcout << L"DLL injected successfully" << std::endl;

    // Call exported init function after injection
    Sleep(500); // Give DLL time to initialize
    CallModInitFunction(pi.dwProcessId, proc, dllPath, isHost, localSteamID, hostSteamID);

    // Wait a bit to see if game stays alive
    for (int i = 0; i < 50; ++i) {
        DWORD code = 0;
        if (GetExitCodeProcess(proc, &code)) {
            if (code != STILL_ACTIVE) {
                DebugLog("LaunchOrAttach: Game process exited with code %lu after %dms", code, i * 100);
                break;
            }
        }
        Sleep(100);
    }
    DWORD finalCode = 0;
    GetExitCodeProcess(proc, &finalCode);
    DebugLog("LaunchOrAttach: Final game process code: %lu", finalCode);
}

int Run(int argc, wchar_t* argv[]) {
    try {
        std::wcout << L"Frostpunk Multiplayer Launcher v1.0.0" << std::endl;
        std::wcout << L"Working directory: " << GetLauncherDir().wstring() << std::endl;

        DebugLog("Launcher Run: argc=%d", argc);
        for (int i = 0; i < argc; ++i) {
            char buf[256];
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buf, sizeof(buf), nullptr, nullptr);
            DebugLog("  argv[%d] = %s", i, buf);
        }

        bool newInstance = false;
        DWORD attachPid = 0;
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--new-instance") == 0) newInstance = true;
            else if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
                std::wstring s = argv[++i];
                if (s.empty() || s.find_first_not_of(L"0123456789") != std::wstring::npos) throw std::runtime_error("Invalid PID");
                unsigned long long v = std::stoull(s);
                if (!v || v > MAXDWORD) throw std::runtime_error("PID out of range");
                attachPid = (DWORD)v;
            } else throw std::runtime_error("Usage: Launcher.exe [--new-instance | --pid PID]");
        }
        if (newInstance && attachPid) throw std::runtime_error("Use --new-instance or --pid, not both");

        // Ensure multiplayer.ini exists next to the launcher. Create defaults if missing.
        auto configPath = GetLauncherDir() / CONFIG_FILE;
        if (!std::filesystem::is_regular_file(configPath)) {
            DebugLog("Launcher: multiplayer.ini not found, creating defaults at %s", configPath.string().c_str());
            std::wstring iniContent =
                L"[Multiplayer]\r\n"
                L"Role=Host\r\n"
                L"LocalSteamID=76561199020386575\r\n"
                L"HostSteamID=76561199019505166\r\n"
                L"\r\n"
                L"[Game]\r\n"
                L"Path=C:\\Program Files (x86)\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe\r\n";
            std::error_code ec;
            std::filesystem::create_directories(configPath.parent_path(), ec);
            std::ofstream iniFile(configPath, std::ios::out | std::ios::trunc);
            if (iniFile) {
                iniFile << Narrow(iniContent);
                iniFile.close();
                DebugLog("Launcher: multiplayer.ini created with defaults");
            } else {
                DebugLog("Launcher: WARNING - could not create multiplayer.ini");
            }
        }

        auto dllPath = std::filesystem::weakly_canonical(GetLauncherDir() / MOD_DLL);
        DebugLog("Launcher: dllPath = %s", dllPath.string().c_str());
        if (!std::filesystem::is_regular_file(dllPath)) throw std::runtime_error("FrostMenuMod.dll not found next to launcher");

        DebugLog("Launcher: Calling LaunchOrAttach");
        LaunchOrAttach(dllPath, newInstance, attachPid);
        DebugLog("Launcher: LaunchOrAttach returned");
        return 0;
    } catch (const std::exception& e) {
        DebugLog("Launcher exception: %s", e.what());
        std::wcerr << L"ERROR: " << Widen(e.what()) << std::endl;
        MessageBoxW(nullptr, Widen(e.what()).c_str(), L"Frostpunk Multiplayer Launcher", MB_OK | MB_ICONERROR);
        return 1;
    }
}

} // namespace FrostMP::Launcher

int wmain(int argc, wchar_t* argv[]) {
    return FrostMP::Launcher::Run(argc, argv);
}
