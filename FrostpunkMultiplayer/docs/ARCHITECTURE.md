# Frostpunk Multiplayer — Technical Architecture

**Date:** 2026-11-13
**Version:** 1.0 (Target Architecture for MVP)
**Status:** Design document — reflects intended architecture after critical fixes

---

## 1. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        FROSTPUNK PROCESS (Injected)                         │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                      FrostMenuMod.dll                               │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │   │
│  │  │  ModCore │  │GameReader│  │GameWriter│  │   HookManager    │   │   │
│  │  │(Lifecycle)│  │ (Access) │  │ (Mutate) │  │  (Intercept)     │   │   │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────────┬─────────┘   │   │
│  │       │             │             │             │             │   │   │
│  │       ▼             ▼             ▼             ▼             │   │   │
│  │  ┌──────────────────────────────────────────────────────────┐  │   │
│  │  │              SessionManager (State Machine)              │  │   │
│  │  │  Host/Client • Ready/NotReady • Sync/InGame • Disconnect │  │   │
│  │  └────────────────────────┬─────────────────────────────────┘  │   │
│  │                           │                                    │   │
│  │       ┌───────────────────┼───────────────────┐               │   │
│  │       ▼                   ▼                   ▼               │   │
│  │  ┌─────────┐         ┌─────────┐         ┌─────────┐        │   │
│  │  │ Network │         │  Time   │         │Resource │        │   │
│  │  │ Manager │         │  Sync   │         │ Transfer│        │   │
│  │  └────┬────┘         └────┬────┘         └────┬────┘        │   │
│  │       │                   │                   │              │   │
│  │       ▼                   ▼                   ▼              │   │
│  │  ┌────────────────────────────────────────────────────────┐  │   │
│  │  │           SteamNetworkingSockets (P2P)                 │  │   │
│  │  │  Reliable/Unreliable • Sequencing • Heartbeat • NAT    │  │   │
│  │  └────────────────────────────────────────────────────────┘  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              ▲                                      │
│                              │ IPC (Shared Memory)                  │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              OverlayManager (External Window)               │   │
│  │  Transparent Click-Through • Direct2D • Input Handling      │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
                              ▲
                              │ Injection
                              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                    FrostpunkMultiplayerLauncher.exe                         │
│  • Game detection & SHA256 verification                                     │
│  • Version compatibility matrix                                             │
│  • DLL injection (CreateRemoteThread + LoadLibraryW)                        │
│  • Config GUI (Host/Client, SteamID64, Game Path)                           │
│  • Force-inject into running game                                           │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Component Specifications

### 2.1 ModCore (`ModMain.h/cpp`)

**Responsibility:** Mod lifecycle, subsystem coordination, main update loop.

**Interface:**
```cpp
class FrostpunkMultiplayerMod {
public:
    bool Initialize(u64 localSteamID, NetworkManager::Role role, u64 hostSteamID);
    void Shutdown();
    void Update();  // Called from background thread @ ~60Hz
    
    // State queries
    bool IsHost() const;
    bool IsClient() const;
    bool IsInGame() const;
    bool IsInitialized() const { return initialized_; }
    
    // Network callbacks (called from NetworkManager)
    void OnConnected(u64 peerSteamID);
    void OnDisconnected(u64 peerSteamID, const char* reason);
    void OnGameStateReceived(u64 sender, const GameStatePayload* state);
    void OnGameStateDeltaReceived(u64 sender, const GameStateDeltaPayload* delta);
    void OnResourceTransferReceived(u64 sender, const ResourceTransferPayload* xfer);
    void OnResourceTransferResultReceived(u64 sender, const ResourceTransferResultPayload* result);
    // ... other packet handlers
    
private:
    // Subsystems (owned, initialized in order)
    LocalMemory mem_;
    std::unique_ptr<GameReader> reader_;
    std::unique_ptr<GameWriter> writer_;
    std::unique_ptr<HookManager> hooks_;
    std::unique_ptr<NetworkManager> network_;
    std::unique_ptr<OverlayManager> overlay_;
    std::unique_ptr<TimeSync> timeSync_;      // NEW: extracted from ModMain
    std::unique_ptr<ResourceTransferManager> resourceXfer_; // NEW
    
    // Session state
    NetworkManager::Role role_;
    u64 localSteamID_, hostSteamID_, clientSteamID_;
    bool initialized_ = false;
    bool inGame_ = false;
    // ...
};
```

**Initialization Order (deferred, not in DllMain):**
1. `mem_` — LocalMemory (trivial)
2. `reader_` / `writer_` — GameReader/GameWriter (resolve RVAs, validate vtables)
3. `hooks_` — HookManager (install hooks AFTER reader/writer validated)
4. `overlay_` — OverlayManager (create shared memory + window)
5. `network_` — NetworkManager (init Steam, create P2P sockets)
6. `timeSync_` — TimeSync (initialize after network connected)
7. `resourceXfer_` — ResourceTransferManager
8. Start background update thread

**Shutdown Order (reverse):**
1. Signal update thread exit, join
2. `network_->Disconnect()`, `network_->Shutdown()`
3. `hooks_->RemoveAllHooks()`
4. `overlay_->Shutdown()`
5. Reset all unique_ptrs

---

### 2.2 GameReader (`GameReader.h/cpp`)

**Responsibility:** Read-only access to game state. Never writes.

**Key Methods:**
```cpp
class GameReader {
public:
    bool Initialize();  // Resolve globals, validate vtables
    
    // Time
    std::optional<int> GetGameSpeed() const;        // 0=1x, 1=3x, 2=12x
    std::optional<bool> GetPaused() const;
    std::optional<int64_t> GetGameTimeMs() const;   // Calendar time → ms
    
    // Economy
    std::optional<int> GetResource(ResourceID id) const;
    std::array<std::optional<int>, 6> GetAllResources() const;
    
    // Hope/Discontent (read-only)
    std::optional<int> GetHope() const;
    std::optional<int> GetDiscontent() const;
    
    // Session
    std::optional<std::string> GetCurrentSaveName() const;
    GameStateSnapshot GetFullSnapshot() const;  // Aggregated state
    bool IsInGame() const;
    bool IsLoading() const;
    
private:
    const LocalMemory& mem_;
    std::uintptr_t timeController_ = 0;
    std::uintptr_t economy_ = 0;
    std::uintptr_t hope_ = 0;
    std::uintptr_t discontent_ = 0;
    std::uintptr_t saveOwner_ = 0;
};
```

**Validation:** Every `Initialize()` call verifies vtable pointers against expected RVAs. Failure → return `false`, mod stays uninitialized.

---

### 2.3 GameWriter (`GameWriter.h/cpp`)

**Responsibility:** Mutate game state via game's own internal functions. Never direct memory writes to data structures.

**Key Methods:**
```cpp
class GameWriter {
public:
    bool Initialize();  // Resolve function pointers
    
    // Time control (Host only, enforced by hooks)
    bool SetGameSpeed(int speedMode);  // 0,1,2
    bool SetPaused(bool paused);
    
    // Resources (Host only, enforced by hooks)
    bool ChangeResource(ResourceID id, int delta);  // Uses game's ChangeResource fn
    bool SetResource(ResourceID id, int amount);    // Delta from current
    std::optional<int> GetResourceAmount(ResourceID id) const;  // Read via reader
    
    // Save/Load (Host only)
    bool SaveGame(const char* saveName, bool isMultiplayer);
    bool LoadGame(const char* saveName);
    
    // Map/Game start (Host only)
    bool SelectMap(int mapIndex);
    bool StartEndlessGame();
    
private:
    const LocalMemory& mem_;
    // Function pointers resolved at Initialize()
    using SetSpeedFn = bool(*)(uintptr_t timeController, int speedMode);
    using ChangeResourceFn = bool(*)(uintptr_t economy, uintptr_t resourceEntry, 
                                      int delta, void* eventContext, int changeType, bool option);
    // ... etc
    SetSpeedFn setSpeed_ = nullptr;
    ChangeResourceFn changeResource_ = nullptr;
    // ...
    uintptr_t timeController_ = 0;
    uintptr_t economy_ = 0;
    uintptr_t saveOwner_ = 0;
};
```

**Safety:** All writes go through game's validated functions. Hooks intercept to enforce Host-authority.

---

### 2.4 HookManager (`HookManager.h/cpp`) — REDESIGNED

**Responsibility:** Safe runtime function hooking with trampolines.

**Design Principles:**
1. **No `std::function` in executable memory** — use static trampoline functions
2. **Instruction relocation** — disassemble prologue, rewrite relative ops
3. **Memory protection** — `VirtualProtect` + `FlushInstructionCache`
4. **Thread safety** — suspend all threads during install/remove
5. **Verifiable** — read-back verification after write

**Interface:**
```cpp
class HookManager {
public:
    struct Hook {
        uintptr_t targetAddress = 0;
        std::vector<u8> originalBytes;  // Complete original prologue
        void* trampoline = nullptr;     // Executable: original + jmp back
        void* detour = nullptr;         // Executable: save regs → call static handler → restore → jmp trampoline/ret
        bool enabled = false;
        std::string name;
    };

    // Install hook with static C callback (no captures)
    using HookHandler = bool(*)(HookContext* ctx);  // Return true=continue, false=skip
    
    bool InstallHook(const std::string& name, uintptr_t targetRVA, HookHandler handler, size_t minPrologueSize = 5);
    bool EnableHook(const std::string& name);
    bool DisableHook(const std::string& name);
    void RemoveHook(const std::string& name);
    void RemoveAllHooks();
    bool IsHooked(const std::string& name) const;
    void* GetTrampoline(const std::string& name) const;  // For CallOriginal
    
    // Context passed to handler (registers at hook point)
    struct HookContext {
        uintptr_t rcx, rdx, r8, r9;  // First 4 integer args (x64 Windows)
        uintptr_t returnAddress;      // Original return address
        // Stack args accessible via rsp offset
    };
    
private:
    const LocalMemory& mem_;
    std::unordered_map<std::string, Hook> hooks_;
    std::unordered_map<std::string, HookHandler> handlers_;
    // Disassembler instance (Zydis or minimal decoder)
};
```

**Static Handlers (in ModMain, no captures):**
```cpp
// Called from detour assembly
extern "C" bool HookHandler_SetSpeed(HookManager::HookContext* ctx);
extern "C" bool HookHandler_ChangeResource(HookManager::HookContext* ctx);
extern "C" uintptr_t HookHandler_PauseAddReason(HookManager::HookContext* ctx);
extern "C" void HookHandler_PauseRemoveReason(HookManager::HookContext* ctx);
// ... etc
```

**Detour Assembly Pattern (x64):**
```
detour:
  sub rsp, 0x58              ; Shadow space + saved regs + alignment
  mov [rsp+0x20], rcx        ; Save args
  mov [rsp+0x28], rdx
  mov [rsp+0x30], r8
  mov [rsp+0x38], r9
  lea rax, [rsp+0x58]        ; Return address
  mov [rsp+0x40], rax
  lea rcx, [rsp+0x58]        ; Arg1: return address
  lea rdx, [rsp+0x20]        ; Arg2: HookContext* (saved args)
  mov rax, HandlerAddr       ; Static handler function
  call rax                   ; Call handler
  test al, al                ; Check return (bool in AL)
  je skip_original           ; If false, skip original
  mov rcx, [rsp+0x20]        ; Restore args
  mov rdx, [rsp+0x28]
  mov r8, [rsp+0x30]
  mov r9, [rsp+0x38]
  mov rax, TrampolineAddr
  jmp rax                    ; Jump to trampoline (executes original prologue + jmp back)
skip_original:
  add rsp, 0x58
  ret                        ; Return to caller (original skipped)
```

**Trampoline:**
```
trampoline:
  <original prologue instructions, with relative ops relocated>
  jmp TargetAddress + prologueSize  ; Jump back to original function
```

---

### 2.5 NetworkManager (`NetworkManager.h/cpp`)

**Responsibility:** High-level P2P session management, packet routing, state machine.

**State Machine:**
```
Disconnected
    │
    ├── StartHost() ──▶ Connecting ──▶ Listening ──▶ Connected (waiting for client)
    │                                                          │
    │                         Hello received                   │
    │                         HelloAck sent                    │
    ▼                                                          ▼
Connected ◀────────────────────────────────────────────────── Connected
    │
    ├── Host: OnInitialGameStateSent() ──▶ Syncing ──▶ InGame
    │         Client: OnInitialGameStateReceived() ──▶ Syncing ──▶ InGame
    │
    └── Disconnect() ──▶ Disconnected
```

**Packet Types (from NetworkProtocol.h):**
- Connection: Hello, HelloAck, Heartbeat, Disconnect
- Game State: GameState (full), GameStateDelta (incremental)
- Resources: ResourceTransfer, ResourceTransferResult
- Time/Pause/Speed: TimeSync, PauseState, SpeedState
- Chat/Save: ChatMessage, SaveRequest, SaveData, LoadRequest, LoadResponse
- Map/Scenario: MapSelect, MapSelected, GameStart, GameStarted

**Reliability:**
- Reliable: Hello, GameState, ResourceTransfer, Save/Load, MapSelect, GameStart
- Unreliable: Heartbeat, GameStateDelta, ChatMessage
- Sequence numbers per-connection, ACK for reliable (future)

---

### 2.6 TimeSync (NEW — extracted from ModMain)

**Responsibility:** Authoritative game time synchronization.

**Design:**
- Host is time authority
- Host sends `TimeSyncPayload` every 100ms (with GameStateDelta)
- Client measures RTT via Heartbeat, applies offset correction
- Client adjusts local speed temporarily to catch up / slow down
- Desync detection: if |localTime - hostTime| > 5000ms → request full resync

**Interface:**
```cpp
class TimeSync {
public:
    void Initialize(NetworkManager* network, GameReader* reader, GameWriter* writer);
    void Update();  // Called from ModMain::Update
    
    // Host calls
    void OnHostTimeUpdate();  // Broadcast current time
    
    // Client calls (from network handler)
    void OnTimeSyncReceived(const TimeSyncPayload* payload);
    
    // Query
    int64_t GetHostTimeOffset() const;  // hostTime - localTime
    bool IsSynced() const;  // |offset| < threshold
    
private:
    NetworkManager* network_;
    GameReader* reader_;
    GameWriter* writer_;
    
    int64_t hostTimeOffset_ = 0;
    int64_t lastSyncTime_ = 0;
    std::vector<int64_t> rttSamples_;
    
    // Smoothing: exponential moving average of offset
    // Speed adjustment: if offset > 100ms, temporarily adjust speed by ±10%
};
```

---

### 2.7 ResourceTransferManager (NEW)

**Responsibility:** Safe, validated resource transfers with deduplication.

**Interface:**
```cpp
class ResourceTransferManager {
public:
    void Initialize(NetworkManager* network, GameWriter* writer, bool isHost);
    
    // Client calls to request transfer
    bool RequestTransfer(ResourceID resource, int amount, u64 toPlayer);
    
    // Host processes incoming requests
    void OnTransferRequest(u64 fromPlayer, const ResourceTransferPayload* payload);
    
    // Client receives result
    void OnTransferResult(const ResourceTransferResultPayload* result);
    
    // Deduplication
    bool IsProcessed(u32 transferID) const;
    void MarkProcessed(u32 transferID);
    
private:
    NetworkManager* network_;
    GameWriter* writer_;
    bool isHost_;
    std::unordered_set<u32> processedTransfers_;
    std::mutex mutex_;
};
```

**Validation Rules (Host):**
1. `transferID` not already processed
2. `fromPlayer` matches sender
3. `amount > 0` for give, `< 0` for take (but MVP only give)
4. Sender has sufficient resources: `GetResourceAmount(resource) >= amount`
5. Receiver capacity not exceeded (future)
6. On success: `ChangeResource(sender, -amount)`, `ChangeResource(receiver, +amount)`, send Result(Success)
7. On failure: send Result(InsufficientResources/InvalidResource)

---

### 2.8 SessionManager (NEW — extracted from ModMain)

**Responsibility:** Session lifecycle, ready checks, settings negotiation.

**Interface:**
```cpp
class SessionManager {
public:
    enum class State { Lobby, ReadyCheck, Starting, InGame, Ended };
    
    void Initialize(NetworkManager* network, bool isHost);
    void Update();
    
    // Host
    void SetGameSettings(int mapIndex, int scenarioType, int difficulty, u32 seed);
    void SetPlayerReady(u64 steamID, bool ready);
    bool CanStart() const;  // Both players ready + settings match
    
    // Client
    void OnSettingsReceived(const GameStatePayload* settings);
    void OnGameStartReceived(const GameStartPayload* start);
    
    // Callbacks
    using StateChangeCallback = std::function<void(State oldState, State newState)>;
    void SetStateChangeCallback(StateChangeCallback cb);
    
private:
    NetworkManager* network_;
    bool isHost_;
    State state_ = State::Lobby;
    // Settings
    int mapIndex_ = -1;
    int scenarioType_ = 0;
    int difficulty_ = 0;
    u32 randomSeed_ = 0;
    // Ready state
    bool hostReady_ = false;
    bool clientReady_ = false;
    // ...
};
```

---

### 2.9 OverlayManager (`OverlayUI.h/cpp`) — UPGRADED

**Responsibility:** External overlay UI for multiplayer features.

**Changes from Current:**
- **Direct2D** instead of GDI (hardware accelerated, no flicker)
- **Input handling** via Raw Input or Win32 message loop (not hotkeys that conflict)
- **UI Components:** Connection status, Player list, Chat, Trade, Save/Load, Settings
- **Hotkeys:** Configurable, default `Ctrl+Shift+M` (menu), `Ctrl+Shift+T` (trade), `Ctrl+Shift+C` (chat)

**Shared Memory (OverlayControl):**
```cpp
struct OverlayControl {
    // Versioned for compatibility
    u32 signature = 0x464D504F;  // "FMPO"
    u32 version = 2;
    
    // Connection
    std::atomic<int> connectionState{0};
    std::atomic<u64> hostSteamID{0};
    std::atomic<u64> clientSteamID{0};
    std::atomic<u64> localSteamID{0};
    
    // Game state (updated by ModMain)
    std::atomic<int> gameSpeed{0};
    std::atomic<int> isPaused{0};
    std::atomic<long long> gameTimeMs{0};
    std::atomic<int> resources[6]{};
    
    // UI state
    std::atomic<int> showMainMenu{0};
    std::atomic<int> showTradeUI{0};
    std::atomic<int> showChatUI{0};
    std::atomic<int> showSaveUI{0};
    
    // Chat (circular buffer)
    struct ChatMsg { char sender[64]; char msg[512]; u64 time; };
    std::atomic<int> chatHead{0}, chatCount{0};
    ChatMsg chatHistory[200];
    
    // Trade offers
    struct TradeOffer { u32 id; u64 from, to; int res, amount; int status; };
    std::atomic<int> tradeHead{0}, tradeCount{0};
    TradeOffer tradeOffers[100];
    
    // Status message
    char statusMsg[256];
    std::atomic<int> showStatus{0};
};
```

---

## 3. Network Protocol Details

### 3.1 Packet Structure
```
┌─────────────────────────────────────────────────────────────┐
│ PacketHeader (32 bytes)                                     │
├──────────┬────────┬────────┬────────┬──────────┬────────────┤
│ u32 magic│ u16 ver│ u16 type│ u32 seq │ u32 size │ u64 sender │
│ 0xFPMF   │   1    │ PacketType│ seq#   │ payload  │  SteamID64 │
├──────────┴────────┴────────┴────────┴──────────┴────────────┤
│ u64 timestampMs                                              │
└─────────────────────────────────────────────────────────────┘
│ Payload (variable, per PacketType)                          │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Critical Payloads

**GameState (Host→Client, reliable, ~200 bytes):**
- Map/scenario/difficulty/seed
- Game time, pause, speed
- All 6 resources + population + temp + hope/discontent
- Player SteamIDs

**GameStateDelta (Host→Client, unreliable, ~80 bytes):**
- Game time, pause, speed (only if changed)
- Resource deltas (only changed)
- Sequence for ordering

**ResourceTransfer (Client→Host, reliable):**
- transferID (timestamp-based unique)
- Resource type, amount
- fromPlayer, toPlayer

**TimeSync (Host→Client, unreliable, 24 bytes):**
- hostGameTimeMs, hostRealTimeMs, sequence

---

## 4. Security Model

1. **No trust** — All incoming packets validated:
   - Magic + version check
   - Payload size matches type
   - Sender SteamID matches expected peer
   - Sequence monotonic (with wrap handling)
   - Resource amounts non-negative, within bounds

2. **Host Authority** — Client never writes game state directly:
   - Hooks block client `SetSpeed`, `ChangeResource`, `PauseAddReason`
   - Client only sends requests; host validates & applies
   - Time sync: host broadcasts, client adjusts

3. **No Secrets in Memory** — SteamIDs not secret, no crypto keys

4. **P2P Only** — No external servers, no relay

---

## 5. Build & Deployment

### 5.1 Build Configuration
- **Toolset:** VS2022 (v143), C++20
- **Target:** x64 Release
- **Runtime:** `/MT` (static CRT) for DLL to avoid CRT version mismatch
- **Optimizations:** `/O2`, `/Ob2`, `/GL` (LTCG)
- **Security:** `/guard:cf`, `/DYNAMICBASE`, `/NXCOMPAT`, `/SAFESEH`

### 5.2 Output Structure
```
Release/
├── FrostpunkMultiplayerLauncher.exe  (~120 KB)
├── FrostMenuMod.dll                   (~200 KB)
├── FrostMenuMod.pdb                   (for debugging)
├── multiplayer.ini                    (default config)
└── README.txt                         (install instructions)

Release_Package/FrostpunkMultiplayer_vX.Y.Z.zip
├── FrostpunkMultiplayerLauncher.exe
├── FrostMenuMod.dll
├── multiplayer.ini
├── README.txt
├── INSTALL.txt
├── CHANGELOG.txt
├── LICENSE.txt
└── SHA256SUMS.txt
```

---

## 6. Version Compatibility Matrix

| Mod Version | Frostpunk Version | SHA256 | Status |
|-------------|-------------------|--------|--------|
| 1.0.0       | 1.6.1             | 719c6e... | Legacy |
| 1.1.0       | 1.6.2             | TBD     | **Target** |
| 1.1.0       | 1.6.1             | 719c6e... | Supported (fallback) |

**Runtime Check:**
1. Launcher computes SHA256 of `Frostpunk.exe`
2. Matches against known good hashes
3. If unknown: prompt user, allow override with warning
4. Mod re-validates on Initialize() via vtable checks

---

## 7. Testing Strategy

### 7.1 Unit Tests (GoogleTest)
- HookManager: install/remove, trampoline execution, relative instruction relocation
- NetworkManager: state machine, packet serialization, sequence handling
- GameReader/Writer: against known memory snapshots (mocked)
- TimeSync: offset calculation, speed adjustment, desync detection
- ResourceTransfer: validation, deduplication, negative balance prevention

### 7.2 Integration Tests
- Local loopback: two Mod instances in same process (separate memory regions)
- Single-machine two-process: launcher injects into two Frostpunk instances
- Network latency simulation: `tc` / Clumsy for packet loss, latency, reordering

### 7.3 E2E Tests (Two Machines)
- Steam P2P connection establishment
- Host creates session, client joins
- Settings negotiation
- Game start synchronization
- Time sync over 30+ minutes
- Resource trades (all 6 types)
- Disconnect/reconnect handling
- Save/load cycle

---

## 8. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| 1.6.2 offsets differ significantly | Medium | High | Pattern scanning fallback; automated offset dumper |
| SteamNetworkingSockets vtable changes | Low | High | Use official SDK headers; validate at runtime |
| HookManager crashes on certain CPUs | Low | Critical | Extensive testing; fallback to MinHook if needed |
| Game updates break offsets | Medium | High | Version pinning; auto-update detection |
| Desync on high latency (>200ms) | Medium | Medium | Adaptive time sync; configurable tolerances |
| Overlay crashes game (D3D conflict) | Low | Medium | Direct2D with proper D3D interop; fallback to GDI |

---

## 9. Future Extensibility (Post-MVP)

1. **4+ Players** — Extend `NetworkManager` peer list, `SessionManager` ready checks
2. **Spectator Mode** — Read-only `GameReader` streaming, no `GameWriter` access
3. **Dedicated Server** — Headless `NetworkManager` + `SessionManager`, no game hooks
4. **Voice Chat** — Steam Voice API integration in overlay
5. **Mod Settings UI** — In-game ImGui/Scaleform panel
6. **Scenario Support** — Story mode sync (complex — requires entity sync)

---

## 10. Appendix: Key RVAs (1.6.1 Reference)

*To be updated after 1.6.2 verification*

| Function | RVA (1.6.1) | Verified 1.6.2? |
|----------|-------------|-----------------|
| SetSpeed | 0x11BD080 | ❌ |
| CalendarConvert | 0x11AE360 | ❌ |
| ChangeResource | 0x168E340 | ❌ |
| PauseAddReason | 0xF6C530 | ❌ |
| PauseRemoveReason | 0xF6CA80 | ❌ |
| NativeSave | 0x11B5F80 | ❌ |
| NativeLoad | 0x11B5E30 | ❌ |
| SelectMap | 0x1A8B880 | ❌ |
| EndlessStart | 0x1A8ADE0 | ❌ |
| GlobalTime | 0x2B6A710 | ❌ |
| GlobalEconomy | 0x3FC93B0 | ❌ |
| GlobalHope | 0x3FD3DE0 | ❌ |
| GlobalDiscontent | 0x3FD1410 | ❌ |
| GlobalSaveOwner | 0x2B68120 | ❌ |

*All RVAs relative to `Frostpunk.exe` base address.*