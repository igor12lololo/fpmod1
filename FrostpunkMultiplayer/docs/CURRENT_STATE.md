# Frostpunk Multiplayer — Current Project State

**Date:** 2026-11-13
**Target Game Version:** Frostpunk 1.6.2 (Steam)
**Project Path:** `G:\claude_code\FrostpunkMultiplayer`

---

## 1. Project Structure Overview

```
FrostpunkMultiplayer/
├── src/
│   ├── Common/              # Shared utilities, logging, types
│   │   ├── Common.h         # Core types, string utils, error handling, timestamp
│   │   └── Logger.h         # Structured logging with file output
│   ├── GameHooks/           # Game memory access and hooking
│   │   ├── GameStructs.h    # Offsets, RVAs, structures for Frostpunk 1.6.1
│   │   ├── Memory.h/.cpp    # LocalMemory (SEH-safe) + remote Memory
│   │   ├── GameReader.h/.cpp # Read-only game state access
│   │   ├── GameWriter.h/.cpp # Write game state via internal functions
│   │   ├── HookManager.h/.cpp # Runtime function hooking (trampoline-based)
│   ├── Network/             # Steam P2P networking
│   │   ├── NetworkProtocol.h # Packet definitions, serialization
│   │   ├── PacketSerializer.h # Fluent packet builder
│   │   ├── NetworkManager.h/.cpp # High-level network state machine
│   │   ├── SteamNetworking.h/.cpp # SteamNetworkingSockets interface
│   ├── Overlay/             # External overlay UI
│   │   ├── OverlayUI.h/.cpp  # Shared memory IPC + GDI overlay window
│   ├── ModDLL/              # Injected DLL entry point
│   │   ├── DllMain.cpp      # DLL entry, auto-init from INI
│   │   ├── ModMain.h/.cpp   # Main mod class, lifecycle, hooks, sync
│   ├── Launcher/            # Standalone launcher (GUI + console)
│   │   ├── Launcher.cpp     # Injection logic, SHA256 verification
│   │   ├── LauncherGUI.cpp  # Config dialog, force-inject into running game
│   ├── Test/                # Test application
│       └── TestMod.cpp      # Protocol/struct unit tests
├── build/                   # MSBuild intermediates
├── Release/                 # Built binaries
│   ├── FrostMenuMod.dll
│   ├── FrostpunkMultiplayerLauncher.exe
│   ├── multiplayer.ini
│   └── steam_api64.dll
├── Release_Package/         # Distribution package
├── FrostpunkMultiplayer_v1.0.0/ # Older release
├── Build-All.ps1 / Build-ModDLL.ps1 / Build-Launcher.ps1 / Package-Release.ps1
└── ARCHITECTURE.md          # Existing architecture documentation
```

---

## 2. Component Status Summary

| Component | Status | Notes |
|-----------|--------|-------|
| **Launcher** | ✅ Built, functional | Injects DLL, verifies SHA256, supports --new-instance/--pid |
| **DllMain** | ⚠️ **CRITICAL ISSUES** | Calls heavy init in DLL_PROCESS_ATTACH, no deferred init |
| **ModMain** | 🟡 Partially functional | Complex init sequence, network init may fail but returns true |
| **Memory (LocalMemory)** | ✅ Functional | SEH-safe read/write, RVA resolution |
| **GameReader** | 🟡 Functional for 1.6.1 | Offsets hardcoded for 1.6.1, no 1.6.2 verification |
| **GameWriter** | 🟡 Functional for 1.6.1 | Function pointers resolved at init, no version check |
| **HookManager** | ⚠️ **CRITICAL ISSUES** | Multiple architectural flaws (detailed below) |
| **SteamNetworking** | 🟡 Complex, fragile | Multiple fallback methods, vtable index assumptions |
| **NetworkManager** | 🟡 State machine exists | Packet handling, heartbeats, but some handlers incomplete |
| **NetworkProtocol** | ✅ Well-defined | PacketHeader + typed payloads, sequence numbers |
| **Overlay** | 🟡 Basic GDI | Shared memory IPC works, rendering is minimal |

---

## 3. Critical Issues Identified (Per Task Requirements)

### 3.1 HookManager — CRITICAL

**Location:** `src/GameHooks/HookManager.cpp/h`

**Issues Found:**

1. **`std::function` address used as callback address** (line 69, 109, 118):
   ```cpp
   auto callbackPtr = new HookCallback(std::move(callback));
   // Then pointer to this heap object used as code address in detour
   *reinterpret_cast<std::uintptr_t*>(&detourCode[detourCode.size()]) = reinterpret_cast<std::uintptr_t>(callbackPtr);
   ```
   This passes a pointer to a `std::function` object as if it were executable code. The detour does `call rax` where `rax` points to the `std::function` object, not a function. This **will crash**.

2. **JMP size (14 bytes) vs prologueSize mismatch**:
   - Hook uses 14-byte absolute JMP (`FF 25 00 00 00 00 + 8-byte addr`)
   - `InstallHook` reads `hookSize` bytes (default 14)
   - `InstallCallbackHook` reads `prologueSize` bytes (default 5)
   - If original prologue > 14 bytes, trampoline returns into overwritten code
   - If original prologue < 14 bytes, original bytes not fully saved

3. **No relative instruction fixup in trampoline**:
   - Trampoline copies raw bytes: `std::memcpy(trampoline, original.data(), prologueSize)`
   - If prologue contains `CALL rel32`, `JMP rel32`, `Jcc rel32`, or RIP-relative memory access, they will execute with wrong RIP in trampoline
   - Must disassemble and relocate relative instructions

4. **No memory protection / instruction cache handling**:
   - `VirtualAlloc` with `PAGE_EXECUTE_READWRITE` used for trampoline/detour
   - Target memory written via `WriteProcessMemory` / `WriteLocalRaw` without `VirtualProtect`
   - No `FlushInstructionCache` after writing code
   - On some CPUs/Windows versions, this causes stale instruction cache execution

5. **No thread synchronization during hook install/remove**:
   - Other threads may be executing the target function while bytes are being overwritten
   - No `SuspendThread` / `WaitForThreadpoolWorkCallbacks` or similar
   - Race condition: thread reads half-old half-new instruction

6. **False success reporting**:
   - `EnableHook` returns `true` if `WriteLocalRaw` succeeds
   - `WriteLocalRaw` uses SEH `__try/__except` — catches AV but not partial writes
   - No verification that written bytes match intended bytes

7. **Callback memory leak / double-free risk**:
   - `callbackPtrs_` map stores `HookCallback*` allocated with `new`
   - `RemoveHook` deletes it, but `RemoveAllHooks` also iterates and deletes
   - If `RemoveHook` called then `RemoveAllHooks`, double-free
   - `callbacks_` map stores *copy* of `std::function` — if callback captures `this`, dangling reference after mod shutdown

### 3.2 DllMain — CRITICAL

**Location:** `src/ModDLL/DllMain.cpp` lines 153-169

```cpp
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DebugLog("DllMain: DLL_PROCESS_ATTACH");
            DisableThreadLibraryCalls(hModule);
            FrostMP::Mod::TryAutoInitialize(hModule);  // HEAVY INIT HERE
            DebugLog("DllMain: TryAutoInitialize returned");
            break;
        case DLL_PROCESS_DETACH:
            DebugLog("DllMain: DLL_PROCESS_DETACH");
            if (lpReserved == nullptr) {
                FrostMP::Mod::ShutdownMod();
            }
            break;
    }
    return TRUE;
}
```

**Issues:**
- `TryAutoInitialize` reads INI file, parses SteamIDs, calls `InitializeMod`
- `InitializeMod` creates `FrostpunkMultiplayerMod` which:
  - Creates `GameReader`, `GameWriter`, `HookManager`, `NetworkManager`, `OverlayManager`
  - Calls `HookGameFunctions()` — installs 20+ hooks with complex detour generation
  - Calls `overlay_->Initialize()` — creates window, shared memory
  - Calls `network_->Initialize()` — loads SteamNetworkingSockets DLLs, resolves vtables
  - Calls `network_->StartHost()` / `ConnectToHost()` — creates P2P sockets
  - Creates background thread (`UpdateThread`)
- **All of this runs inside `DllMain`** — violates Windows loader lock rules
- Can deadlock on loader lock, crash on `LoadLibrary`/`GetProcAddress`, fail on thread creation
- No error recovery — if any step fails, DLL still returns `TRUE`

### 3.3 Version Verification — MISSING

**Location:** `src/GameHooks/GameStructs.h` line 7

```cpp
// Frostpunk Liquid Engine structures (based on reverse engineering)
// These are offsets for Frostpunk 1.6.1 (SHA: 719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4)
```

**Issues:**
- All offsets/RVAs hardcoded for **1.6.1 only**
- Launcher verifies SHA256 of `Frostpunk.exe` against 1.6.1 hash (line 15 in Launcher.cpp)
- **No support for 1.6.2** — target version per spec
- No signature scanning / pattern matching for functions
- No graceful degradation if version mismatches
- `GameReader::ValidateVTable` checks vtable pointers but only logs, doesn't fail init

### 3.4 Error Handling — DEFECTIVE

**Location:** `src/ModDLL/ModMain.cpp` lines 30-161

```cpp
bool FrostpunkMultiplayerMod::Initialize(...) {
    // ... hook install ...
    DebugLog("Game hooks installed OK");  // Line 49 - assumes success

    // ... overlay init ...
    if (!overlay_->Initialize(localSteamID_)) {
        DebugLog("Failed to initialize overlay");  // Line 54 - logs but continues
    }

    // ... network init ...
    bool networkOk = network_->Initialize(localSteamID_);  // Line 68
    if (networkOk) {
        // Register handlers, start host/client
        bool success = false;
        if (role_ == Role::Host) success = network_->StartHost();
        else success = network_->ConnectToHost(hostSteamID_);

        if (!success) {
            network_->Shutdown();  // Line 150
            networkOk = false;
        }
    } else {
        DebugLog("Network initialization failed, continuing without networking");  // Line 154
    }

    initialized_ = true;  // Line 158 - ALWAYS SETS TRUE
    return true;  // Line 161 - ALWAYS RETURNS TRUE
}
```

**Issues:**
- `initialized_ = true` set **regardless of hook/overlay/network success**
- Network failure → continues without networking, returns `true`
- Hook installation failure not checked (assumes `HookGameFunctions` works)
- No rollback on partial failure
- Caller cannot distinguish "fully working" from "partially broken"

---

## 4. Build & Runtime Verification

### 4.1 Build Scripts
- `Build-ModDLL.ps1` — Creates MSBuild project, compiles `FrostMenuMod.dll`
- `Build-Launcher.ps1` — Creates MSBuild project, compiles `FrostpunkMultiplayerLauncher.exe`
- Both use VS2022 toolset (v143), C++20, Release x64
- Module definition file (`.def`) exports 5 functions

### 4.2 Built Artifacts (in `Release/`)
```
FrostMenuMod.dll          180,736 bytes  (with PDB 4,100,096 bytes)
FrostpunkMultiplayerLauncher.exe  104,960 bytes
multiplayer.ini           Configuration file
steam_api64.dll           295,336 bytes (Steam API)
FrostMP.log               Runtime log
```

### 4.3 Launcher Verification
- SHA256 check hardcoded for 1.6.1 (`EXPECTED_SHA256` in Launcher.cpp:15)
- Injection via `CreateRemoteThread` + `LoadLibraryW`
- Waits for `kernel32.dll` in target process
- Verifies DLL appears in target module list

---

## 5. Network Architecture Assessment

### SteamNetworkingSocketsImpl Analysis

**Loading strategy (SteamNetworking.cpp:11-250):**
1. Tries `SteamInternal_CreateInterface("SteamNetworkingSockets012")`
2. Falls back to `SteamNetworkingSockets011`
3. Tries `SteamNetworkingSockets_LibV12()` from loaded DLL
4. Sets P2P env vars via `SteamNetworkingSockets_SetEnvironmentVariable`
5. Tries `SteamAPI_GetISteamNetworkingSockets` using game's Steam context
6. Last resort: `SteamDatagramClient_Init()` with 3-second timeout

**Risks:**
- VTable indices hardcoded (lines 200-233) — assumes specific Steam SDK version
- `SteamDatagramClient_Init` may hang — async with timeout mitigation
- No version verification of loaded `steamnetworkingsockets.dll`
- Relies on game having initialized SteamAPI (reasonable for Steam game)

**Connection handling:**
- `StartListening()` → `CreateListenSocketP2P` (host)
- `Connect()` → `CreateP2PConnectionSocket` (client)
- `Poll()` calls `SteamAPI_RunCallbacks` + `AcceptConnection` loop
- `ReceiveMessages` uses `ReceiveMessagesOnConnection` with vtable indices 2,3,4

---

## 6. Game Hooks Analysis

### Hooked Functions (ModMain.cpp:768-865)

| Hook | RVA | Purpose | Risk |
|------|-----|---------|------|
| SetSpeed | 0x11BD080 | Block client speed changes | High — callback crash |
| ChangeResource | 0x168E340 | Block client resource changes | High — callback crash |
| PauseAddReason | 0xF6C530 | Track network pause token | High — callback crash |
| PauseRemoveReason | 0xF6CA80 | Clear network pause token | High — callback crash |
| NativeSave | 0x11B5F80 | Sync save to client | Medium |
| NativeLoad | 0x11B5E30 | Notify client to load | Medium |
| SelectMap | 0x1A8B880 | Send map to client | Medium |
| EndlessStart | 0x1A8ADE0 | Notify client to start | Medium |
| MenuUpdate | 0x1A770F0 | Inject MP button | Low (not implemented) |
| MenuBindButtons | 0x1A76B80 | Bind MP button | Low (not implemented) |
| ... | ... | ... | ... |

**All callback hooks use the broken `InstallCallbackHook` mechanism.**

---

## 7. Next Steps Priority

### Immediate (Blockers)
1. **Fix HookManager** — Complete rewrite of trampoline/detour logic
2. **Fix DllMain** — Defer all initialization to background thread
3. **Add version verification** — Support 1.6.2, fail gracefully on mismatch
4. **Fix error handling** — Return actual status, rollback on failure

### Short Term
5. **Verify 1.6.2 offsets** — Update GameStructs.h or add pattern scanning
6. **Test SteamNetworking on two machines** — Current code untested for real P2P
7. **Implement time synchronization** — Core MVP requirement

### Medium Term
8. **Resource transfer validation** — Prevent negative balances, double-processing
9. **Overlay UI improvements** — Direct2D, proper trade/chat UI
10. **Reconnection logic** — Handle transient disconnects

---

## 8. Testing Status

| Test | Status | Notes |
|------|--------|-------|
| Unit tests (TestMod.cpp) | ✅ Compiles | Protocol serialization, structs, SteamID parsing |
| Launcher injection | 🟡 Works locally | Tested on single machine |
| DLL load in target | 🟡 Works locally | Basic init, no game running test |
| Hook installation | ❌ **Broken** | Will crash on callback |
| Steam P2P connection | ❌ **Untested** | Requires 2 Steam accounts, 2 machines |
| Game state reading | 🟡 Untested | Offsets for 1.6.1 only |
| Game state writing | 🟡 Untested | Function pointers for 1.6.1 only |
| Time sync | ❌ Not implemented | Core MVP gap |
| Resource trade | ❌ Not implemented | Core MVP gap |

---

## 9. Conclusion

The project has **substantial infrastructure** but **critical defects** in the three areas explicitly called out in the task:
1. **HookManager** — Architecturally broken, will crash
2. **DllMain** — Heavy initialization in loader lock, unsafe
3. **Version verification** — Only supports 1.6.1, target is 1.6.2
4. **Error handling** — False success reporting throughout

**Recommendation:** Do not proceed to feature development (Stages 2-8) until Stage 0 (Audit) and Stage 1 (Stable Loading) are complete with verified fixes for the above.