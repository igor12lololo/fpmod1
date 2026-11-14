#include "ModMain.h"
#include "Common/Logger.h"
#include "GameHooks/GameStructs.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <bcrypt.h>
#include <array>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "bcrypt.lib")

using FrostMP::TraceToFile;

namespace FrostMP::Mod {

namespace Network = FrostMP::Network;

FrostpunkMultiplayerMod* FrostpunkMultiplayerMod::instance_ = nullptr;

FrostpunkMultiplayerMod* GetModInstance() {
    return FrostpunkMultiplayerMod::instance_;
}

FrostpunkMultiplayerMod::FrostpunkMultiplayerMod()
    : mem_()
    , reader_(std::make_unique<Game::GameReader>(mem_))
    , writer_(std::make_unique<Game::GameWriter>(mem_))
    , hooks_(std::make_unique<Game::HookManager>(mem_))
    , network_(std::make_unique<Network::NetworkManager>())
    , overlay_(std::make_unique<Overlay::OverlayManager>()) {
    instance_ = this;
    // Register this hook manager instance so StaticCallback trampolines can route
    // to the correct hook callbacks. Without this, g_hookManager stays null and
    // every hook silently passes through to the original game function.
    FrostMP::Game::g_hookManager = hooks_.get();
}

FrostpunkMultiplayerMod::~FrostpunkMultiplayerMod() {
    Shutdown();
    instance_ = nullptr;
}

bool FrostpunkMultiplayerMod::Initialize(u64 localSteamID, Network::NetworkManager::Role role, u64 hostSteamID) {
    TraceToFile("FrostpunkMultiplayerMod::Initialize ENTRY");
    std::lock_guard<std::mutex> lock(initMutex_);
    if (initialized_) return true;

    localSteamID_ = localSteamID;
    role_ = role;
    hostSteamID_ = hostSteamID;

    // Reset initialization status
    initStatus_ = InitializationStatus{};

    // Setup file logging - write to a separate mod log file to avoid
    // file-sharing conflicts with the launcher's FrostMP.log.
    auto modDir = GetModuleDirectory(GetModuleHandleW(L"FrostMenuMod.dll"));
    std::filesystem::path logPath = modDir / "FrostMP_Mod.log";
    FrostMP::Logger::Instance().SetFile(logPath);
    FrostMP::Logger::Instance().SetLevel(FrostMP::LogLevel::Debug);
    FrostMP::Log::Reinit(logPath.wstring());

    DebugLog("Mod Initialize: localSteamID=%llu, role=%d, hostSteamID=%llu",
             localSteamID, (int)role, hostSteamID);

    // Lambda for rollback on failure
    auto rollback = [this](const char* reason) {
        DebugLog("ROLLBACK: %s", reason);
        if (initStatus_.network) {
            network_->Shutdown();
            initStatus_.network = false;
        }
        if (initStatus_.overlay) {
            overlay_->Shutdown();
            initStatus_.overlay = false;
        }
        if (initStatus_.hooks) {
            UnhookGameFunctions();
            initStatus_.hooks = false;
        }
        initStatus_.errorMessage = reason;
        initialized_ = false;
    };

    try {
        // 0. Verify game version (critical)
        DebugLog("STEP 0: Verifying game version...");
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        DebugLog("STEP 0: exePath=%ls", exePath);
        std::string sha256 = ComputeSHA256(exePath);
        DebugLog("STEP 0: Game executable SHA256: %s", sha256.c_str());

        initStatus_.detectedVersion = DetectGameVersion(sha256);
        initStatus_.gameVersionOk = (initStatus_.detectedVersion != GameVersion::Unknown);

        if (!initStatus_.gameVersionOk) {
            std::string msg = "Unsupported game version. Detected SHA256: " + sha256 +
                              ". Expected 1.6.1 or 1.6.2.";
            rollback(msg.c_str());
            return false;
        }
        DebugLog("STEP 0: Game version verified: %s",
                 initStatus_.detectedVersion == GameVersion::V1_6_1 ? "1.6.1" : "1.6.2");

        // 1. Install hooks FIRST - they don't need network (CRITICAL)
        DebugLog("STEP 1: Installing game hooks...");
        HookGameFunctions();
        initStatus_.hooks = hooksInstalled_;
        DebugLog("STEP 1: HookGameFunctions done, hooksInstalled_=%d", hooksInstalled_);
        if (!initStatus_.hooks) {
            rollback("Failed to install game hooks");
            return false;
        }
        DebugLog("STEP 1: Game hooks installed OK");

        // 2. Initialize overlay (important but not critical)
        DebugLog("STEP 2: Initializing overlay...");
        initStatus_.overlay = overlay_->Initialize(localSteamID_);
        DebugLog("STEP 2: Overlay init result=%d", initStatus_.overlay);
        if (!initStatus_.overlay) {
            DebugLog("WARNING: Failed to initialize overlay");
        } else {
            DebugLog("STEP 2: Overlay initialized OK");
        }

        overlay_->SetLocalSteamID(localSteamID_);
        overlay_->SetConnectionState(0); // Disconnected initially

        // NOTE: Reader/writer initialization deferred until in-game
        DebugLog("STEP 2: Reader/writer initialization deferred until in-game");
        initStatus_.reader = false;
        initStatus_.writer = false;

        // 3. Try to initialize network (optional - continue in offline mode if fails)
        DebugLog("STEP 3: Initializing network...");
        initStatus_.network = network_->Initialize(localSteamID_);
        DebugLog("STEP 3: Network init result=%d", initStatus_.network);
        bool networkRoleStarted = false;

        if (initStatus_.network) {
            DebugLog("STEP 3: Network initialized OK");

            // Register network handlers
            network_->RegisterHandler(Network::PacketType::GameState,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnGameStateReceived(sender, static_cast<const Network::GameStatePayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::GameStateDelta,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnGameStateDeltaReceived(sender, static_cast<const Network::GameStateDeltaPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::PlayerAction,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnPlayerActionReceived(sender, static_cast<const Network::PlayerActionPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::ResourceTransfer,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnResourceTransferReceived(sender, static_cast<const Network::ResourceTransferPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::ResourceTransferResult,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnResourceTransferResultReceived(sender, static_cast<const Network::ResourceTransferResultPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::ChatMessage,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnChatMessageReceived(sender, static_cast<const Network::ChatMessagePayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::SaveRequest,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnSaveRequestReceived(sender, static_cast<const Network::SaveRequestPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::LoadRequest,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnLoadRequestReceived(sender, static_cast<const Network::LoadRequestPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::LoadResponse,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnLoadResponseReceived(sender, static_cast<const Network::LoadResponsePayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::MapSelect,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnMapSelectReceived(sender, static_cast<const Network::MapSelectPayload*>(payload));
                });
            network_->RegisterHandler(Network::PacketType::GameStart,
                [this](u64 sender, const Network::PacketHeader&, const void* payload) {
                    OnGameStartReceived(sender, static_cast<const Network::GameStartPayload*>(payload));
                });
            network_->SetStateChangeHandler([this](Network::NetworkManager::State oldState, Network::NetworkManager::State newState) {
                overlay_->SetConnectionState(static_cast<int>(newState));
                if (newState == Network::NetworkManager::State::Connected) {
                    if (role_ == Network::NetworkManager::Role::Host) {
                        overlay_->SetStatusMessage("Client connected - waiting for game start");
                    } else {
                        overlay_->SetStatusMessage("Connected to host");
                    }
                } else if (newState == Network::NetworkManager::State::InGame) {
                    overlay_->SetStatusMessage("In game - multiplayer active");
                }
            });

            // Start network role
            DebugLog("STEP 3: Starting network role: %s", role_ == Network::NetworkManager::Role::Host ? "HOST" : "CLIENT");
            bool success = false;
            if (role_ == Network::NetworkManager::Role::Host) {
                success = network_->StartHost();
                DebugLog("STEP 3: StartHost returned: %s", success ? "true" : "false");
                if (success) {
                    overlay_->SetStatusMessage("Hosting game - waiting for client...");
                }
            } else if (role_ == Network::NetworkManager::Role::Client) {
                DebugLog("STEP 3: Connecting to host: %llu", hostSteamID_);
                success = network_->ConnectToHost(hostSteamID_);
                DebugLog("STEP 3: ConnectToHost returned: %s", success ? "true" : "false");
                if (success) {
                    overlay_->SetStatusMessage("Connecting to host...");
                }
            }

            if (!success) {
                DebugLog("STEP 3: Network role start failed, cleaning up network");
                network_->Shutdown();
                initStatus_.network = false;
            } else {
                networkRoleStarted = true;
            }
        } else {
            DebugLog("STEP 3: Network initialization failed, continuing without networking");
            overlay_->SetStatusMessage("Multiplayer: Offline (network init failed)");
        }

        // Final check: hooks are critical, everything else is optional
        if (!initStatus_.hooks) {
            rollback("Hook installation failed (final check)");
            return false;
        }

        // Mark as initialized even if some optional subsystems failed
        initialized_ = true;
        initStatus_.errorMessage.clear();

        DebugLog("STEP DONE: Mod initialized successfully (hooks=%s, overlay=%s, network=%s, version=%s), waiting for game session...",
                 initStatus_.hooks ? "OK" : "FAIL",
                 initStatus_.overlay ? "OK" : "FAIL",
                 initStatus_.network ? "OK" : "FAIL",
                 initStatus_.detectedVersion == GameVersion::V1_6_1 ? "1.6.1" : "1.6.2");
    } catch (const std::exception& e) {
        DebugLog("EXCEPTION in Initialize: %s", e.what());
        rollback(("Exception: " + std::string(e.what())).c_str());
        return false;
    } catch (...) {
        DebugLog("UNKNOWN EXCEPTION in Initialize");
        rollback("Unknown exception");
        return false;
    }

    return true;
}

// Helper function to compute SHA256 of a file
std::string FrostpunkMultiplayerMod::ComputeSHA256(const wchar_t* filePath) {
    // Static function - no 'this' pointer needed
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    DWORD objLen = 0, resLen = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &resLen, 0) < 0) { BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("BCryptGetProperty failed"); }
    std::vector<u8> obj(objLen);
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("BCryptCreateHash failed"); }
    ScopedHandle file(CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("Open file failed"); }
    std::vector<u8> buf(1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buf.data(), (DWORD)buf.size(), &read, nullptr)) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("Read failed"); }
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

void FrostpunkMultiplayerMod::Shutdown() {
    if (!initialized_) return;

    UnhookGameFunctions();

    network_->Disconnect("Mod shutting down");
    network_->Shutdown();

    overlay_->Shutdown();

    initialized_ = false;
    inGame_ = false;
    hooksInstalled_ = false;
}

void FrostpunkMultiplayerMod::Update() {
    if (!initialized_) return;

    char enterMsg[128];
    snprintf(enterMsg, sizeof(enterMsg), "UPDATE: entered (pid=%lu)", GetCurrentProcessId());
    TraceToFile(enterMsg);

    static int updateCounter = 0;
    if (++updateCounter % 300 == 0) { // Log every ~5 seconds
        char msg[256];
        snprintf(msg, sizeof(msg), "UPDATE: initialized=%d, inGame=%d, hooks=%d, networkState=%d, readerInit=%d",
                 initialized_, inGame_, hooksInstalled_, (int)network_->GetState(), (int)readerInitialized_);
        TraceToFile(msg);
    }

    TraceToFile("UPDATE: before network_->Poll");
    network_->Poll();
    TraceToFile("UPDATE: after network_->Poll");

    TraceToFile("UPDATE: before overlay_->Update");
    overlay_->Update();
    TraceToFile("UPDATE: after overlay_->Update");

    TraceToFile("UPDATE: before overlay_->Render");
    overlay_->Render();
    TraceToFile("UPDATE: after overlay_->Render");

    // Try to initialize reader/writer when we detect we're in a game session
    if (!readerInitialized_) {
        using namespace FrostMP::Game;
        TraceToFile("UPDATE: attempting reader/writer init");
        auto timePtr = mem_.ReadLocal<std::uintptr_t>(mem_.ResolveRVALocal(OFFSET_GLOBAL_TIME));
        auto econPtr = mem_.ReadLocal<std::uintptr_t>(mem_.ResolveRVALocal(OFFSET_GLOBAL_ECONOMY));
        if (timePtr && econPtr && *timePtr && *econPtr) {
            TraceToFile("UPDATE: game session detected, initializing reader/writer...");
            if (reader_->Initialize() && writer_->Initialize()) {
                readerInitialized_ = true;
                TraceToFile("UPDATE: reader/writer initialized successfully");
                inGame_ = reader_->IsInGame();
            } else {
                TraceToFile("UPDATE: reader/writer initialization failed, will retry");
            }
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "UPDATE: reader init skipped - timePtr=%p, econPtr=%p",
                     timePtr ? (void*)*timePtr : nullptr, econPtr ? (void*)*econPtr : nullptr);
            TraceToFile(msg);
        }
    }

    // Check if we're in game
    bool wasInGame = inGame_;
    inGame_ = readerInitialized_ && reader_->IsInGame();

    if (inGame_ && !wasInGame) {
        TraceToFile("UPDATE: just entered game");
        network_->SetStateChangeHandler([this](auto old, auto n) {
            overlay_->SetConnectionState(static_cast<int>(n));
        });
        network_->OnClientInGame();
    }

    if (inGame_) {
        TraceToFile("UPDATE: calling UpdateGameState");
        UpdateGameState();
        TraceToFile("UPDATE: UpdateGameState returned");
    }

    // Check for loading screen
    if (readerInitialized_ && reader_->IsLoading()) {
        TraceToFile("UPDATE: loading screen detected");
        inGame_ = false;
    }
}

void FrostpunkMultiplayerMod::UpdateGameState() {
    u64 now = GetTimestampMs();

    TraceToFile("UPDATE STATE: reading snapshot");
    // Read local state
    auto snapshot = reader_->GetFullSnapshot();
    if (!snapshot) {
        TraceToFile("UPDATE STATE: GetFullSnapshot returned null");
        return;
    }

    TraceToFile("UPDATE STATE: snapshot read OK");
    lastLocalSnapshot_ = *snapshot;

    // Update overlay with local state
    overlay_->SetGameSpeed(snapshot->gameSpeed);
    overlay_->SetPaused(snapshot->isPaused);
    overlay_->SetGameTime(snapshot->gameTimeMs);

    for (int i = 0; i < 6; ++i) {
        if (snapshot->resources[i] >= 0) {
            overlay_->SetResource(i, snapshot->resources[i]);
        }
    }

    // Send state updates periodically
    // Host: full state every 2 seconds, deltas every 100ms
    // Client: full state every 5 seconds, deltas every 500ms
    static const u64 FULL_STATE_INTERVAL_HOST = 2000;
    static const u64 DELTA_INTERVAL_HOST = 100;
    static const u64 FULL_STATE_INTERVAL_CLIENT = 5000;
    static const u64 DELTA_INTERVAL_CLIENT = 500;

    u64 timeSinceLastSend = now - lastStateSendTime_.load();
    bool isHost = (role_ == Network::NetworkManager::Role::Host);
    u64 fullStateInterval = isHost ? FULL_STATE_INTERVAL_HOST : FULL_STATE_INTERVAL_CLIENT;
    u64 deltaInterval = isHost ? DELTA_INTERVAL_HOST : DELTA_INTERVAL_CLIENT;

    // Track if we should send full state (every N delta intervals)
    static std::atomic<u32> deltaCounter{0};

    if (timeSinceLastSend >= deltaInterval) {
        TraceToFile("UPDATE STATE: sending state");
        if (deltaCounter.load() % (fullStateInterval / deltaInterval) == 0) {
            SendGameState();
        } else {
            SendGameStateDelta();
        }
        deltaCounter.fetch_add(1);
        lastStateSendTime_.store(now);
    }
}

void FrostpunkMultiplayerMod::SendGameState() {
    Network::GameStatePayload state{};
    state.mapIndex = lastLocalSnapshot_.mapIndex;
    state.scenarioType = lastLocalSnapshot_.scenarioType;
    state.difficulty = lastLocalSnapshot_.difficulty;
    state.randomSeed = lastLocalSnapshot_.randomSeed;
    state.gameTimeMs = lastLocalSnapshot_.gameTimeMs;
    state.isPaused = lastLocalSnapshot_.isPaused;
    state.gameSpeed = lastLocalSnapshot_.gameSpeed;
    state.hostSteamID = hostSteamID_;
    state.clientSteamID = clientSteamID_;

    for (int i = 0; i < 6; ++i) {
        switch (static_cast<Game::ResourceID>(i)) {
            case Game::ResourceID::Coal: state.coal = lastLocalSnapshot_.resources[i]; break;
            case Game::ResourceID::Wood: state.wood = lastLocalSnapshot_.resources[i]; break;
            case Game::ResourceID::Steel: state.steel = lastLocalSnapshot_.resources[i]; break;
            case Game::ResourceID::SteamCores: state.steamCores = lastLocalSnapshot_.resources[i]; break;
            case Game::ResourceID::RawFood: state.rawFood = lastLocalSnapshot_.resources[i]; break;
            case Game::ResourceID::FoodRations: state.foodRations = lastLocalSnapshot_.resources[i]; break;
        }
    }
    state.population = lastLocalSnapshot_.population;
    state.temperature = lastLocalSnapshot_.temperature;
    state.hope = lastLocalSnapshot_.hope;
    state.discontent = lastLocalSnapshot_.discontent;

    lastSentState_ = state;

    if (role_ == Network::NetworkManager::Role::Host && clientSteamID_) {
        network_->SendTo(clientSteamID_, &state, sizeof(state), true);
    } else if (role_ == Network::NetworkManager::Role::Client && hostSteamID_) {
        network_->SendTo(hostSteamID_, &state, sizeof(state), true);
    }
}

void FrostpunkMultiplayerMod::SendGameStateDelta() {
    // Delta compression: only send changed fields since last sent state
    Network::GameStateDeltaPayload delta{};
    delta.gameTimeMs = lastLocalSnapshot_.gameTimeMs;
    delta.isPaused = lastLocalSnapshot_.isPaused;
    delta.gameSpeed = lastLocalSnapshot_.gameSpeed;

    // Check which resources changed
    bool hasResourceChanges = false;
    for (int i = 0; i < 6; ++i) {
        int currentAmount = lastLocalSnapshot_.resources[i];
        int lastSentAmount = 0;
        switch (static_cast<Game::ResourceID>(i)) {
            case Game::ResourceID::Coal: lastSentAmount = lastSentState_.coal; break;
            case Game::ResourceID::Wood: lastSentAmount = lastSentState_.wood; break;
            case Game::ResourceID::Steel: lastSentAmount = lastSentState_.steel; break;
            case Game::ResourceID::SteamCores: lastSentAmount = lastSentState_.steamCores; break;
            case Game::ResourceID::RawFood: lastSentAmount = lastSentState_.rawFood; break;
            case Game::ResourceID::FoodRations: lastSentAmount = lastSentState_.foodRations; break;
        }
        if (currentAmount != lastSentAmount) {
            hasResourceChanges = true;
            switch (static_cast<Game::ResourceID>(i)) {
                case Game::ResourceID::Coal: delta.coal = currentAmount; break;
                case Game::ResourceID::Wood: delta.wood = currentAmount; break;
                case Game::ResourceID::Steel: delta.steel = currentAmount; break;
                case Game::ResourceID::SteamCores: delta.steamCores = currentAmount; break;
                case Game::ResourceID::RawFood: delta.rawFood = currentAmount; break;
                case Game::ResourceID::FoodRations: delta.foodRations = currentAmount; break;
            }
        }
    }
    delta.hasResources = hasResourceChanges;

    if (role_ == Network::NetworkManager::Role::Host && clientSteamID_) {
        network_->SendTo(clientSteamID_, &delta, sizeof(delta), true);
    } else if (role_ == Network::NetworkManager::Role::Client && hostSteamID_) {
        network_->SendTo(hostSteamID_, &delta, sizeof(delta), true);
    }
}

void FrostpunkMultiplayerMod::ApplyGameState(const Network::GameStatePayload& state) {
    if (role_ == Network::NetworkManager::Role::Host) return; // Host is authoritative

    // Apply game time - sync client time to host time
    if (state.gameTimeMs > 0) {
        // We don't directly set game time, but we can adjust speed temporarily
        // For now, just track the offset for display purposes
        // Actual time sync would require hooking the calendar conversion
    }

    // Apply pause state
    if (static_cast<bool>(state.isPaused) != lastLocalSnapshot_.isPaused) {
        writer_->SetPaused(static_cast<bool>(state.isPaused));
    }

    // Apply game speed
    if (state.gameSpeed != lastLocalSnapshot_.gameSpeed) {
        writer_->SetGameSpeed(state.gameSpeed);
    }

    // Apply resources (only if significantly different to avoid flickering)
    for (int i = 0; i < 6; ++i) {
        int remoteAmount = 0;
        switch (static_cast<Game::ResourceID>(i)) {
            case Game::ResourceID::Coal: remoteAmount = state.coal; break;
            case Game::ResourceID::Wood: remoteAmount = state.wood; break;
            case Game::ResourceID::Steel: remoteAmount = state.steel; break;
            case Game::ResourceID::SteamCores: remoteAmount = state.steamCores; break;
            case Game::ResourceID::RawFood: remoteAmount = state.rawFood; break;
            case Game::ResourceID::FoodRations: remoteAmount = state.foodRations; break;
        }

        if (remoteAmount >= 0) {
            auto localAmount = writer_->GetResourceAmount(static_cast<Game::ResourceID>(i));
            if (localAmount && std::abs(*localAmount - remoteAmount) > 5) {
                writer_->SetResource(static_cast<Game::ResourceID>(i), remoteAmount);
            }
        }
    }

    // Apply population (read-only, just for display)
    // Apply temperature (read-only, just for display)
    // Hope/Discontent - read-only for now
}

void FrostpunkMultiplayerMod::ApplyGameStateDelta(const Network::GameStateDeltaPayload& delta) {
    if (role_ == Network::NetworkManager::Role::Host) return;

    if (static_cast<bool>(delta.isPaused) != lastLocalSnapshot_.isPaused) {
        writer_->SetPaused(static_cast<bool>(delta.isPaused));
    }

    if (delta.gameSpeed >= 0 && delta.gameSpeed != lastLocalSnapshot_.gameSpeed) {
        writer_->SetGameSpeed(delta.gameSpeed);
    }

    if (delta.hasResources) {
        // Apply resource deltas
        if (delta.coal != 0) {
            auto localAmount = writer_->GetResourceAmount(Game::ResourceID::Coal);
            if (localAmount && std::abs(*localAmount - delta.coal) > 5) {
                writer_->SetResource(Game::ResourceID::Coal, delta.coal);
            }
        }
        if (delta.wood != 0) {
            auto localAmount = writer_->GetResourceAmount(Game::ResourceID::Wood);
            if (localAmount && std::abs(*localAmount - delta.wood) > 5) {
                writer_->SetResource(Game::ResourceID::Wood, delta.wood);
            }
        }
        if (delta.steel != 0) {
            auto localAmount = writer_->GetResourceAmount(Game::ResourceID::Steel);
            if (localAmount && std::abs(*localAmount - delta.steel) > 5) {
                writer_->SetResource(Game::ResourceID::Steel, delta.steel);
            }
        }
        if (delta.steamCores != 0) {
            auto localAmount = writer_->GetResourceAmount(Game::ResourceID::SteamCores);
            if (localAmount && std::abs(*localAmount - delta.steamCores) > 5) {
                writer_->SetResource(Game::ResourceID::SteamCores, delta.steamCores);
            }
        }
        if (delta.rawFood != 0) {
            auto localAmount = writer_->GetResourceAmount(Game::ResourceID::RawFood);
            if (localAmount && std::abs(*localAmount - delta.rawFood) > 5) {
                writer_->SetResource(Game::ResourceID::RawFood, delta.rawFood);
            }
        }
        if (delta.foodRations != 0) {
            auto localAmount = writer_->GetResourceAmount(Game::ResourceID::FoodRations);
            if (localAmount && std::abs(*localAmount - delta.foodRations) > 5) {
                writer_->SetResource(Game::ResourceID::FoodRations, delta.foodRations);
            }
        }
    }
}

void FrostpunkMultiplayerMod::SyncResources() {
    // Resource synchronization handled in ApplyGameState
}

void FrostpunkMultiplayerMod::SyncTime() {
    // Time synchronization - would adjust local time to match host
    // For now, this is a placeholder for future implementation
}

void FrostpunkMultiplayerMod::HandleChatCommand(const char* message) {
    if (!message || !*message) return;

    std::string cmd(message);
    if (cmd.empty() || cmd[0] != '/') return;

    // Parse command and arguments
    std::istringstream iss(cmd.substr(1)); // Skip '/'
    std::string command;
    iss >> command;

    std::transform(command.begin(), command.end(), command.begin(), ::tolower);

    if (command == "pause") {
        if (role_ == Network::NetworkManager::Role::Host) {
            bool newPaused = !lastLocalSnapshot_.isPaused;
            writer_->SetPaused(newPaused);
            network_->SendChatMessage(newPaused ? "Game paused" : "Game resumed");
        } else {
            network_->SendChatMessage("Only host can pause");
        }
    } else if (command == "speed") {
        int speedMode = -1;
        iss >> speedMode;
        if (speedMode >= 0 && speedMode <= 2) {
            if (role_ == Network::NetworkManager::Role::Host) {
                writer_->SetGameSpeed(speedMode);
                const char* names[] = {"1x", "3x", "12x"};
                char msg[64];
                sprintf_s(msg, "Game speed set to %s", names[speedMode]);
                network_->SendChatMessage(msg);
            } else {
                network_->SendChatMessage("Only host can change speed");
            }
        } else {
            network_->SendChatMessage("Usage: /speed 0|1|2 (1x|3x|12x)");
        }
    } else if (command == "trade") {
        int resource = -1, amount = 0;
        iss >> resource >> amount;
        if (resource >= 0 && resource < 6 && amount != 0) {
            // Send trade request
            Network::ResourceTransferPayload transfer{};
            transfer.transferID = static_cast<u32>(GetTimestampMs());
            transfer.resource = static_cast<Network::ResourceTransferPayload::ResourceType>(resource);
            transfer.amount = amount;
            transfer.fromPlayer = localSteamID_;
            transfer.toPlayer = (role_ == Network::NetworkManager::Role::Host) ? clientSteamID_ : hostSteamID_;

            if (transfer.toPlayer != 0) {
                network_->SendTo(transfer.toPlayer, &transfer, sizeof(transfer), true);
                const char* resNames[] = {"Coal", "Wood", "Steel", "Steam Cores", "Raw Food", "Food Rations"};
                char msg[128];
                sprintf_s(msg, "Trade request sent: %d %s", amount, resNames[resource]);
                network_->SendChatMessage(msg);
            } else {
                network_->SendChatMessage("Not connected to other player");
            }
        } else {
            network_->SendChatMessage("Usage: /trade <resource 0-5> <amount>");
        }
    } else if (command == "save") {
        if (role_ == Network::NetworkManager::Role::Host) {
            std::string saveName;
            std::getline(iss, saveName);
            // Trim leading spaces
            while (!saveName.empty() && saveName[0] == ' ') saveName.erase(0, 1);
            if (saveName.empty()) {
                // Generate default name with timestamp
                char defaultName[64];
                time_t t = time(nullptr);
                struct tm tm;
                localtime_s(&tm, &t);
                strftime(defaultName, sizeof(defaultName), "MP_Save_%Y%m%d_%H%M%S", &tm);
                saveName = defaultName;
            }
            writer_->SaveGame(saveName.c_str(), true);
            network_->SendChatMessage(("Save requested: " + saveName).c_str());
        } else {
            network_->SendChatMessage("Only host can save");
        }
    } else if (command == "load") {
        if (role_ == Network::NetworkManager::Role::Host) {
            std::string saveName;
            std::getline(iss, saveName);
            while (!saveName.empty() && saveName[0] == ' ') saveName.erase(0, 1);
            if (!saveName.empty()) {
                writer_->LoadGame(saveName.c_str());
                network_->SendChatMessage(("Load requested: " + saveName).c_str());
            } else {
                network_->SendChatMessage("Usage: /load <save_name>");
            }
        } else {
            network_->SendChatMessage("Only host can load");
        }
    } else if (command == "help") {
        network_->SendChatMessage("Commands: /pause, /speed [0-2], /trade <resource> <amount>, /save [name], /load <name>, /help, /players");
    } else if (command == "players") {
        char msg[128];
        sprintf_s(msg, "Host: %llu, Client: %llu", hostSteamID_, clientSteamID_);
        network_->SendChatMessage(msg);
    } else {
        network_->SendChatMessage(("Unknown command: " + command + ". Type /help").c_str());
    }
}

void FrostpunkMultiplayerMod::OnConnected(u64 peerSteamID) {
    if (role_ == Network::NetworkManager::Role::Host) {
        clientSteamID_ = peerSteamID;
        overlay_->SetClientSteamID(peerSteamID);
        overlay_->SetStatusMessage("Client connected!");
    } else {
        hostSteamID_ = peerSteamID;
        overlay_->SetHostSteamID(peerSteamID);
        overlay_->SetStatusMessage("Connected to host!");
    }
}

void FrostpunkMultiplayerMod::OnDisconnected(u64 peerSteamID, const char* reason) {
    overlay_->SetStatusMessage(reason ? reason : "Disconnected");
    if (role_ == Network::NetworkManager::Role::Host && peerSteamID == clientSteamID_) {
        clientSteamID_ = 0;
    } else if (role_ == Network::NetworkManager::Role::Client && peerSteamID == hostSteamID_) {
        hostSteamID_ = 0;
    }
}

void FrostpunkMultiplayerMod::OnGameStateReceived(u64 senderSteamID, const Network::GameStatePayload* state) {
    if (!state) return;
    ApplyGameState(*state);
}

void FrostpunkMultiplayerMod::OnGameStateDeltaReceived(u64 senderSteamID, const Network::GameStateDeltaPayload* delta) {
    if (!delta) return;
    ApplyGameStateDelta(*delta);
}

void FrostpunkMultiplayerMod::OnPlayerActionReceived(u64 senderSteamID, const Network::PlayerActionPayload* action) {
    // Handle player action replication
    // This would require hooking into the game's entity system
}

void FrostpunkMultiplayerMod::OnResourceTransferReceived(u64 senderSteamID, const Network::ResourceTransferPayload* transfer) {
    if (!transfer) return;

    if (role_ == Network::NetworkManager::Role::Host) {
        // Host validates and processes transfer
        bool success = writer_->ChangeResource(
            static_cast<Game::ResourceID>(transfer->resource),
            transfer->amount
        );

        Network::ResourceTransferResultPayload result{};
        result.transferID = transfer->transferID;
        result.result = success ? Network::ResourceTransferResultPayload::Result::Success
                                : Network::ResourceTransferResultPayload::Result::InsufficientResources;
        result.appliedAmount = success ? transfer->amount : 0;

        network_->SendTo(senderSteamID, &result, sizeof(result), true);
    }
}

void FrostpunkMultiplayerMod::OnResourceTransferResultReceived(u64 senderSteamID, const Network::ResourceTransferResultPayload* result) {
    if (!result) return;
    // Client receives result of their transfer request
    const char* statusStr = "";
    switch (result->result) {
        case Network::ResourceTransferResultPayload::Result::Success: statusStr = "Transfer successful"; break;
        case Network::ResourceTransferResultPayload::Result::InsufficientResources: statusStr = "Insufficient resources"; break;
        case Network::ResourceTransferResultPayload::Result::InvalidResource: statusStr = "Invalid resource"; break;
        case Network::ResourceTransferResultPayload::Result::NotConnected: statusStr = "Not connected"; break;
        case Network::ResourceTransferResultPayload::Result::AlreadyProcessing: statusStr = "Already processing"; break;
    }
    overlay_->SetStatusMessage(statusStr);
}

void FrostpunkMultiplayerMod::OnChatMessageReceived(u64 senderSteamID, const Network::ChatMessagePayload* chat) {
    if (!chat) return;
    overlay_->AddChatMessage(chat->senderName, chat->message);
}

void FrostpunkMultiplayerMod::OnSaveRequestReceived(u64 senderSteamID, const Network::SaveRequestPayload* save) {
    if (!save) return;

    if (role_ == Network::NetworkManager::Role::Host) {
        // Host performs the save
        bool success = writer_->SaveGame(save->saveName, static_cast<bool>(save->isMultiplayerSave));
        // Send save data to client
        // This would need to read the actual save file and send it
    }
}

void FrostpunkMultiplayerMod::OnLoadRequestReceived(u64 senderSteamID, const Network::LoadRequestPayload* load) {
    if (!load) return;

    if (role_ == Network::NetworkManager::Role::Client) {
        // Client receives load request from host
        writer_->LoadGame(load->saveName);
        // Send confirmation to host
        Network::LoadResponsePayload response{};
        strncpy_s(response.saveName, load->saveName, sizeof(response.saveName) - 1);
        response.success = true;
        network_->SendTo(hostSteamID_, &response, sizeof(response), true);
    }
}

void FrostpunkMultiplayerMod::OnLoadResponseReceived(u64 senderSteamID, const Network::LoadResponsePayload* loadResponse) {
    if (!loadResponse) return;

    if (role_ == Network::NetworkManager::Role::Host) {
        // Host receives load confirmation from client
        if (loadResponse->success) {
            overlay_->SetStatusMessage("Client loaded save successfully");
        } else {
            char msg[256];
            sprintf_s(msg, "Client failed to load save: %s", loadResponse->errorMessage);
            overlay_->SetStatusMessage(msg);
        }
    }
}

void FrostpunkMultiplayerMod::OnMapSelectReceived(u64 senderSteamID, const Network::MapSelectPayload* map) {
    if (!map) return;

    if (role_ == Network::NetworkManager::Role::Client) {
        // Client receives map selection from host
        writer_->SelectMap(map->mapIndex);
        // Send confirmation
        Network::MapSelectedPayload response{};
        response.mapIndex = map->mapIndex;
        response.scenarioType = map->scenarioType;
        response.difficulty = map->difficulty;
        response.randomSeed = map->randomSeed;
        response.confirmed = true;
        network_->SendTo(hostSteamID_, &response, sizeof(response), true);
    }
}

void FrostpunkMultiplayerMod::OnGameStartReceived(u64 senderSteamID, const Network::GameStartPayload* start) {
    if (!start) return;

    if (role_ == Network::NetworkManager::Role::Client) {
        // Client starts the game
        writer_->StartEndlessGame();
        network_->SetStateChangeHandler(nullptr); // Suppress for now
        network_->SetStateChangeHandler([this](auto old, auto n) {
            overlay_->SetConnectionState(static_cast<int>(n));
        });
    }
}

void FrostpunkMultiplayerMod::HookGameFunctions() {
    if (hooksInstalled_) return;

    bool allCriticalOk = true;

    // ---- CRITICAL hooks (verified RVAs for 1.6.2) ----
    // SetSpeed is DISABLED - causes crash ~3s after launch (even with empty callback).
    // Other hooks verified stable individually.
    TraceToFile("HOOK: Installing ChangeResource (RVA 0xE55880)");
    if (!hooks_->InstallCallbackHook("ChangeResource", 0xE55880,
                                     &FrostpunkMultiplayerMod::OnChangeResourceHookStatic, this)) {
        allCriticalOk = false;
        TraceToFile("HOOK: FAILED ChangeResource");
        DebugLog("HookGameFunctions: FAILED to install ChangeResource");
    } else {
        TraceToFile("HOOK: Installed ChangeResource");
        DebugLog("HookGameFunctions: Installed ChangeResource (RVA 0xE55880)");
    }
    TraceToFile("HOOK: Installing PauseRemoveReason (RVA 0x120A4E0)");
    if (!hooks_->InstallCallbackHook("PauseRemoveReason", 0x120A4E0,
                                     &FrostpunkMultiplayerMod::OnPauseRemoveReasonHookStatic, this)) {
        allCriticalOk = false;
        TraceToFile("HOOK: FAILED PauseRemoveReason");
        DebugLog("HookGameFunctions: FAILED to install PauseRemoveReason");
    } else {
        TraceToFile("HOOK: Installed PauseRemoveReason");
        DebugLog("HookGameFunctions: Installed PauseRemoveReason (RVA 0x120A4E0)");
    }
    TraceToFile("HOOK: Installing NativeSave (RVA 0x16A2850)");
    if (!hooks_->InstallCallbackHook("NativeSave", 0x16A2850,
                                     &FrostpunkMultiplayerMod::OnNativeSaveHookStatic, this)) {
        allCriticalOk = false;
        TraceToFile("HOOK: FAILED NativeSave");
        DebugLog("HookGameFunctions: FAILED to install NativeSave");
    } else {
        TraceToFile("HOOK: Installed NativeSave");
        DebugLog("HookGameFunctions: Installed NativeSave (RVA 0x16A2850)");
    }
    TraceToFile("HOOK: Installing EndlessStart (RVA 0xDCDA00)");
    if (!hooks_->InstallCallbackHook("EndlessStart", 0xDCDA00,
                                     &FrostpunkMultiplayerMod::OnEndlessStartHookStatic, this)) {
        allCriticalOk = false;
        TraceToFile("HOOK: FAILED EndlessStart");
        DebugLog("HookGameFunctions: FAILED to install EndlessStart");
    } else {
        TraceToFile("HOOK: Installed EndlessStart");
        DebugLog("HookGameFunctions: Installed EndlessStart (RVA 0xDCDA00)");
    }

    hooksInstalled_ = allCriticalOk;
    char msg[128];
    snprintf(msg, sizeof(msg), "HOOK: HookGameFunctions done, installed=%d, ok=%d",
        hooksInstalled_, allCriticalOk);
    TraceToFile(msg);
    DebugLog("HookGameFunctions: hooksInstalled_=%d (critical hooks OK=%d)",
        hooksInstalled_, allCriticalOk);
}

void FrostpunkMultiplayerMod::UnhookGameFunctions() {
    if (!hooksInstalled_) return;

    hooks_->RemoveAllHooks();
    hooksInstalled_ = false;
}

// Static callback implementations (HookCallback signature: bool(*)(uintptr_t, void*, void*))

// Game speed hook - block client changes, allow host, sync via network
bool FrostpunkMultiplayerMod::OnSetSpeedHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    // SetSpeed hook is DISABLED - it causes a crash (~3s after launch) even with
    // an empty callback and return=false. The detour mechanism itself breaks for
    // this specific function. Investigating further is needed.
    return true;
}

// Resource change hook - block client changes, allow host, sync via network
bool FrostpunkMultiplayerMod::OnChangeResourceHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    static std::atomic<int> callCount{0};
    int calls = ++callCount;
    if (calls <= 3) {
        char msg[128];
        snprintf(msg, sizeof(msg), "HOOK TRIGGERED: ChangeResource (call #%d)", calls);
        TraceToFile(msg);
    }
    __try {
        auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
        if (!instance) return true;

        struct HookContext {
            std::uintptr_t rcx;  // economy
            std::uintptr_t rdx;  // resourceEntry
            std::uintptr_t r8;   // delta
            std::uintptr_t r9;   // eventContext
            std::uintptr_t retAddr;
        };
        auto c = static_cast<HookContext*>(context);

        if (instance->role_ == Network::NetworkManager::Role::Client) {
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("CRASH in OnChangeResourceHookStatic!");
        return true;
    }
}

// Pause add hook - track network pause token, block unauthorized pauses
bool FrostpunkMultiplayerMod::OnPauseAddReasonHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    __try {
        auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
        if (!instance) return true;

        struct HookContext {
            std::uintptr_t rcx;  // timeController
            std::uintptr_t rdx;  // reason
            std::uintptr_t r8;
            std::uintptr_t r9;
            std::uintptr_t retAddr;
        };
        auto c = static_cast<HookContext*>(context);

        if (instance->role_ == Network::NetworkManager::Role::Host) {
            instance->networkPauseToken_ = c->rdx;
        } else if (instance->role_ == Network::NetworkManager::Role::Client) {
            if (c->rdx != instance->networkPauseToken_) {
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("CRASH in OnPauseAddReasonHookStatic!");
        return true;
    }
}

// Pause remove hook - clear network pause token
bool FrostpunkMultiplayerMod::OnPauseRemoveReasonHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    static std::atomic<int> callCount{0};
    int calls = ++callCount;
    if (calls <= 3) {
        char msg[128];
        snprintf(msg, sizeof(msg), "HOOK TRIGGERED: PauseRemoveReason (call #%d)", calls);
        TraceToFile(msg);
    }
    __try {
        auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
        if (!instance) return true;

        struct HookContext {
            std::uintptr_t rcx;  // timeController
            std::uintptr_t rdx;  // reason
            std::uintptr_t r8;
            std::uintptr_t r9;
            std::uintptr_t retAddr;
        };
        auto c = static_cast<HookContext*>(context);

        if (c->rdx == instance->networkPauseToken_) {
            instance->networkPauseToken_ = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("CRASH in OnPauseRemoveReasonHookStatic!");
        return true;
    }
}

// Save hook - sync save to client
bool FrostpunkMultiplayerMod::OnNativeSaveHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    static std::atomic<int> callCount{0};
    int calls = ++callCount;
    if (calls <= 3) {
        char msg[128];
        snprintf(msg, sizeof(msg), "HOOK TRIGGERED: NativeSave (call #%d)", calls);
        TraceToFile(msg);
    }
    __try {
        auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
        if (!instance) return true;

        struct HookContext {
            std::uintptr_t rcx;  // saveOwner
            std::uintptr_t rdx;  // multiplayer
            std::uintptr_t r8;   // overwrite
            std::uintptr_t r9;   // name
            std::uintptr_t retAddr;
        };
        auto c = static_cast<HookContext*>(context);

        if (instance->role_ == Network::NetworkManager::Role::Host && c->r9) {
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("CRASH in OnNativeSaveHookStatic!");
        return true;
    }
}

// Load hook - sync load to client
bool FrostpunkMultiplayerMod::OnNativeLoadHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
    if (!instance) return true;

    struct HookContext {
        std::uintptr_t rcx;  // saveOwner
        std::uintptr_t rdx;  // name
        std::uintptr_t r8;
        std::uintptr_t r9;
        std::uintptr_t retAddr;
    };
    auto c = static_cast<HookContext*>(context);

    if (c->rdx) {
        if (instance->role_ == Network::NetworkManager::Role::Host) {
            Network::LoadRequestPayload load{};
            std::strncpy(load.saveName, reinterpret_cast<const char*>(c->rdx), sizeof(load.saveName) - 1);
            if (instance->clientSteamID_) {
                instance->network_->SendTo(instance->clientSteamID_, &load, sizeof(load), true);
            }
        }
    }
    return true;
}

// Map selection hook - send to client
bool FrostpunkMultiplayerMod::OnSelectMapHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
    if (!instance) return true;

    struct HookContext {
        std::uintptr_t rcx;  // timeController
        std::uintptr_t rdx;  // mapIndex
        std::uintptr_t r8;
        std::uintptr_t r9;
        std::uintptr_t retAddr;
    };
    auto c = static_cast<HookContext*>(context);

    if (instance->role_ == Network::NetworkManager::Role::Host) {
        Network::MapSelectPayload map{};
        map.mapIndex = static_cast<int>(c->rdx);
        map.scenarioType = 0;
        map.difficulty = 0;
        map.randomSeed = 0;
        instance->network_->SendTo(instance->clientSteamID_, &map, sizeof(map), true);
        instance->network_->OnInitialGameStateSent();
    }
    return true;
}

// Endless start hook - notify client to start
bool FrostpunkMultiplayerMod::OnEndlessStartHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    static std::atomic<bool> firstCall{true};
    if (firstCall.exchange(false)) {
        TraceToFile("HOOK TRIGGERED: EndlessStart (first call)");
    }
    __try {
        auto instance = static_cast<FrostpunkMultiplayerMod*>(userData);
        if (!instance) return true;

        TraceToFile("HOOK: EndlessStart executing");

        // Network sends are deferred to the update thread — never call
        // network_->SendTo from inside a hook detour (crash risk).
        if (instance->role_ == Network::NetworkManager::Role::Host) {
            instance->inGame_ = true;
            TraceToFile("HOOK: EndlessStart - host role, marked inGame=true");
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("CRASH in OnEndlessStartHookStatic!");
        return true;
    }
}

// ============================================================
// Static Hook Handler Methods (called from non-static hooks)
// These contain the actual hook logic
// ============================================================

// Game speed hook - block client changes, allow host, sync via network
bool FrostpunkMultiplayerMod::OnSetSpeed(std::uintptr_t timeController, int speedMode) {
    // For now, allow all speed changes - the static callback does the blocking
    // This method can be expanded for more complex logic
    return true;
}

// Resource change hook - block client changes, allow host, sync via network
bool FrostpunkMultiplayerMod::OnChangeResource(std::uintptr_t economy, std::uintptr_t resourceEntry,
                                               int delta, void* eventContext, int changeType, bool option) {
    // For now, allow all resource changes - the static callback does the blocking
    // This method can be expanded for more complex logic
    return true;
}

// Pause add hook - track network pause token, block unauthorized pauses
std::uintptr_t FrostpunkMultiplayerMod::OnPauseAddReason(std::uintptr_t timeController, void* reason) {
    // Return the reason token - can be used to track network pauses
    return reinterpret_cast<std::uintptr_t>(reason);
}

// Pause remove hook - clear network pause token
void FrostpunkMultiplayerMod::OnPauseRemoveReason(std::uintptr_t timeController, void* reason) {
    // Could clear network pause token tracking here
}

// Save hook - sync save to client
bool FrostpunkMultiplayerMod::OnNativeSave(std::uintptr_t saveOwner, bool multiplayer, bool overwrite, const char* name) {
    // Allow save - the static callback handles network sync
    return true;
}

// Load hook - sync load to client
bool FrostpunkMultiplayerMod::OnNativeLoad(std::uintptr_t saveOwner, const char* name) {
    // Allow load - the static callback handles network sync
    return true;
}

// Map selection hook - send to client
bool FrostpunkMultiplayerMod::OnSelectMap(std::uintptr_t timeController, int mapIndex) {
    // Allow map selection - the static callback handles network sync
    return true;
}

// Endless start hook - notify client to start
bool FrostpunkMultiplayerMod::OnEndlessStart(std::uintptr_t timeController) {
    // Allow game start - the static callback handles network sync
    return true;
}

// Menu hooks - placeholders
bool FrostpunkMultiplayerMod::OnMenuUpdateHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    return true;
}
bool FrostpunkMultiplayerMod::OnMenuBindButtonsHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    return true;
}
bool FrostpunkMultiplayerMod::OnMenuPanelCallbackHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    return true;
}
bool FrostpunkMultiplayerMod::OnScenariosUpdateHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    return true;
}
bool FrostpunkMultiplayerMod::OnScenarioRowCallbackHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    return true;
}
bool FrostpunkMultiplayerMod::OnSelectEndlessModeHookStatic(std::uintptr_t returnAddress, void* context, void* userData) {
    return true;
}

// Menu hooks for multiplayer UI integration
void FrostpunkMultiplayerMod::OnMenuUpdate(std::uintptr_t panel) {
    // Inject multiplayer button into main menu
    // This runs every frame - we'll add the button once
    auto instance = GetModInstance();
    if (instance) {
        static bool buttonAdded = false;
        if (!buttonAdded) {
            instance->AddMultiplayerButton(panel);
            buttonAdded = true;
        }
    }
}

void FrostpunkMultiplayerMod::OnMenuBindButtons(std::uintptr_t panel) {
    // Bind multiplayer button callback
    auto instance = GetModInstance();
    if (instance) {
        instance->BindMultiplayerButton(panel);
    }
}

void FrostpunkMultiplayerMod::OnMenuPanelCallback(std::uintptr_t panel, void* event) {
    // Handle menu events - could detect button clicks
    auto instance = GetModInstance();
    if (instance) {
        instance->HandleMenuEvent(panel, event);
    }
}

void FrostpunkMultiplayerMod::OnScenariosUpdate(std::uintptr_t panel) {
    // Modify scenarios panel for multiplayer - add "Multiplayer" scenario type
    auto instance = GetModInstance();
    if (instance) {
        instance->AddMultiplayerScenarioOption(panel);
    }
}

void FrostpunkMultiplayerMod::OnScenarioRowCallback(std::uintptr_t panel, void* event) {
    // Handle scenario row selection - if multiplayer selected, show connection screen
    auto instance = GetModInstance();
    if (instance) {
        instance->HandleScenarioSelection(panel, event);
    }
}

void FrostpunkMultiplayerMod::OnSelectEndlessMode(std::uintptr_t panel, void* param) {
    // Redirect endless mode to multiplayer connection screen
    auto instance = GetModInstance();
    if (instance) {
        instance->ShowMultiplayerConnectionScreen();
    }
}

// UI Integration Helpers (simplified - overlay provides hotkey-based UI)
void FrostpunkMultiplayerMod::AddMultiplayerButton(std::uintptr_t panel) {
    // Scaleform UI integration is complex - using overlay hotkeys instead (F1=Chat, F2=Trade, F3=Save)
    // This would require creating GFxMovie objects and adding to display list
}

void FrostpunkMultiplayerMod::BindMultiplayerButton(std::uintptr_t panel) {
    // Not implemented - overlay provides hotkey-based UI
}

void FrostpunkMultiplayerMod::HandleMenuEvent(std::uintptr_t panel, void* event) {
    // Not implemented - overlay provides hotkey-based UI
}

void FrostpunkMultiplayerMod::AddMultiplayerScenarioOption(std::uintptr_t panel) {
    // Not implemented - overlay provides hotkey-based UI
}

void FrostpunkMultiplayerMod::HandleScenarioSelection(std::uintptr_t panel, void* event) {
    // Not implemented - overlay provides hotkey-based UI
}

void FrostpunkMultiplayerMod::ShowMultiplayerConnectionScreen() {
    // Show connection status in overlay
    overlay_->ShowChatUI(true);
    overlay_->SetStatusMessage("Multiplayer active - F1: Chat, F2: Trade, F3: Save, F10: Toggle overlay");
}

// ============================================================
// Hook Handler Methods (called from InstallCallbackHook lambdas)
// These extract parameters and call static hook functions
// ============================================================

// Context layout (from detour assembly):
// ctx points to saved registers area:
//   +0x00: saved RCX (1st arg)
//   +0x08: saved RDX (2nd arg)
//   +0x10: saved R8  (3rd arg)
//   +0x18: saved R9  (4th arg)
//   +0x20: saved return address

struct HookContext {
    std::uintptr_t rcx;  // 1st arg
    std::uintptr_t rdx;  // 2nd arg
    std::uintptr_t r8;   // 3rd arg
    std::uintptr_t r9;   // 4th arg
    std::uintptr_t retAddr;
};

bool FrostpunkMultiplayerMod::OnSetSpeedHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // SetSpeed(timeController, speedMode): RCX=timeController, RDX=speedMode
    bool allow = OnSetSpeed(c->rcx, static_cast<int>(c->rdx));
    return allow; // true = continue to original, false = skip
}

bool FrostpunkMultiplayerMod::OnChangeResourceHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // ChangeResource(economy, resourceEntry, delta, eventContext, changeType, option)
    // RCX=economy, RDX=resourceEntry, R8=delta, R9=eventContext
    bool allow = OnChangeResource(c->rcx, c->rdx, static_cast<int>(c->r8),
                                   reinterpret_cast<void*>(c->r9), 0, false);
    return allow;
}

bool FrostpunkMultiplayerMod::OnPauseAddReasonHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // PauseAddReason(timeController, reason): RCX=timeController, RDX=reason
    // Returns the reason token (or 0 to block)
    std::uintptr_t result = OnPauseAddReason(c->rcx, reinterpret_cast<void*>(c->rdx));
    // We can't easily modify the return value from here since the detour doesn't support it
    // The static function is called from writer_->SetPaused for network pauses
    // For now, just continue to original
    return true;
}

void FrostpunkMultiplayerMod::OnPauseRemoveReasonHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // PauseRemoveReason(timeController, reason): RCX=timeController, RDX=reason
    OnPauseRemoveReason(c->rcx, reinterpret_cast<void*>(c->rdx));
}

void FrostpunkMultiplayerMod::OnNativeSaveHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // NativeSave(saveOwner, multiplayer, overwrite, name)
    // RCX=saveOwner, RDX=multiplayer, R8=overwrite, R9=name
    OnNativeSave(c->rcx, static_cast<bool>(c->rdx), static_cast<bool>(c->r8),
                 reinterpret_cast<const char*>(c->r9));
}

void FrostpunkMultiplayerMod::OnNativeLoadHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // NativeLoad(saveOwner, name): RCX=saveOwner, RDX=name
    OnNativeLoad(c->rcx, reinterpret_cast<const char*>(c->rdx));
}

void FrostpunkMultiplayerMod::OnSelectMapHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // SelectMap(timeController, mapIndex): RCX=timeController, RDX=mapIndex
    OnSelectMap(c->rcx, static_cast<int>(c->rdx));
}

void FrostpunkMultiplayerMod::OnEndlessStartHook(std::uintptr_t retAddr, void* ctx) {
    auto c = static_cast<HookContext*>(ctx);
    // EndlessStart(timeController): RCX=timeController
    OnEndlessStart(c->rcx);
}

// Menu hook handlers - placeholders for UI integration
void FrostpunkMultiplayerMod::OnMenuUpdateHook(std::uintptr_t retAddr, void* ctx) {
    OnMenuUpdate(retAddr);
}

void FrostpunkMultiplayerMod::OnMenuBindButtonsHook(std::uintptr_t retAddr, void* ctx) {
    OnMenuBindButtons(retAddr);
}

void FrostpunkMultiplayerMod::OnMenuPanelCallbackHook(std::uintptr_t retAddr, void* ctx) {
    OnMenuPanelCallback(retAddr, ctx);
}

void FrostpunkMultiplayerMod::OnScenariosUpdateHook(std::uintptr_t retAddr, void* ctx) {
    OnScenariosUpdate(retAddr);
}

void FrostpunkMultiplayerMod::OnScenarioRowCallbackHook(std::uintptr_t retAddr, void* ctx) {
    OnScenarioRowCallback(retAddr, ctx);
}

void FrostpunkMultiplayerMod::OnSelectEndlessModeHook(std::uintptr_t retAddr, void* ctx) {
    OnSelectEndlessMode(retAddr, ctx);
}

// Forward declarations (defined in DllMain.cpp)
void InitializeMod(u64 localSteamID, Network::NetworkManager::Role role, u64 hostSteamID);
void ShutdownMod();

// Exported functions for launcher to call
extern "C" {
    __declspec(dllexport) void FrostMP_InitializeHost(u64 localSteamID) {
        TraceToFile("FrostMP_InitializeHost ENTRY");
        try {
            DebugLog("FrostMP_InitializeHost called with localSteamID=%llu", localSteamID);
            InitializeMod(localSteamID, Network::NetworkManager::Role::Host, 0);
            TraceToFile("FrostMP_InitializeHost EXIT OK");
        } catch (const std::exception& e) {
            TraceToFile(("FrostMP_InitializeHost EXCEPTION: " + std::string(e.what())).c_str());
        } catch (...) {
            TraceToFile("FrostMP_InitializeHost UNKNOWN EXCEPTION");
        }
    }

    __declspec(dllexport) void FrostMP_InitializeClient(u64 localSteamID, u64 hostSteamID) {
        TraceToFile("FrostMP_InitializeClient ENTRY");
        try {
            DebugLog("FrostMP_InitializeClient called with localSteamID=%llu, hostSteamID=%llu", localSteamID, hostSteamID);
            InitializeMod(localSteamID, Network::NetworkManager::Role::Client, hostSteamID);
            TraceToFile("FrostMP_InitializeClient EXIT OK");
        } catch (const std::exception& e) {
            TraceToFile(("FrostMP_InitializeClient EXCEPTION: " + std::string(e.what())).c_str());
        } catch (...) {
            TraceToFile("FrostMP_InitializeClient UNKNOWN EXCEPTION");
        }
    }

    __declspec(dllexport) void FrostMP_Shutdown() {
        TraceToFile("FrostMP_Shutdown ENTRY");
        ShutdownMod();
        TraceToFile("FrostMP_Shutdown EXIT OK");
    }

    __declspec(dllexport) bool FrostMP_IsInitialized() {
        return GetModInstance() != nullptr;
    }

    __declspec(dllexport) int FrostMP_GetConnectionState() {
        auto mod = GetModInstance();
        if (!mod) return 0;
        return mod->IsHost() ? 2 : (mod->IsClient() ? 3 : 1);
    }
}

} // namespace FrostMP::Mod