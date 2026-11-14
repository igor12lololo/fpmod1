# Frostpunk Multiplayer Mod - Architecture Documentation

## Overview

This document describes the architecture and implementation of the Frostpunk Multiplayer Mod, a Steam P2P multiplayer modification for Frostpunk (11 bit studios) that enables two players to play together cooperatively over the internet.

## Game Analysis

### Engine
- **Engine**: Liquid Engine (proprietary, developed by 11 bit studios)
- **Version**: Frostpunk 1.6.1 / 1.6.2
- **Platform**: Windows x64
- **Executable**: Frostpunk.exe (48.8 MB)
- **SHA-256 (1.6.1)**: `719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4`

### Key Findings from Reverse Engineering
- Custom C++ engine with no standard scripting (no Lua, Mono, IL2CPP)
- Game state managed through global pointers to manager objects
- Resources identified by telemetry keys (stable across sessions)
- Time system uses calendar ticks with hour-scale conversion
- Menu system uses UI controller pattern with virtual function tables
- Steamworks already integrated (steam_api64.dll present)

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Frostpunk Process                           │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────────────┐  │
│  │ FrostMenuMod│  │  Game Hooks  │  │   Overlay (Shared Mem) │  │
│  │    (DLL)    │──│  (Reader/    │──│   OverlayControl.h     │  │
│  │             │  │   Writer/    │  │                        │  │
│  │ ┌─────────┐ │  │   Hooks)     │  └────────────────────────┘  │
│  │ │Network  │ │  └──────────────┘              ▲                │
│  │ │Manager  │ │           ▲                    │                │
│  │ │         │ │           │                    │                │
│  │ │Steam P2P│ │           │ IPC                │                │
│  │ └────┬────┘ │           │                    │                │
│  └───────│─────┘           │                    │                │
│          │                 ▼                    │                │
│          │        ┌──────────────┐              │                │
│          └────────▶│ FrostBridge  │──────────────┘                │
│                   │   (External) │                                 │
│                   │  Steam P2P   │                                 │
│                   └──────────────┘                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Components

#### 1. FrostMenuMod.dll (Injected DLL)
Main mod entry point, injected into Frostpunk process via launcher.

**Responsibilities:**
- Game memory reading/writing (GameReader, GameWriter)
- Function hooking (HookManager)
- Network state management (NetworkManager)
- Overlay UI communication (shared memory)
- Game event interception and synchronization

#### 2. NetworkManager
Handles all network communication using Steam Networking Sockets.

**Features:**
- Steam P2P connection (no port forwarding)
- Reliable/unreliable message channels
- Packet sequencing and acknowledgment
- Heartbeat/keepalive
- Connection state machine

**Protocol:**
- Magic: `FPMF` (0x464D5046)
- Version: 1
- Packet types: Hello, GameState, PlayerAction, ResourceTransfer, Chat, Save, MapSelect, etc.

#### 3. GameReader / GameWriter
Safe memory access to Frostpunk's internal game state.

**GameReader (Read-only):**
- Time controller (game speed, pause, calendar time)
- Economy (resources: coal, wood, steel, cores, food, rations)
- Hope/Discontent systems
- Save owner (current save name)
- Loading state detection

**GameWriter (Write):**
- Set game speed (1x, 3x, 12x)
- Set pause state
- Change resources (via game's own mutation function)
- Save/Load games
- Map selection
- UI button manipulation

#### 4. HookManager
Runtime function hooking for intercepting game events.

**Hooked Functions:**
- `SetSpeed` - Game speed changes
- `ChangeResource` - Resource modifications
- `PauseAddReason` / `PauseRemoveReason` - Pause/unpause
- `NativeSave` / `NativeLoad` - Save/load operations
- `SelectMap` / `EndlessStart` - Map/game start
- Menu callbacks - UI integration

#### 5. OverlayManager
External overlay window for multiplayer UI (TRADE, CHAT/SAVE).

**Implementation:**
- Shared memory (OverlayControl struct) for IPC
- Transparent click-through window over game
- GDI rendering (can be upgraded to Direct2D)
- Input handling for chat/trade

#### 6. FrostpunkMultiplayerLauncher.exe
Standalone launcher that injects the mod.

**Features:**
- Auto-detects Frostpunk installation
- Supports `--new-instance` and `--pid` modes
- Validates game executable (64-bit check)
- Waits for kernel32.dll load before injection
- Uses CreateRemoteThread + LoadLibraryW for injection
- Verifies DLL loaded successfully

#### 7. FrostBridgeNet (External Process)
Standalone network bridge process (referenced from existing mod).

**Purpose:**
- Handles Steam Networking Sockets in separate process
- Communicates with injected DLL via IPC
- More stable than in-process networking

## Network Protocol

### Packet Structure
```
PacketHeader (32 bytes):
  u32 magic          = 0x464D5046 ("FPMF")
  u16 version        = 1
  u16 type           = PacketType enum
  u32 sequence       = Monotonically increasing
  u32 payloadSize    = Size of payload in bytes
  u64 senderSteamID  = Sender's SteamID64
  u64 timestampMs    = Unix timestamp milliseconds

Payload (variable):
  Depends on packet type
```

### Packet Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| Hello | 1 | Both | Initial connection, player info |
| HelloAck | 2 | Both | Connection acknowledged |
| Heartbeat | 3 | Both | Keepalive (every 5s) |
| Disconnect | 4 | Both | Graceful disconnect |
| GameState | 10 | Host→Client | Full authoritative state |
| GameStateDelta | 12 | Client→Host | State changes |
| PlayerAction | 20 | Both | Entity actions (move, build, etc.) |
| ResourceTransfer | 30 | Client→Host | Trade request |
| ResourceTransferResult | 31 | Host→Client | Trade result |
| TimeSync | 40 | Host→Client | Time synchronization |
| PauseState | 41 | Both | Pause/unpause |
| SpeedState | 42 | Both | Game speed change |
| ChatMessage | 50 | Both | Chat messages |
| SaveRequest | 51 | Both | Save game request |
| SaveData | 52 | Host→Client | Save file data |
| MapSelect | 60 | Host→Client | Map/scenario selection |
| GameStart | 62 | Host→Client | Start game signal |

### Synchronization Model

**Authoritative Host:**
- Host owns the simulation
- Host sends full GameState every 100ms
- Client applies state deltas
- Client sends PlayerAction/ResourceTransfer requests
- Host validates and confirms

**Interpolation:**
- Client interpolates between state updates
- Game time synchronized via calendar ticks
- Speed/pause commands acknowledged before applying

**Desync Handling:**
- Sequence numbers detect dropped packets
- Heartbeat timeout (15s) detects disconnection
- Full state resync on reconnection

## Game State Synchronization

### Resources
Identified by stable telemetry keys:
- Coal: `res_coal`
- Wood: `res_wood`
- Steel: `res_steel`
- Steam Cores: `res_cores`
- Raw Food: `res_food`
- Food Rations: `res_food_rations`

Mutation via game's own `FrostpunkEconomy::ChangeResource` function (RVA 0x168E340).

### Time
- Calendar time from GameplayTimer (offset 0xB0)
- Conversion: `ticks / (hourScale << 31) = hours`
- Hour scale at RVA 0x2B70CC0 (typically 25)
- Speed modes: 1x, 3x, 12x (float multipliers at 0x2B70CB4)

### Pause
- UserPause byte at GameplayTimer + 0xE0
- Pause reasons array at +0xB8 (count at +0xC0)
- Network uses dedicated pause token

### Hope/Discontent
- Separate systems with identical structure
- Total float + citizen count = average percentage
- Read-only (no writes implemented)

## Installation & Usage

### Requirements
- Windows 10/11 64-bit
- Frostpunk 1.6.1 or 1.6.2 (Steam)
- Steam client running and online
- Two separate Steam accounts with Frostpunk ownership

### Installation
1. Extract release ZIP to folder (e.g., `Documents\FrostpunkMultiplayer`)
2. Run `FrostpunkMultiplayerLauncher.exe`
3. Select Frostpunk.exe if prompted
4. Set Frostpunk to **Windowed** mode in display settings

### Playing
1. Both players launch via launcher
2. In Frostpunk main menu: Multiplayer → Steam
3. Enter friend's SteamID64 (17 digits, right-click Steam profile → Copy SteamID64)
4. Both click Connect (first click = host)
5. Host selects map/scenario/difficulty and starts
6. Client auto-starts with same settings

### In-Game Controls
- **F1**: Toggle Chat UI
- **F2**: Toggle Trade UI
- **F3**: Toggle Save UI
- **F10**: Show/Hide Overlay
- **Chat commands**:
  - `/pause` - Toggle pause (host)
  - `/speed 0|1|2` - Set speed (host)
  - `/trade <resource> <amount>` - Offer trade
  - `/save <name>` - Save game
  - `/load <name>` - Load game

## Building from Source

### Prerequisites
- Visual Studio 2022 with C++ workload
- Windows 10 SDK
- C++20 support

### Build Commands
```powershell
# Build everything
.\Build-All.ps1 -Configuration Release -Version 1.0.0 -Package

# Build individual components
.\Build-ModDLL.ps1 -Configuration Release
.\Build-Launcher.ps1 -Configuration Release
.\Package-Release.ps1 -Version 1.0.0
```

### Output Structure
```
Release/
├── FrostpunkMultiplayerLauncher.exe
├── FrostMenuMod.dll
└── (build intermediates)

Release_Package/
└── FrostpunkMultiplayer_v1.0.0.zip
    ├── FrostpunkMultiplayerLauncher.exe
    ├── FrostMenuMod.dll
    ├── README.txt
    ├── INSTALL.txt
    ├── CHANGELOG.txt
    ├── LICENSE.txt
    └── SHA256SUMS.txt
```

## Security Considerations

- No modification of game files on disk
- DLL injection only into own game process
- Steam Networking Sockets uses Valve-signed binaries
- No DRM/anti-cheat bypass
- No external server - pure P2P
- Open source, auditable code

## Limitations & Known Issues

### Current Limitations
- 2 players only (host + 1 client)
- Windowed mode required for overlay
- No voice chat
- No spectator mode
- Save/load requires matching saves on both clients
- Some UI elements may not sync perfectly

### Known Issues
- Occasional desync on high latency (>100ms)
- Resource amounts may drift slightly
- Overlay rendering is basic (GDI)
- Menu integration is minimal
- No reconnection after disconnect (requires restart)

## Future Improvements

1. **4+ Player Support** - Extend protocol for multiple clients
2. **Better Desync Recovery** - Checksums, automatic resync
3. **Direct2D Overlay** - Hardware-accelerated rendering
4. **Voice Chat** - Steam Voice API integration
5. **Spectator Mode** - Read-only observers
6. **Dedicated Server** - Headless host option
7. **Mod Settings UI** - In-game configuration
8. **Improved Menu Integration** - Native-looking multiplayer menu

## Credits

Based on research from:
- Existing Frostpunk Multiplayer mod (Fitchers/Frostpunk_multiplayer)
- Liquid Engine reverse engineering notes
- Steamworks Networking Sockets documentation
- Community modding resources (ModDB, NexusMods)

## License

Educational/Research use. Frostpunk © 11 bit studios. Steam/Steamworks © Valve Corporation.
Not affiliated with or endorsed by either company.