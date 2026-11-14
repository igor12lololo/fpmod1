<#
.SYNOPSIS
Packages the Frostpunk Multiplayer mod for distribution
#>

param(
    [string]$Version = "1.0.0",
    [switch]$Sign
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$outDir = Join-Path $scriptDir "Release"
$packageDir = Join-Path $scriptDir "FrostpunkMultiplayer_v$Version"
$releaseDir = Join-Path $scriptDir "Release_Package"

if (Test-Path $packageDir) { Remove-Item $packageDir -Recurse -Force }
if (Test-Path $releaseDir) { Remove-Item $releaseDir -Recurse -Force }

New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

# Files to include
$files = @(
    "FrostpunkMultiplayerLauncher.exe",
    "FrostMenuMod.dll",
    "README.txt",
    "INSTALL.txt",
    "CHANGELOG.txt",
    "LICENSE.txt"
)

Write-Host "Copying release files..." -ForegroundColor Cyan
foreach ($file in $files) {
    $src = Join-Path $outDir $file
    $dst = Join-Path $packageDir $file
    if (Test-Path $src) {
        Copy-Item $src -Destination $dst -Force
        Write-Host "  Copied $file" -ForegroundColor Gray
    } else {
        Write-Warning "Missing file: $file"
    }
}

# Create README.txt
$readme = @"
🚨 FROSTPUNK MULTIPLAYER — STEAM P2P COMMUNITY TEST 🚨
Version: $Version

Turn Frostpunk into a shared survival experience: every player controls an
independent city, while the host controls the map, difficulty and start.

THIS IS A TEST BUILD

Official source
---------------
https://github.com/your-repo/FrostpunkMultiplayer

Requirements
------------
- Windows 10/11 64-bit.
- Frostpunk 1.6.1 or 1.6.2 (Steam version).
- Steam installed and online.
- One different Steam account per player.
- Every player owns and installs Frostpunk separately.
- Frostpunk must run in WINDOWED mode. The custom TRADE / CHAT/SAVE overlay
  does not work in Fullscreen mode. Set Windowed before connecting.

Install
-------
1. Extract this entire folder. Keep the EXE and DLL files together.
2. Do not copy these files into the Frostpunk folder.
3. Run FrostpunkMultiplayerLauncher.exe. It starts Frostpunk and loads the mod.
   If Frostpunk is already open, it attaches to that process.
4. In the game's display settings select Windowed, not Fullscreen. Restart
   Frostpunk if the display-mode change does not take effect immediately.

Start a Steam P2P game
----------------------
1. Both players open Multiplayer in the Frostpunk main menu and select Steam.
2. Each player enters the other player's 17-digit SteamID64.
3. Both click Connect. The first player to click Connect becomes the host.
4. The host chooses Endless or Story Scenario, selects the map and difficulty,
   then starts the game.
5. The client receives the same map and difficulty and starts automatically.
6. Each player controls their own city. Open TRADE for resource transfers and
   CHAT/SAVE for chat and synchronized saves.

Saving and continuing
---------------------
The host types a save name such as survival. The multiplayer save is stored as
survival_multiplayer. The same name is used for every connected player. To
continue later, start both games, connect again and load the matching
multiplayer save through CHAT/SAVE.

Current limits
--------------
- Two players are supported in this community build.
- Frostpunk 1.6.1 and 1.6.2 are covered by the compatibility scanner.
- The ZIP does not include Frostpunk game files. It includes a Valve-signed
  Steamworks redistributable used by the network bridge.
- The current build is for testing with friends; expect bugs and report them.

Troubleshooting
---------------
- If compatibility scanning fails, send FrostMenuMod.log to the developer; it
  names the engine signature that could not be resolved safely.
- If Steam P2P cannot connect, confirm both accounts are online and that the
  two SteamID64 values were entered correctly.
- Restart both game processes after replacing the mod files.
- If TRADE or CHAT/SAVE is hidden or unresponsive, switch Frostpunk to
  Windowed mode. Fullscreen is not supported by the custom overlay.
- Do not disable Windows Defender or add a broad antivirus exclusion. If a file
  is blocked, report the exact detection name and its SHA-256 on the project's
  GitHub issue tracker so the build can be reviewed and submitted as a possible
  false positive.

Controls
--------
- F1: Toggle Chat UI
- F2: Toggle Trade UI
- F3: Toggle Save UI
- F10: Show/Hide Overlay
- Enter in chat: Send message
- Type /help for commands

Commands
--------
- /pause - Toggle pause (host only)
- /speed 0|1|2 - Set game speed (host only)
- /trade <resource> <amount> - Offer trade
- /save <name> - Save game
- /load <name> - Load game
- /help - Show this help

Resources:
  coal, wood, steel, cores, food, rations

Example: /trade coal 100
"@
$readme | Set-Content -Path (Join-Path $packageDir "README.txt") -Encoding UTF8

# Create INSTALL.txt
$install = @"
INSTALLATION INSTRUCTIONS
=========================

1. EXTRACT
   - Extract the ZIP file to any folder (e.g., Documents\FrostpunkMultiplayer)
   - Keep all files together in the same folder

2. VERIFY FILES
   You should have:
   - FrostpunkMultiplayerLauncher.exe
   - FrostMenuMod.dll
   - README.txt
   - INSTALL.txt
   - CHANGELOG.txt
   - LICENSE.txt

3. RUN
   - Double-click FrostpunkMultiplayerLauncher.exe
   - If Frostpunk is not running, it will start automatically
   - If Frostpunk is already running, it will attach to the existing process

4. CONFIGURE
   - On first run, you may be prompted to locate Frostpunk.exe
   - Select your Frostpunk installation (usually in Steam folder)
   - In Frostpunk settings, set Display Mode to WINDOWED (not Fullscreen!)

5. PLAY
   - In Frostpunk main menu, click "Multiplayer" -> "Steam"
   - Enter your friend's SteamID64 (17 digits, right-click profile -> Copy SteamID64)
   - Both players click Connect
   - Host selects map and starts game
   - Enjoy!

UNINSTALL
=========
- Simply delete the extracted folder
- No files are modified in your Frostpunk installation
- Your original saves are not affected

TROUBLESHOOTING
===============
Game won't start:
- Verify Frostpunk is installed via Steam
- Run launcher as Administrator
- Check antivirus isn't blocking the DLL

Can't connect:
- Both players must be online on Steam
- Verify SteamID64 is correct (17 digits, starts with 7656119...)
- Try restarting Steam

Overlay not visible:
- MUST use Windowed mode in Frostpunk settings
- Press F10 to toggle overlay
- Try Alt+Tab and back

Crashes on launch:
- Verify game files in Steam
- Delete FrostMenuMod.log and retry
- Report crash with log file
"@
$install | Set-Content -Path (Join-Path $packageDir "INSTALL.txt") -Encoding UTF8

# Create CHANGELOG.txt
$changelog = @"
CHANGELOG
=========

v$Version (Initial Release)
---------------------------
- Initial Steam P2P multiplayer implementation
- Host/Client architecture with authoritative host
- Game state synchronization (resources, time, speed, pause)
- Resource trading between players
- Chat system with overlay
- Synchronized save/load
- Map/scenario selection sync
- Steam Networking Sockets integration (no port forwarding)
- Windowed mode overlay UI
- Compatibility with Frostpunk 1.6.1 and 1.6.2
- Signature-based engine compatibility (no file hashing)

Known Issues
------------
- Only 2 players supported
- Fullscreen mode not supported for overlay
- Some UI elements may not sync perfectly
- Occasional desync on poor connections
- Save/load requires both players to have matching saves
- No voice chat integration
- No spectator mode

Planned Features
----------------
- 4+ player support
- Improved desync detection and recovery
- Better overlay with Direct2D rendering
- Spectator mode
- Voice chat via Steam
- Mod configuration UI
- Dedicated server support
"@
$changelog | Set-Content -Path (Join-Path $packageDir "CHANGELOG.txt") -Encoding UTF8

# Create LICENSE.txt
$license = @"
Frostpunk Multiplayer Mod
Copyright (C) 2026 FrostMP Team

This mod is provided for educational and research purposes.
Frostpunk is a trademark of 11 bit studios.
Steam and Steamworks are trademarks of Valve Corporation.

This project is not affiliated with, endorsed by, or sponsored by
11 bit studios or Valve Corporation.

Use at your own risk. The authors are not responsible for any
damage to your game installation, saves, or Steam account.

Redistribution of this mod in binary form is permitted provided:
1. The original archive is distributed unmodified
2. No fee is charged for the mod itself
3. Credit is given to the FrostMP Team

Source code modifications must be shared under the same terms.
"@
$license | Set-Content -Path (Join-Path $packageDir "LICENSE.txt") -Encoding UTF8

# Create SHA256SUMS.txt
Write-Host "Generating checksums..." -ForegroundColor Cyan
$checksums = @()
foreach ($file in Get-ChildItem $packageDir -File) {
    $hash = Get-FileHash -Algorithm SHA256 -Path $file.FullName
    $checksums += "$($hash.Hash)  $($file.Name)"
}
$checksums -join "`n" | Set-Content -Path (Join-Path $packageDir "SHA256SUMS.txt") -Encoding ASCII

# Create ZIP
Write-Host "Creating release ZIP..." -ForegroundColor Cyan
$zipPath = Join-Path $releaseDir "FrostpunkMultiplayer_v$Version.zip"
Compress-Archive -Path $packageDir\* -DestinationPath $zipPath -Force

Write-Host "Package created: $zipPath" -ForegroundColor Green
Write-Host "Release folder: $releaseDir" -ForegroundColor Green

# Optional signing
if ($Sign) {
    Write-Host "Signing not implemented - add your code signing certificate here" -ForegroundColor Yellow
}