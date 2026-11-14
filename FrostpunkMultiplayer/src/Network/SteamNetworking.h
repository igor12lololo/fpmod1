#pragma once
#include "Common.h"
#include "NetworkProtocol.h"

namespace FrostMP::Network {

class ISteamNetworking {
public:
    virtual ~ISteamNetworking() = default;
    virtual bool Initialize(u64 localSteamID) = 0;
    virtual void Shutdown() = 0;
    virtual bool Connect(u64 remoteSteamID) = 0;
    virtual void Disconnect(u64 remoteSteamID, const char* reason) = 0;
    virtual bool Send(u64 remoteSteamID, const void* data, size_t size, bool reliable) = 0;
    // Receive all pending messages using callback
    virtual void ReceiveMessages(const std::function<void(u64 remoteSteamID, const void* data, size_t size)>& handler) = 0;
    virtual bool IsConnected(u64 remoteSteamID) const = 0;
    virtual bool IsHost() const = 0;
    virtual u64 GetLocalSteamID() const = 0;
    virtual void Poll() = 0;
    virtual u32 GetNextSequence() = 0;
    virtual bool StartListening() = 0;
    virtual void StopListening() = 0;
};

class SteamNetworkingSocketsImpl : public ISteamNetworking {
    struct SteamAPI {
        using SteamAPI_Init_t = bool(*)();
        using SteamAPI_Shutdown_t = void(*)();
        using SteamAPI_RunCallbacks_t = void(*)();
        using SteamNetworkingSockets_t = void* (*)();
        using CreateListenSocketP2P_t = void* (*)(int, int, const void*);
        using CreateP2PConnectionSocket_t = void* (*)(u64, int, int, const void*);
        using AcceptConnection_t = void* (*)(void*); // Returns new connection or nullptr
        using CloseConnection_t = bool (*)(void*, int, const char*, bool);
        using SendMessageToConnection_t = int (*)(void*, const void*, u32, int, int64_t*);
        using ReceiveMessagesOnConnection_t = int (*)(void*, void**, int);
        using CloseListenSocket_t = bool (*)(void*);
        using GetConnectionInfo_t = bool (*)(void*, void*);
        using SetConnectionUserData_t = bool (*)(void*, int64_t);
        using GetConnectionUserData_t = int64_t (*)(void*);

        HMODULE module = nullptr;
        SteamAPI_Init_t Init = nullptr;
        SteamAPI_Shutdown_t Shutdown = nullptr;
        SteamAPI_RunCallbacks_t RunCallbacks = nullptr;
        SteamNetworkingSockets_t GetSteamNetworkingSockets = nullptr;
        void* steamNetworkingSockets = nullptr;

        CreateListenSocketP2P_t CreateListenSocketP2P = nullptr;
        CreateP2PConnectionSocket_t CreateP2PConnectionSocket = nullptr;
        AcceptConnection_t AcceptConnection = nullptr;
        CloseConnection_t CloseConnection = nullptr;
        SendMessageToConnection_t SendMessageToConnection = nullptr;
        ReceiveMessagesOnConnection_t ReceiveMessagesOnConnection = nullptr;
        CloseListenSocket_t CloseListenSocket = nullptr;
        GetConnectionInfo_t GetConnectionInfo = nullptr;
        SetConnectionUserData_t SetConnectionUserData = nullptr;
        GetConnectionUserData_t GetConnectionUserData = nullptr;

        bool Load();
        ~SteamAPI() { if (module) FreeLibrary(module); }
    } steam;

    struct ConnectionInfo {
        void* connection = nullptr;
        u64 steamID = 0;
        bool isOutgoing = false;
        bool isConnected = false;
    };

    struct ListenSocketInfo {
        void* socket = nullptr;
    };

    ListenSocketInfo listenSocket;
    std::unordered_map<u64, ConnectionInfo> connections;
    u64 localSteamID = 0;
    bool isHostMode = false;
    u32 nextSequence = 1;
    mutable std::mutex connMutex;

public:
    ~SteamNetworkingSocketsImpl() override { Shutdown(); }

    bool Initialize(u64 localSteamID_) override;
    void Shutdown() override;
    bool Connect(u64 remoteSteamID) override;
    void Disconnect(u64 remoteSteamID, const char* reason) override;
    bool Send(u64 remoteSteamID, const void* data, size_t size, bool reliable) override;
    void ReceiveMessages(const std::function<void(u64 remoteSteamID, const void* data, size_t size)>& handler) override;
    bool IsConnected(u64 remoteSteamID) const override;
    bool IsHost() const override { return isHostMode; }
    u64 GetLocalSteamID() const override { return localSteamID; }
    void Poll() override;
    bool StartListening() override;
    void StopListening() override;
    u32 GetNextSequence() override { return nextSequence++; }
};

} // namespace FrostMP::Network