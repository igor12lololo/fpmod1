#pragma once
#include "Common.h"
#include "GameHooks/GameStructs.h"
#include "GameHooks/Memory.h"
#include "GameHooks/GameReader.h"
#include "GameHooks/GameWriter.h"
#include "GameHooks/HookManager.h"
#include "Network/NetworkManager.h"
#include "Overlay/OverlayUI.h"

namespace FrostMP::Mod {

// Version detection (mirrors launcher)
enum class GameVersion {
    Unknown = 0,
    V1_6_1 = 1,
    V1_6_2 = 2
};

inline GameVersion DetectGameVersion(const std::string& sha256) {
    // 1.6.1 hash
    if (sha256 == "719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4") return GameVersion::V1_6_1;
    // 1.6.2 hash - verified 2026-11-13
    if (sha256 == "031dbfe529256b7bf532aa11bfaf8ef75ef0618aa0492a970e532566342ba19d") return GameVersion::V1_6_2;
    return GameVersion::Unknown;
}

// Detailed initialization status
struct InitializationStatus {
    bool hooks = false;
    bool overlay = false;
    bool network = false;
    bool reader = false;
    bool writer = false;
    bool gameVersionOk = false;
    GameVersion detectedVersion = GameVersion::Unknown;
    std::string errorMessage;

    bool IsFullyInitialized() const {
        return hooks && overlay && network && reader && writer && gameVersionOk;
    }

    bool IsCriticalFailure() const {
        return !hooks || !reader || !writer || !gameVersionOk;
    }
};

class FrostpunkMultiplayerMod {
public:
    FrostpunkMultiplayerMod();
    ~FrostpunkMultiplayerMod();

    bool Initialize(u64 localSteamID, Network::NetworkManager::Role role, u64 hostSteamID = 0);
    void Shutdown();
    void Update();

    // Get detailed initialization status
    InitializationStatus GetInitializationStatus() const { return initStatus_; }

    // Network callbacks
    void OnConnected(u64 peerSteamID);
    void OnDisconnected(u64 peerSteamID, const char* reason);
    void OnGameStateReceived(u64 senderSteamID, const Network::GameStatePayload* state);
    void OnGameStateDeltaReceived(u64 senderSteamID, const Network::GameStateDeltaPayload* delta);
    void OnPlayerActionReceived(u64 senderSteamID, const Network::PlayerActionPayload* action);
    void OnResourceTransferReceived(u64 senderSteamID, const Network::ResourceTransferPayload* transfer);
    void OnResourceTransferResultReceived(u64 senderSteamID, const Network::ResourceTransferResultPayload* result);
    void OnChatMessageReceived(u64 senderSteamID, const Network::ChatMessagePayload* chat);
    void OnSaveRequestReceived(u64 senderSteamID, const Network::SaveRequestPayload* save);
    void OnLoadRequestReceived(u64 senderSteamID, const Network::LoadRequestPayload* load);
    void OnLoadResponseReceived(u64 senderSteamID, const Network::LoadResponsePayload* loadResponse);
    void OnMapSelectReceived(u64 senderSteamID, const Network::MapSelectPayload* map);
    void OnGameStartReceived(u64 senderSteamID, const Network::GameStartPayload* start);

    // Game hooks
    void HookGameFunctions();
    void UnhookGameFunctions();

    // State access
    bool IsHost() const { return role_ == Network::NetworkManager::Role::Host; }
    bool IsClient() const { return role_ == Network::NetworkManager::Role::Client; }
    bool IsInGame() const { return inGame_; }
    u64 GetHostSteamID() const { return hostSteamID_; }
    u64 GetClientSteamID() const { return clientSteamID_; }

private:
    void UpdateGameState();
    void SendGameState();
    void SendGameStateDelta();
    void ApplyGameState(const Network::GameStatePayload& state);
    void ApplyGameStateDelta(const Network::GameStateDeltaPayload& delta);
    void SyncResources();
    void SyncTime();
    void HandleChatCommand(const char* message);

    // Helper for version verification
    static std::string ComputeSHA256(const wchar_t* filePath);

    // Hook handler methods (called from InstallCallbackHook lambdas)
    bool OnSetSpeedHook(std::uintptr_t retAddr, void* ctx);
    bool OnChangeResourceHook(std::uintptr_t retAddr, void* ctx);
    bool OnPauseAddReasonHook(std::uintptr_t retAddr, void* ctx);
    void OnPauseRemoveReasonHook(std::uintptr_t retAddr, void* ctx);
    void OnNativeSaveHook(std::uintptr_t retAddr, void* ctx);
    void OnNativeLoadHook(std::uintptr_t retAddr, void* ctx);
    void OnSelectMapHook(std::uintptr_t retAddr, void* ctx);
    void OnEndlessStartHook(std::uintptr_t retAddr, void* ctx);

    // Menu hook handlers
    void OnMenuUpdateHook(std::uintptr_t retAddr, void* ctx);
    void OnMenuBindButtonsHook(std::uintptr_t retAddr, void* ctx);
    void OnMenuPanelCallbackHook(std::uintptr_t retAddr, void* ctx);
    void OnScenariosUpdateHook(std::uintptr_t retAddr, void* ctx);
    void OnScenarioRowCallbackHook(std::uintptr_t retAddr, void* ctx);
    void OnSelectEndlessModeHook(std::uintptr_t retAddr, void* ctx);

    // Hook callbacks (static - called from hook handlers)
    static bool OnSetSpeed(std::uintptr_t timeController, int speedMode);
    static bool OnChangeResource(std::uintptr_t economy, std::uintptr_t resourceEntry, int delta, void* eventContext, int changeType, bool option);
    static std::uintptr_t OnPauseAddReason(std::uintptr_t timeController, void* reason);
    static void OnPauseRemoveReason(std::uintptr_t timeController, void* reason);
    static bool OnNativeSave(std::uintptr_t saveOwner, bool multiplayer, bool overwrite, const char* name);
    static bool OnNativeLoad(std::uintptr_t saveOwner, const char* name);
    static bool OnSelectMap(std::uintptr_t timeController, int mapIndex);
    static bool OnEndlessStart(std::uintptr_t timeController);

    // Static HookCallback implementations (for HookManager::InstallCallbackHook)
    static bool OnSetSpeedHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnChangeResourceHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnPauseAddReasonHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnPauseRemoveReasonHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnNativeSaveHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnNativeLoadHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnSelectMapHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnEndlessStartHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnMenuUpdateHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnMenuBindButtonsHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnMenuPanelCallbackHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnScenariosUpdateHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnScenarioRowCallbackHookStatic(std::uintptr_t returnAddress, void* context, void* userData);
    static bool OnSelectEndlessModeHookStatic(std::uintptr_t returnAddress, void* context, void* userData);

    // Menu hooks
    static void OnMenuUpdate(std::uintptr_t panel);
    static void OnMenuBindButtons(std::uintptr_t panel);
    static void OnMenuPanelCallback(std::uintptr_t panel, void* event);
    static void OnScenariosUpdate(std::uintptr_t panel);
    static void OnScenarioRowCallback(std::uintptr_t panel, void* event);
    static void OnSelectEndlessMode(std::uintptr_t panel, void* param);

    // UI Integration Helpers (overlay provides hotkey-based UI)
    void AddMultiplayerButton(std::uintptr_t panel);
    void BindMultiplayerButton(std::uintptr_t panel);
    void HandleMenuEvent(std::uintptr_t panel, void* event);
    void AddMultiplayerScenarioOption(std::uintptr_t panel);
    void HandleScenarioSelection(std::uintptr_t panel, void* event);
    void ShowMultiplayerConnectionScreen();

    Game::LocalMemory mem_;
    std::unique_ptr<Game::GameReader> reader_;
    std::unique_ptr<Game::GameWriter> writer_;
    std::unique_ptr<Game::HookManager> hooks_;
    std::unique_ptr<Network::NetworkManager> network_;
    std::unique_ptr<Overlay::OverlayManager> overlay_;

    Network::NetworkManager::Role role_ = Network::NetworkManager::Role::None;
    u64 localSteamID_ = 0;
    u64 hostSteamID_ = 0;
    u64 clientSteamID_ = 0;

    bool initialized_ = false;
    bool inGame_ = false;
    bool hooksInstalled_ = false;

    // State tracking
    std::atomic<u32> lastSentSequence_{0};
    std::atomic<u32> lastReceivedSequence_{0};
    std::atomic<u64> lastStateSendTime_{0};
    Network::GameStatePayload lastSentState_{};
    Game::GameStateSnapshot lastLocalSnapshot_{};

    // Network pause token
    std::uintptr_t networkPauseToken_ = 0;

    // Reader/writer initialized when game session detected
    bool readerInitialized_ = false;

    // Initialization status tracking
    InitializationStatus initStatus_{};

    // Thread-safety for Initialize/Shutdown (deferred init vs launcher init)
    std::mutex initMutex_;

    // Singleton instance for static callbacks
    static FrostpunkMultiplayerMod* instance_;

    friend FrostpunkMultiplayerMod* GetModInstance();
};

// Global access
FrostpunkMultiplayerMod* GetModInstance();

} // namespace FrostMP::Mod