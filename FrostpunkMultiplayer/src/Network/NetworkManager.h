#pragma once
#include "Common.h"
#include "NetworkProtocol.h"
#include "SteamNetworking.h"

namespace FrostMP::Network {

class NetworkManager {
public:
    enum class Role { None, Host, Client };
    enum class State { Disconnected, Connecting, Connected, Syncing, InGame };

    using PacketHandler = std::function<void(u64 senderSteamID, const PacketHeader&, const void* payload)>;
    using StateChangeHandler = std::function<void(State oldState, State newState)>;

    NetworkManager();
    ~NetworkManager();

    bool Initialize(u64 localSteamID);
    void Shutdown();

    bool StartHost();
    bool ConnectToHost(u64 hostSteamID);
    void Disconnect(const char* reason = "Disconnected");

    void Poll();
    void SendToAll(const void* data, size_t size, bool reliable = true);
    void SendTo(u64 steamID, const void* data, size_t size, bool reliable = true);
    void SendChatMessage(const char* message);

    void RegisterHandler(PacketType type, PacketHandler handler);
    void SetStateChangeHandler(StateChangeHandler handler);

    Role GetRole() const { return role; }
    State GetState() const { return state; }
    u64 GetHostSteamID() const { return hostSteamID; }
    u64 GetLocalSteamID() const { return localSteamID; }
    u64 GetClientSteamID() const { return clientSteamID; }
    bool IsConnected() const { return state == State::Connected || state == State::Syncing || state == State::InGame; }

    u32 GetNextSequence() { return steam->GetNextSequence(); }

    // State transition helpers
    void OnInitialGameStateSent();
    void OnInitialGameStateReceived();
    void OnClientInGame();

private:
    void HandleIncomingPackets();
    void ProcessPacket(u64 senderSteamID, const void* data, size_t size);
    void SendPacket(u64 steamID, PacketType type, const void* payload, size_t payloadSize, bool reliable);
    void SendHello(u64 steamID);
    void HandleHello(u64 senderSteamID, const HelloPayload* payload);
    void HandleHelloAck(u64 senderSteamID, const void* payload);
    void HandleHeartbeat(u64 senderSteamID, const HeartbeatPayload* payload);
    void HandleDisconnect(u64 senderSteamID, const DisconnectPayload* payload);
    void SetState(State newState);

    std::unique_ptr<ISteamNetworking> steam;
    Role role = Role::None;
    State state = State::Disconnected;
    u64 localSteamID = 0;
    u64 hostSteamID = 0;
    u64 clientSteamID = 0;

    std::unordered_map<PacketType, PacketHandler> handlers;
    StateChangeHandler stateChangeHandler;

    std::mutex packetMutex;
    std::queue<std::tuple<u64, std::vector<u8>>> incomingQueue;

    std::atomic<u64> lastHeartbeatSent{0};
    std::atomic<u64> lastHeartbeatReceived{0};
    std::atomic<u32> localSequence{1};
    std::atomic<u32> remoteSequence{1};
};

} // namespace FrostMP::Network