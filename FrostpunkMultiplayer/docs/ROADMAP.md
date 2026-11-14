# Frostpunk Multiplayer — Implementation Roadmap

**Date:** 2026-11-13
**Status:** Living document — updated as stages complete
**Methodology:** Sequential stages with explicit exit criteria (per Section 6 of task)

---

## Stage 0: Project Audit ✅ COMPLETE

**Deliverables:**
- [x] `docs/CURRENT_STATE.md` — Full project scan, component status, critical issues
- [x] `docs/ARCHITECTURE.md` — Target architecture with redesigned components
- [x] `docs/BUGS.md` — 24 tracked defects (9 Critical, 6 High, 5 Medium, 4 Low)
- [x] `docs/ROADMAP.md` — This document

**Key Findings:**
1. HookManager architecturally broken (7 critical defects)
2. DllMain violates loader lock rules (heavy init in DLL_PROCESS_ATTACH)
3. Version verification only supports 1.6.1, target is 1.6.2
4. Error handling reports false success throughout

**Decision:** Do not proceed to feature work until Stages 1-2 complete.

---

## Stage 1: Stable Loading (Current Focus)

**Goal:** Frostpunk launches with mod injected, runs 30+ minutes stable, no crashes.

### 1.1 Fix DllMain — Deferred Initialization
**Files:** `src/ModDLL/DllMain.cpp`, `src/ModDLL/ModMain.cpp`

**Changes:**
- DllMain: Only store `hModule`, set `g_DllModule = hModule`, return TRUE
- Add exported `FrostMP_InitializeDeferred()` called by launcher after injection
- Or: Background thread created in DllMain (minimal, just `CreateThread` with init function)
- Move all `TryAutoInitialize` logic to `FrostMP_InitializeHost/Client` exports

**Exit Criteria:**
- [ ] DllMain < 20 lines, no complex logic
- [ ] Launcher calls `FrostMP_InitializeHost/Client` after injection verified
- [ ] No loader lock violations (no LoadLibrary, no thread sync, no I/O in DllMain)

### 1.2 Rewrite HookManager
**Files:** `src/GameHooks/HookManager.h`, `src/GameHooks/HookManager.cpp`

**Changes:**
- Remove `std::function` from executable memory — use static C handlers
- Add minimal x64 disassembler (Zydis submodule or custom 15-instruction decoder)
- Implement relative instruction relocation in trampoline (CALL/JMP/Jcc/RIP-relative)
- Add `VirtualProtect` + `FlushInstructionCache` in EnableHook/DisableHook
- Add thread suspension during install/remove (`SuspendThread` all threads except caller)
- Fix JMP size vs prologue: disassemble to find instruction boundary ≥ 5 bytes
- Single ownership for callbacks — `std::unique_ptr` or static array
- Read-back verification after memory write

**Exit Criteria:**
- [ ] HookManager compiles without warnings
- [ ] Unit test: install hook on known function (e.g., `GetTickCount`), verify trampoline executes original
- [ ] Unit test: hook with relative CALL/JMP in prologue executes correctly
- [ ] Unit test: concurrent hook install/remove from multiple threads safe
- [ ] No `std::function` in detour/trampoline memory regions

### 1.3 Add Version Verification for 1.6.2
**Files:** `src/GameHooks/GameStructs.h`, `src/Launcher/Launcher.cpp`, `src/ModDLL/ModMain.cpp`

**Changes:**
- Obtain Frostpunk 1.6.2 executable, compute SHA256
- Update `EXPECTED_SHA256` in Launcher.cpp with 1.6.2 hash (keep 1.6.1 as fallback)
- Add `GameVersion` enum: `Unknown, V1_6_1, V1_6_2`
- Runtime version detection in `GameReader::Initialize()` via vtable validation
- If version unknown: log error, return `false` from Initialize, mod stays inactive
- Document all verified 1.6.2 RVAs in GameStructs.h (separate namespace or versioned)

**Exit Criteria:**
- [ ] Launcher verifies 1.6.2 SHA256 (and 1.6.1 for compat)
- [ ] Mod Initialize() fails fast on unknown version with clear log message
- [ ] All RVAs in GameStructs.h marked with version verified status

### 1.4 Fix Error Handling in ModMain::Initialize
**Files:** `src/ModDLL/ModMain.cpp`

**Changes:**
- Track each subsystem init result: `bool hooksOk, overlayOk, networkOk, readerOk, writerOk`
- On any failure: rollback already-initialized subsystems (reverse order)
- Return `false` if any critical subsystem fails (hooks, reader, writer)
- Network failure → continue in "offline mode" but `initialized_` reflects partial state
- Add `GetInitializationStatus()` returning detailed status struct

**Exit Criteria:**
- [ ] Initialize() returns `false` if hooks/reader/writer fail
- [ ] Network failure → mod initializes but `IsOnline()` returns false
- [ ] No false `initialized_ = true` on partial failure
- [ ] Clear log messages for each failure reason

### 1.5 Verify Stable Loading
**Test Procedure:**
1. Launch Frostpunk via launcher (--new-instance)
2. Verify DLL injected, `FrostMP.log` created
3. Reach main menu, wait 30 minutes
4. No Access Violations, no crashes, no memory growth
5. Repeat 5x consecutively

**Exit Criteria (per task Section 6):**
- [ ] Game launches without mod (baseline)
- [ ] Game launches with DLL injected
- [ ] Game runs ≥ 30 minutes with DLL
- [ ] No Access Violation in logs
- [ ] DLL logs load confirmation
- [ ] Initialization errors displayed clearly
- [ ] 5 consecutive successful launches

---

## Stage 2: Safe Game Hooks

**Goal:** Prove safe interaction with Frostpunk 1.6.2 — read time, speed, pause; write speed, pause.

### 2.1 Research & Verify 1.6.2 Offsets
**Method:** Pattern scanning + static analysis on 1.6.2 binary
**Targets:** All RVAs in GameStructs.h
**Deliverable:** Updated GameStructs.h with verified 1.6.2 offsets

### 2.2 Implement & Test Core Hooks (One at a Time)
| Hook | Function | Test |
|------|----------|------|
| GetGameTime | GameReader::GetGameTimeMs | Matches in-game clock |
| GetGameSpeed | GameReader::GetGameSpeed | 0/1/2 matches UI |
| SetPaused | GameWriter::SetPaused | Pause/unpause works |
| SetGameSpeed | GameWriter::SetGameSpeed | 1x/3x/12x works |

**Per-Hook Process:**
1. Install hook via new HookManager
2. Verify trampoline executes original correctly
3. Verify callback intercepts correctly
4. Verify DisableHook restores original behavior
5. Log all results

**Exit Criteria:**
- [ ] Reading time matches game UI continuously
- [ ] Reading speed matches game UI (1x/3x/12x)
- [ ] Pause toggle works (hook blocks client, allows host)
- [ ] Speed change works (hook blocks client, allows host)
- [ ] Game stable 30+ minutes with hooks active
- [ ] After UnhookGameFunctions: original behavior fully restored

---

## Stage 3: Steam P2P Connection

**Goal:** Two real computers connect via Steam P2P, exchange messages 30+ minutes.

### 3.1 Stabilize SteamNetworkingSocketsImpl
- Remove `SteamDatagramClient_Init` fallback (use game's Steam context only)
- Validate vtable indices against actual loaded DLL
- Add proper error codes for each failure mode
- Timeout handling for all blocking calls

### 3.2 Implement Connection Test Harness
- Standalone test app (or launcher mode) that:
  - Host: `StartListening()`, waits for connection
  - Client: `Connect(hostSteamID)`, sends Hello
  - Both: Exchange heartbeats, measure latency
  - Auto-reconnect on disconnect

### 3.3 Two-Machine Test
**Requirements:** 2 Steam accounts, 2 computers, both own Frostpunk
**Test Plan:**
1. Host launches, creates session
2. Client launches, enters host SteamID64, connects
3. Verify: Connected state, heartbeats, latency < 200ms typical
4. Run 30 minutes, verify no disconnects
5. Force disconnect (unplug network), verify detection
6. Reconnect, verify session recovery

**Exit Criteria:**
- [ ] Two real computers connect via Steam P2P
- [ ] Messages pass both directions reliably
- [ ] Heartbeats maintain connection
- [ ] Disconnect detected within 15s
- [ ] Reconnect works without restart
- [ ] 30-minute stable session

---

## Stage 4: Game Session System

**Goal:** Two players in one session, synchronized start.

### 4.1 Implement SessionManager
- Lobby state (waiting for client)
- Settings negotiation (map, scenario, difficulty, seed)
- Ready/NotReady for both players
- Host-authoritative start sequence

### 4.2 Integration with NetworkManager
- Hello/HelloAck carries session info
- GameState carries settings (mapIndex, scenarioType, difficulty, seed)
- GameStart payload triggers simultaneous start

### 4.3 Exit Criteria
- [ ] Two computers see same session
- [ ] Both confirm Ready
- [ ] Start blocked if settings mismatch
- [ ] Start blocked if version mismatch
- [ ] Both enter game simultaneously

---

## Stage 5: Time Synchronization

**Goal:** Two independent cities share timebase — pause, speed, game time synced.

### 5.1 Implement TimeSync Class
- Host broadcasts TimeSync every 100ms (with GameStateDelta)
- Client measures RTT via Heartbeat, computes offset
- Client applies smooth speed adjustment (±10%) to converge
- Desync detection: |offset| > 5s → request full resync

### 5.2 Hook Integration
- Host SetSpeed/SetPaused → broadcast immediately
- Client receives → applies via GameWriter (hooks allow network-induced changes)
- Network pause token prevents client manual pause

### 5.3 Exit Criteria
- [ ] Both cities start at same game time
- [ ] Speed change by host → client matches within 200ms
- [ ] Pause by host → client pauses within 200ms
- [ ] No accumulating time drift over 30 minutes
- [ ] Network latency spikes don't cause uncontrolled desync
- [ ] Client pauses simulation on disconnect (no runaway)

---

## Stage 6: Resource Trading

**Goal:** Verified, safe resource transfers between cities.

### 6.1 Implement ResourceTransferManager
- Request/confirm/deny flow
- Deduplication by transferID
- Pre-validation: sender has resources, amount > 0
- Atomic: both sides updated or neither

### 6.2 Supported Resources (6)
- Coal, Wood, Steel, Steam Cores, Raw Food, Food Rations

### 6.3 Exit Criteria
- [ ] Sender resources deducted
- [ ] Receiver resources added
- [ ] Negative balance prevented
- [ ] Duplicate packet rejected
- [ ] Deny leaves balances unchanged
- [ ] Disconnect during trade handled safely

---

## Stage 7: Minimal Overlay UI

**Goal:** Functional overlay showing status, trade, chat.

### 7.1 Upgrade to Direct2D
- Hardware-accelerated, no flicker
- Proper D3D11 interop (game uses D3D11)

### 7.2 UI Panels
- Connection status (host/client, peer name, SteamID)
- Game time, speed, pause
- Resources (6 types)
- Trade: send request, accept/decline incoming
- Chat: send/receive messages
- Save/Load (host only)

### 7.3 Hotkeys (Configurable, Non-Conflicting)
- Default: `Ctrl+Shift+M` = Main menu, `Ctrl+Shift+T` = Trade, `Ctrl+Shift+C` = Chat

### 7.4 Exit Criteria
- [ ] Overlay renders without flicker
- [ ] All status info accurate
- [ ] Trade flow works end-to-end
- [ ] Chat works
- [ ] No game input interference

---

## Stage 8: Final Integration Testing

**Goal:** Complete MVP verified on two machines.

### 8.1 Full Test Scenario (60+ minutes)
1. Host creates session, selects Endless map
2. Client connects, both Ready
3. Game starts simultaneously
4. Play 60 minutes: pause, speed changes, resource trades
5. Client disconnects, reconnects
6. Host saves, both load
7. Clean exit, restart new session

### 8.2 Exit Criteria (Definition of Done per Section 10)
- [ ] Two players on different computers connect via Steam P2P
- [ ] Either can create session
- [ ] Both launch Endless Mode
- [ ] Independent cities
- [ ] Time synchronized (pause, speed, game time)
- [ ] Resource trade works with confirmation
- [ ] Session status displayed
- [ ] Disconnect handled
- [ ] No crashes with mod loaded
- [ ] Install/run instructions documented
- [ ] All functions verified on two computers

---

## Timeline Estimate

| Stage | Effort | Dependencies |
|-------|--------|--------------|
| 0. Audit | ✅ Done | — |
| 1. Stable Loading | 3-5 days | 0 |
| 2. Safe Hooks | 2-3 days | 1 |
| 3. Steam P2P | 2-3 days | 1,2 |
| 4. Session System | 1-2 days | 3 |
| 5. Time Sync | 2-3 days | 2,4 |
| 6. Resource Trade | 1-2 days | 4,5 |
| 7. Overlay UI | 2-3 days | 4 |
| 8. Final Testing | 2-3 days | 5,6,7 |
| **Total** | **15-24 days** | Sequential |

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| 1.6.2 offsets significantly different | Build pattern scanner; allocate extra time in Stage 2 |
| HookManager rewrite unstable | Prototype on simple function first; fallback to MinHook if needed |
| Steam P2P fails on some NATs | Test early (Stage 3); document relay requirements |
| Time sync impossible without entity sync | Design allows independent cities — only sync pause/speed/time |
| Overlay crashes game | Direct2D with D3D11On12; fallback to GDI mode |

---

## Current Blockers (Must Resolve Before Stage 2)

1. **HookManager rewrite** — Core enabler for all game interaction
2. **DllMain fix** — Prerequisite for stable loading
3. **1.6.2 binary access** — Need executable to verify offsets
4. **Two Steam accounts + machines** — Required for Stages 3, 5, 8

---

## Next Actions (Immediate)

1. [ ] Obtain Frostpunk 1.6.2 executable (Steam update or user provides)
2. [ ] Begin HookManager rewrite (Stage 1.2)
3. [ ] Fix DllMain deferred initialization (Stage 1.1)
4. [ ] Update Launcher SHA256 for 1.6.2 (Stage 1.3)
5. [ ] Fix ModMain error handling (Stage 1.4)

---

## Notes

- **Do not parallelize stages** — Each stage's exit criteria must be met before next begins
- **Test on real hardware** — VMs/local loopback insufficient for Steam P2P
- **Document everything** — Update CURRENT_STATE.md, BUGS.md, TEST_RESULTS.md after each stage
- **Commit frequently** — Git commit after each subsystem fix