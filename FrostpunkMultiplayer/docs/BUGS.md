# Frostpunk Multiplayer — Known Bugs & Defects

**Date:** 2026-11-13
**Status:** Living document — updated as issues are found/fixed

---

## Critical (Blockers)

### BUG-001: HookManager callback crashes on execution
**Severity:** CRITICAL
**Component:** `src/GameHooks/HookManager.cpp` (InstallCallbackHook)
**Description:** Detour assembly stores pointer to `std::function` object as code address, then executes `call rax`. The CPU attempts to execute the `std::function` object's vtable/control block as machine code → immediate crash (Access Violation / Illegal Instruction).
**Evidence:** Lines 69, 109, 118 in HookManager.cpp; `callbackPtr` (heap-allocated `HookCallback*`) written into detour as call target.
**Impact:** All 12 callback hooks (SetSpeed, ChangeResource, PauseAddReason, etc.) will crash when triggered.
**Fix Required:** Rewrite detour to call a static trampoline function that looks up callback from map, or use a fixed stub per hook.

### BUG-002: HookManager trampoline doesn't relocate relative instructions
**Severity:** CRITICAL
**Component:** `src/GameHooks/HookManager.cpp` (InstallCallbackHook, lines 61-66)
**Description:** Original prologue bytes copied verbatim to trampoline. Any `CALL rel32`, `JMP rel32`, `Jcc rel32`, or RIP-relative memory access (`[rip+offset]`) will use trampoline's RIP, not original function's RIP.
**Impact:** Hooked functions with relative control flow or RIP-relative data access will misbehave/crash when trampoline executes.
**Fix Required:** Integrate disassembler (Zydis, Capstone, or minimal x64 decoder) to identify and rewrite relative instructions in trampoline.

### BUG-003: HookManager no memory protection / instruction cache handling
**Severity:** CRITICAL
**Component:** `src/GameHooks/HookManager.cpp` (EnableHook, DisableHook)
**Description:** `WriteLocalRaw` uses `__try/__except` memcpy without `VirtualProtect` to make target writable, and no `FlushInstructionCache` after write.
**Impact:** On systems with DEP/NX or multi-core, writes may fail silently or execute stale instructions from I-cache.
**Fix Required:** `VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old)`, write, `VirtualProtect(target, size, old, &old)`, `FlushInstructionCache(GetCurrentProcess(), target, size)`.

### BUG-004: HookManager no thread synchronization during install/remove
**Severity:** CRITICAL
**Component:** `src/GameHooks/HookManager.cpp`
**Description:** Other threads may be mid-execution in target function while prologue bytes are overwritten. No thread suspension or safe-point mechanism.
**Impact:** Race condition: thread fetches half-old half-new instruction → crash or undefined behavior.
**Fix Required:** Enumerate process threads, `SuspendThread` each (except caller), install hook, `ResumeThread`. Or use `WaitForThreadpoolWorkCallbacks` + memory barriers.

### BUG-005: HookManager JMP size vs prologueSize mismatch
**Severity:** HIGH
**Component:** `src/GameHooks/HookManager.cpp` (InstallHook vs InstallCallbackHook)
**Description:** `InstallHook` default `hookSize=14` (14-byte JMP). `InstallCallbackHook` default `prologueSize=5`. If original function prologue is 6-13 bytes, 14-byte JMP overwrites into next instruction; if prologue >14, trampoline jumps into middle of overwritten instruction.
**Impact:** Corrupted code flow, crashes, silent corruption.
**Fix Required:** Disassemble to find instruction boundaries, use minimum prologue size that covers complete instructions (min 5 for short JMP, 14 for long JMP).

### BUG-006: HookManager callback memory management defects
**Severity:** HIGH
**Component:** `src/GameHooks/HookManager.cpp` (callbackPtrs_, callbacks_, RemoveHook, RemoveAllHooks)
**Description:** 
- `callbackPtrs_` stores `new HookCallback` — `RemoveHook` deletes, `RemoveAllHooks` also deletes → double-free if both called
- `callbacks_` stores *copy* of `std::function` — if callback captures `this` (ModMain), dangling reference after shutdown
**Impact:** Heap corruption on shutdown, use-after-free in callbacks.
**Fix Required:** Single ownership model, weak_ptr for callbacks, or explicit lifecycle management.

### BUG-007: DllMain performs heavy initialization under loader lock
**Severity:** CRITICAL
**Component:** `src/ModDLL/DllMain.cpp` (lines 155-158)
**Description:** `TryAutoInitialize` → `InitializeMod` → constructs full mod (hooks, network, overlay, threads) inside `DLL_PROCESS_ATTACH`.
**Impact:** Deadlocks on loader lock (LoadLibrary/GetProcAddress), crashes in thread creation, violates Windows DLL best practices.
**Fix Required:** DllMain only sets a flag/event; background thread (created via `CreateThread` with `CREATE_SUSPENDED` or fiber) performs actual initialization.

### BUG-008: No version verification for Frostpunk 1.6.2
**Severity:** CRITICAL
**Component:** `src/GameHooks/GameStructs.h`, `src/Launcher/Launcher.cpp`
**Description:** All offsets/RVAs hardcoded for 1.6.1 (SHA: 719c6e...). Launcher validates against 1.6.1 hash only. Target is 1.6.2.
**Impact:** Mod will hook wrong addresses on 1.6.2 → crashes, memory corruption, silent wrong behavior.
**Fix Required:** 
1. Obtain 1.6.2 executable, verify SHA256
2. Re-verify all offsets via pattern scanning or updated RE
3. Implement runtime version detection with graceful failure

### BUG-009: Initialize() returns true regardless of subsystem failures
**Severity:** CRITICAL
**Component:** `src/ModDLL/ModMain.cpp` (lines 158-161)
**Description:** `initialized_ = true; return true;` executed even if hooks fail, overlay fails, network fails.
**Impact:** Caller (launcher, auto-init) believes mod is fully functional; subsequent operations crash or no-op silently.
**Fix Required:** Track each subsystem init result, rollback on failure, return aggregate status.

---

## High (Major Functionality)

### BUG-010: SteamNetworking vtable indices hardcoded
**Severity:** HIGH
**Component:** `src/Network/SteamNetworking.cpp` (lines 200-233)
**Description:** VTable indices (3=CreateListenSocketP2P, 4=CreateP2PConnectionSocket, etc.) assumed from specific Steam SDK version. May differ in game's loaded `steamnetworkingsockets.dll`.
**Impact:** Function pointers point to wrong vtable slots → crashes on network operations.
**Fix Required:** Validate vtable via known method signatures, or use Steam's official headers/SDK.

### BUG-011: SteamDatagramClient_Init may hang indefinitely
**Severity:** HIGH
**Component:** `src/Network/SteamNetworking.cpp` (lines 139-156)
**Description:** `SteamDatagramClient_Init()` called via `std::async` with 3s timeout, but no mechanism to cancel the async task if timeout expires. Task continues in background.
**Impact:** Resource leak, potential deadlock on subsequent init attempts.
**Fix Required:** Use proper cancellation token, or avoid DatagramClient_Init entirely (use game's existing Steam context).

### BUG-012: NetworkManager SendToAll only sends to single client
**Severity:** HIGH
**Component:** `src/Network/NetworkManager.cpp` (lines 168-176)
**Description:** `SendToAll` implementation assumes single client (`if (role == Role::Host && clientSteamID)`). No support for multiple clients.
**Impact:** MVP requires 2 players only, but architecture doesn't match method name/contract.
**Fix Required:** Rename to `SendToClient` or implement peer list for future expansion.

### BUG-013: GameReader/GameWriter offsets unverified for 1.6.2
**Severity:** HIGH
**Component:** `src/GameHooks/GameStructs.h`, `src/GameHooks/GameReader.cpp`, `src/GameHooks/GameWriter.cpp`
**Description:** All RVAs and structure offsets from 1.6.1 reverse engineering. No validation they match 1.6.2.
**Impact:** Read/write wrong memory → crashes, corruption, desync.
**Fix Required:** Verify each offset against 1.6.2 binary; add runtime signature validation.

### BUG-014: Resource transfer lacks idempotency / duplicate protection
**Severity:** HIGH
**Component:** `src/ModDLL/ModMain.cpp` (OnResourceTransferReceived, lines 659-677)
**Description:** Transfer processed by checking `writer_->ChangeResource`. No deduplication by `transferID`. Retransmitted packet = double transfer.
**Impact:** Network retry → resource duplication/loss.
**Fix Required:** Track processed `transferID`s, reject duplicates.

### BUG-015: No negative balance protection in resource transfer
**Severity:** HIGH
**Component:** `src/ModDLL/ModMain.cpp` (OnResourceTransferReceived, line 664)
**Description:** `ChangeResource` called with `transfer->amount` (can be negative). No pre-check that sender has sufficient resources.
**Impact:** Sender resources can go negative.
**Fix Required:** Read current amount, validate `current + delta >= 0` before calling ChangeResource.

---

## Medium (Maintainability / Correctness)

### BUG-016: Overlay GDI rendering flickers / no double buffering
**Severity:** MEDIUM
**Component:** `src/Overlay/OverlayUI.cpp` (Render, lines 189-298)
**Description:** Direct drawing to window DC with `FillRect` + `TextOutA` each frame. No backbuffer.
**Impact:** Visible flicker, poor performance at high FPS.
**Fix Required:** Use memory DC (CreateCompatibleDC) + BitBlt, or migrate to Direct2D.

### BUG-017: Overlay shared memory name includes PID only
**Severity:** MEDIUM
**Component:** `src/Overlay/OverlayUI.cpp` (CreateSharedMemory, line 81)
**Description:** `FrostMP_Overlay_%llu` with PID. If multiple mod instances in same process (reload), name collision.
**Impact:** Second instance fails to create mapping, or attaches to stale control block.
**Fix Required:** Include DLL base address or random component in name.

### BUG-018: Packet sequence numbers not validated for gaps
**Severity:** MEDIUM
**Component:** `src/Network/NetworkManager.cpp` (HandleIncomingPackets, line 137)
**Description:** `remoteSequence = max(remoteSequence, header->sequence + 1)` — no gap detection, no reordering buffer.
**Impact:** Lost packets silently ignored; out-of-order packets applied in wrong order.
**Fix Required:** Implement sequence window with reordering buffer, gap detection + NACK/retransmit request.

### BUG-019: Heartbeat timeout too aggressive for high latency
**Severity:** MEDIUM
**Component:** `src/Network/NetworkManager.cpp` (Poll, line 111)
**Description:** 15-second timeout (`now - lastHeartbeatReceived > 15000`). Heartbeats every 5s. 3 missed heartbeats = disconnect.
**Impact:** Legitimate disconnect on >500ms RTT with packet loss.
**Fix Required:** Configurable timeout, adaptive based on measured RTT.

### BUG-020: ModMain Update() calls network Poll() every frame but reader init deferred
**Severity:** MEDIUM
**Component:** `src/ModDLL/ModMain.cpp` (Update, lines 247-262)
**Description:** Reader initialization polls global pointers every frame until valid. No timeout, no backoff.
**Impact:** CPU spin if game never loads valid pointers (e.g., wrong version).
**Fix Required:** Add retry limit, exponential backoff, version check before attempting.

---

## Low (Polish / Nice to Have)

### BUG-021: Logger not thread-safe for concurrent file writes
**Severity:** LOW
**Component:** `src/Common/Logger.h` (Log method)
**Description:** `std::lock_guard` protects file write, but `OutputDebugStringA` called outside lock. Interleaved debug output possible.
**Impact:** Garbled debug output in debugger.
**Fix Required:** Move OutputDebugString inside lock, or use thread-local buffer.

### BUG-022: Hardcoded hotkeys (F1-F3, F10) conflict with game
**Severity:** LOW
**Component:** `src/Overlay/OverlayUI.cpp` (WndProc, lines 349-366)
**Description:** Task explicitly says "Не привязывай интерфейс к F10. Этот хоткей не использовать." — but F10 is used.
**Impact:** Violates spec, may conflict with game bindings.
**Fix Required:** Change to non-conflicting keys (e.g., Ctrl+Shift+M for overlay, Ctrl+Shift+T for trade).

### BUG-023: Chat command parsing fragile (no quoted strings)
**Severity:** LOW
**Component:** `src/ModDLL/ModMain.cpp` (HandleChatCommand, lines 517-621)
**Description:** `std::istringstream` splits on whitespace. Save names with spaces break.
**Impact:** `/save My Save Name` → saves as "My".
**Fix Required:** Proper argument parsing with quote support.

### BUG-024: No automated tests for hook/network/game logic
**Severity:** LOW
**Component:** `src/Test/TestMod.cpp`
**Description:** Only tests protocol serialization and structs. No integration tests.
**Impact:** Regressions undetected.
**Fix Required:** Add unit tests for HookManager (mock), NetworkManager (loopback), GameReader/Writer (against known memory patterns).

---

## Fixed / Verified

*None yet — tracking starts now.*

---

## Bug Tracking Index

| ID | Severity | Component | Status |
|----|----------|-----------|--------|
| BUG-001 | CRITICAL | HookManager | Open |
| BUG-002 | CRITICAL | HookManager | Open |
| BUG-003 | CRITICAL | HookManager | Open |
| BUG-004 | CRITICAL | HookManager | Open |
| BUG-005 | HIGH | HookManager | Open |
| BUG-006 | HIGH | HookManager | Open |
| BUG-007 | CRITICAL | DllMain | Open |
| BUG-008 | CRITICAL | Version Check | Open |
| BUG-009 | CRITICAL | ModMain::Initialize | Open |
| BUG-010 | HIGH | SteamNetworking | Open |
| BUG-011 | HIGH | SteamNetworking | Open |
| BUG-012 | HIGH | NetworkManager | Open |
| BUG-013 | HIGH | GameReader/Writer | Open |
| BUG-014 | HIGH | Resource Transfer | Open |
| BUG-015 | HIGH | Resource Transfer | Open |
| BUG-016 | MEDIUM | Overlay | Open |
| BUG-017 | MEDIUM | Overlay | Open |
| BUG-018 | MEDIUM | NetworkProtocol | Open |
| BUG-019 | MEDIUM | NetworkManager | Open |
| BUG-020 | MEDIUM | ModMain | Open |
| BUG-021 | LOW | Logger | Open |
| BUG-022 | LOW | Overlay (Hotkeys) | Open |
| BUG-023 | LOW | Chat Commands | Open |
| BUG-024 | LOW | Testing | Open |