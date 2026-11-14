#pragma once
#include "Common.h"

namespace FrostMP::Overlay {

struct OverlayControl {
    static constexpr u32 MAGIC = 0x464D504F; // "FMPO"
    static constexpr u32 VERSION = 1;

    u32 signature = MAGIC;
    u32 layoutVersion = VERSION;
    std::atomic<u32> modReady{0};

    // Connection state
    std::atomic<int> connectionState{0}; // 0=Disconnected, 1=Connecting, 2=Connected, 3=InGame
    std::atomic<u64> hostSteamID{0};
    std::atomic<u64> clientSteamID{0};
    std::atomic<u64> localSteamID{0};

    // Game state
    std::atomic<int> gameSpeed{0};
    std::atomic<int> isPaused{0};
    std::atomic<long long> gameTimeMs{0};

    // Resources (host->client sync)
    std::atomic<int> resources[6]{};

    // UI state
    std::atomic<int> showTradeUI{0};
    std::atomic<int> showChatUI{0};
    std::atomic<int> showSaveUI{0};

    // Chat
    struct ChatMessage {
        char sender[64];
        char message[512];
        u64 timestamp;
    };
    std::atomic<int> chatCount{0};
    ChatMessage chatHistory[100];

    // Trade
    struct TradeOffer {
        u32 offerID;
        u64 fromPlayer;
        u64 toPlayer;
        int resource;
        int amount;
        int status; // 0=Pending, 1=Accepted, 2=Declined
    };
    std::atomic<int> tradeCount{0};
    TradeOffer tradeOffers[50];

    // Input
    char inputBuffer[256];
    std::atomic<int> inputActive{0};
    std::atomic<int> inputType{0}; // 0=Chat, 1=SaveName, 2=SteamID

    // Error/Status messages
    char statusMessage[256];
    std::atomic<int> showStatus{0};
};

class OverlayManager {
public:
    OverlayManager() = default;
    ~OverlayManager() { Shutdown(); }

    bool Initialize(u64 localSteamID);
    void Shutdown();
    void Update();
    void Render();

    // State setters (called from network/game hooks)
    void SetConnectionState(int state);
    void SetHostSteamID(u64 id);
    void SetClientSteamID(u64 id);
    void SetLocalSteamID(u64 id);
    void SetGameSpeed(int speed);
    void SetPaused(bool paused);
    void SetGameTime(long long timeMs);
    void SetResource(int index, int amount);
    void AddChatMessage(const char* sender, const char* message);
    void AddTradeOffer(u32 offerID, u64 from, u64 to, int resource, int amount);
    void UpdateTradeOffer(u32 offerID, int status);
    void SetStatusMessage(const char* msg);

    // Input handling
    void ActivateInput(int type);
    void DeactivateInput();
    bool IsInputActive() const;
    const char* GetInputText() const;

    // UI visibility
    void ShowTradeUI(bool show);
    void ShowChatUI(bool show);
    void ShowSaveUI(bool show);

    OverlayControl* GetControl() { return control_; }

private:
    void CreateSharedMemory();
    void CreateOverlayWindow();
    void DestroyOverlayWindow();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void HandleInput();

    OverlayControl* control_ = nullptr;
    HANDLE mapping_ = nullptr;
    HWND overlayWindow_ = nullptr;
    HINSTANCE instance_ = nullptr;
    u64 localSteamID_ = 0;
    bool initialized_ = false;
};

} // namespace FrostMP::Overlay