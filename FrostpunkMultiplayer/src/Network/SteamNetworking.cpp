#include "SteamNetworking.h"
#include <future>
#include <chrono>

namespace FrostMP::Network {

// SteamNetworkingMessage interface vtable indices
// Based on ISteamNetworkingMessage from SteamNetworkingSockets
// 0 = QueryInterface, 1 = AddRef, 2 = Release, 3 = GetData, 4 = GetSize, etc.

bool SteamNetworkingSocketsImpl::SteamAPI::Load() {
    // We're injecting into a Steam game - SteamAPI is already initialized by the game
    // Try to get ISteamNetworkingSockets through Steam's internal interface system

    wchar_t gamePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, gamePath, MAX_PATH)) {
        DebugLog("SteamNetworking: Failed to get module filename");
        return false;
    }

    std::filesystem::path gameDir = std::filesystem::path(gamePath).parent_path();

    // The game already loaded steam_api64.dll and steamnetworkingsockets.dll
    // Get module handles
    HMODULE steamApiModule = GetModuleHandleW(L"steam_api64.dll");
    HMODULE socketsModule = GetModuleHandleW(L"steamnetworkingsockets.dll");

    if (!steamApiModule) {
        steamApiModule = LoadLibraryW(L"steam_api64.dll");
        DebugLog("SteamNetworking: Loaded steam_api64.dll: %p", steamApiModule);
    }

    if (!socketsModule) {
        std::filesystem::path socketsDll = gameDir / L"steamnetworkingsockets.dll";
        if (std::filesystem::exists(socketsDll)) {
            socketsModule = LoadLibraryW(socketsDll.c_str());
        } else {
            socketsModule = LoadLibraryW(L"steamnetworkingsockets.dll");
        }
        DebugLog("SteamNetworking: Loaded steamnetworkingsockets.dll: %p", socketsModule);
    }

    if (!socketsModule) {
        DebugLog("SteamNetworking: Failed to load steamnetworkingsockets.dll, error=%lu", GetLastError());
        return false;
    }

    // Try to get the interface through SteamInternal_CreateInterface if available
    // This is the standard way Steam components get interfaces
    using SteamInternal_CreateInterface_t = void* (*)(const char*, int*);
    auto steamInternalCreateInterface = reinterpret_cast<SteamInternal_CreateInterface_t>(
        GetProcAddress(steamApiModule, "SteamInternal_CreateInterface"));
    DebugLog("SteamNetworking: SteamInternal_CreateInterface = %p", steamInternalCreateInterface);

    if (steamInternalCreateInterface) {
        int ret = 0;
        // Try to create ISteamNetworkingSockets interface
        steamNetworkingSockets = steamInternalCreateInterface("SteamNetworkingSockets012", &ret);
        DebugLog("SteamNetworking: SteamInternal_CreateInterface(SteamNetworkingSockets012) returned %p (ret=%d)", steamNetworkingSockets, ret);
    }

    // If that didn't work, try the older version
    if (!steamNetworkingSockets && steamInternalCreateInterface) {
        int ret = 0;
        steamNetworkingSockets = steamInternalCreateInterface("SteamNetworkingSockets011", &ret);
        DebugLog("SteamNetworking: SteamInternal_CreateInterface(SteamNetworkingSockets011) returned %p (ret=%d)", steamNetworkingSockets, ret);
    }

    // If still not working, try SteamNetworkingSockets_LibV12 from the loaded library
    if (!steamNetworkingSockets) {
        using SteamNetworkingSockets_LibV12_t = void* (*)();
        auto getLibV12 = reinterpret_cast<SteamNetworkingSockets_LibV12_t>(GetProcAddress(socketsModule, "SteamNetworkingSockets_LibV12"));
        DebugLog("SteamNetworking: SteamNetworkingSockets_LibV12 = %p", getLibV12);

        if (getLibV12) {
            // Try setting environment variables first
            using SteamNetworkingSockets_SetEnvironmentVariable_t = void (*)(const char*, const char*);
            auto setEnvVar = reinterpret_cast<SteamNetworkingSockets_SetEnvironmentVariable_t>(
                GetProcAddress(socketsModule, "SteamNetworkingSockets_SetEnvironmentVariable"));
            DebugLog("SteamNetworking: SteamNetworkingSockets_SetEnvironmentVariable = %p", setEnvVar);

            if (setEnvVar) {
                setEnvVar("SteamNetworkingSockets_AllowP2P", "1");
                setEnvVar("SteamNetworkingSockets_P2P_AllowAny", "1");
                setEnvVar("SteamNetworkingSockets_EnableSteamDatagram", "1");
                DebugLog("SteamNetworking: Set P2P environment variables");
            }

            // Set service thread callback
            using SteamNetworkingSockets_SetServiceThreadInitCallback_t = void (*)(void (*)(void*));
            auto setServiceThreadCallback = reinterpret_cast<SteamNetworkingSockets_SetServiceThreadInitCallback_t>(
                GetProcAddress(socketsModule, "SteamNetworkingSockets_SetServiceThreadInitCallback"));
            DebugLog("SteamNetworking: SteamNetworkingSockets_SetServiceThreadInitCallback = %p", setServiceThreadCallback);

            if (setServiceThreadCallback) {
                setServiceThreadCallback([](void*) {});
                DebugLog("SteamNetworking: Set service thread init callback");
            }

            steamNetworkingSockets = getLibV12();
            DebugLog("SteamNetworking: SteamNetworkingSockets_LibV12() returned %p", steamNetworkingSockets);
        }
    }

    // If still not working, try to get interface via SteamAPI (game already initialized Steam)
    if (!steamNetworkingSockets) {
        DebugLog("SteamNetworking: Trying SteamAPI_GetISteamNetworkingSockets (game already initialized Steam)...");
        using SteamAPI_GetISteamNetworkingSockets_t = void* (*)(void*, void*, const char*);
        auto getISteamNetworkingSockets = reinterpret_cast<SteamAPI_GetISteamNetworkingSockets_t>(
            GetProcAddress(steamApiModule, "SteamAPI_GetISteamNetworkingSockets"));
        DebugLog("SteamNetworking: SteamAPI_GetISteamNetworkingSockets = %p", getISteamNetworkingSockets);

        if (getISteamNetworkingSockets && steamApiModule) {
            // Get SteamAPI context from game's initialized Steam
            using SteamAPI_GetHSteamUser_t = int (*)();
            using SteamAPI_GetHSteamPipe_t = int (*)();
            auto getHSteamUser = reinterpret_cast<SteamAPI_GetHSteamUser_t>(GetProcAddress(steamApiModule, "SteamAPI_GetHSteamUser"));
            auto getHSteamPipe = reinterpret_cast<SteamAPI_GetHSteamPipe_t>(GetProcAddress(steamApiModule, "SteamAPI_GetHSteamPipe"));

            if (getHSteamUser && getHSteamPipe) {
                int hUser = getHSteamUser();
                int hPipe = getHSteamPipe();
                DebugLog("SteamNetworking: hUser=%d, hPipe=%d", hUser, hPipe);

                if (hUser > 0 && hPipe > 0) {
                    steamNetworkingSockets = getISteamNetworkingSockets((void*)hUser, (void*)hPipe, "SteamNetworkingSockets012");
                    DebugLog("SteamNetworking: SteamAPI_GetISteamNetworkingSockets returned %p", steamNetworkingSockets);
                }
            }
        }
    }

    // Only try SteamDatagramClient_Init as last resort (may hang)
    if (!steamNetworkingSockets) {
        using SteamDatagramClient_Init_t = bool (*)();
        auto datagramInit = reinterpret_cast<SteamDatagramClient_Init_t>(GetProcAddress(socketsModule, "SteamDatagramClient_Init"));
        DebugLog("SteamNetworking: SteamDatagramClient_Init = %p (trying as last resort)", datagramInit);

        if (datagramInit) {
            DebugLog("SteamNetworking: Calling SteamDatagramClient_Init() with timeout...");
            // Run with timeout to avoid hanging
            std::future<bool> initFuture = std::async(std::launch::async, datagramInit);
            if (initFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready) {
                bool initResult = initFuture.get();
                DebugLog("SteamNetworking: SteamDatagramClient_Init() returned %s", initResult ? "true" : "false");
                if (initResult) {
                    using SteamNetworkingSockets_LibV12_t = void* (*)();
                    auto getLibV12 = reinterpret_cast<SteamNetworkingSockets_LibV12_t>(GetProcAddress(socketsModule, "SteamNetworkingSockets_LibV12"));
                    if (getLibV12) {
                        steamNetworkingSockets = getLibV12();
                        DebugLog("SteamNetworking: SteamNetworkingSockets_LibV12() after init returned %p", steamNetworkingSockets);
                    }
                }
            } else {
                DebugLog("SteamNetworking: SteamDatagramClient_Init() timed out after 3s");
            }
        } else {
            DebugLog("SteamNetworking: SteamDatagramClient_Init not found");
        }
    }

    if (!steamNetworkingSockets) {
        DebugLog("SteamNetworking: Failed to get ISteamNetworkingSockets interface - trying SteamAPI_GetISteamNetworkingSockets");
        // Try the standard SteamAPI method
        using SteamAPI_GetISteamNetworkingSockets_t = void* (*)(void*, void*, const char*);
        auto getISteamNetworkingSockets = reinterpret_cast<SteamAPI_GetISteamNetworkingSockets_t>(
            GetProcAddress(steamApiModule, "SteamAPI_GetISteamNetworkingSockets"));
        DebugLog("SteamNetworking: SteamAPI_GetISteamNetworkingSockets = %p", getISteamNetworkingSockets);

        if (getISteamNetworkingSockets && steamApiModule) {
            // Get SteamAPI context
            using SteamAPI_GetHSteamUser_t = int (*)();
            using SteamAPI_GetHSteamPipe_t = int (*)();
            auto getHSteamUser = reinterpret_cast<SteamAPI_GetHSteamUser_t>(GetProcAddress(steamApiModule, "SteamAPI_GetHSteamUser"));
            auto getHSteamPipe = reinterpret_cast<SteamAPI_GetHSteamPipe_t>(GetProcAddress(steamApiModule, "SteamAPI_GetHSteamPipe"));

            if (getHSteamUser && getHSteamPipe) {
                int hUser = getHSteamUser();
                int hPipe = getHSteamPipe();
                DebugLog("SteamNetworking: hUser=%d, hPipe=%d", hUser, hPipe);

                steamNetworkingSockets = getISteamNetworkingSockets((void*)hUser, (void*)hPipe, "SteamNetworkingSockets012");
                DebugLog("SteamNetworking: SteamAPI_GetISteamNetworkingSockets returned %p", steamNetworkingSockets);
            }
        }
    }

    if (!steamNetworkingSockets) {
        DebugLog("SteamNetworking: ALL METHODS FAILED to get ISteamNetworkingSockets interface");
        return false;
    }

    // Get function pointers from the ISteamNetworkingSockets interface
    void** vtable = *static_cast<void***>(steamNetworkingSockets);
    if (!vtable) {
        DebugLog("SteamNetworking: ISteamNetworkingSockets vtable is null");
        return false;
    }

    // VTable indices based on SteamNetworkingSockets interface
    // 0 = QueryInterface, 1 = AddRef, 2 = Release
    // 3 = CreateListenSocketP2P
    // 4 = CreateP2PConnectionSocket
    // 5 = AcceptConnection
    // 6 = CloseConnection
    // 7 = CloseListenSocket
    // 8 = SendMessageToConnection
    // 9 = ReceiveMessagesOnConnection
    // 10 = GetConnectionInfo
    // 11 = SetConnectionUserData
    // 12 = GetConnectionUserData
    // (Exact indices may vary by Steam SDK version)

    CreateListenSocketP2P = reinterpret_cast<CreateListenSocketP2P_t>(vtable[3]);
    DebugLog("SteamNetworking: CreateListenSocketP2P = %p", CreateListenSocketP2P);
    CreateP2PConnectionSocket = reinterpret_cast<CreateP2PConnectionSocket_t>(vtable[4]);
    DebugLog("SteamNetworking: CreateP2PConnectionSocket = %p", CreateP2PConnectionSocket);
    AcceptConnection = reinterpret_cast<AcceptConnection_t>(vtable[5]);
    DebugLog("SteamNetworking: AcceptConnection = %p", AcceptConnection);
    CloseConnection = reinterpret_cast<CloseConnection_t>(vtable[6]);
    DebugLog("SteamNetworking: CloseConnection = %p", CloseConnection);
    CloseListenSocket = reinterpret_cast<CloseListenSocket_t>(vtable[7]);
    DebugLog("SteamNetworking: CloseListenSocket = %p", CloseListenSocket);
    SendMessageToConnection = reinterpret_cast<SendMessageToConnection_t>(vtable[8]);
    DebugLog("SteamNetworking: SendMessageToConnection = %p", SendMessageToConnection);
    ReceiveMessagesOnConnection = reinterpret_cast<ReceiveMessagesOnConnection_t>(vtable[9]);
    DebugLog("SteamNetworking: ReceiveMessagesOnConnection = %p", ReceiveMessagesOnConnection);
    GetConnectionInfo = reinterpret_cast<GetConnectionInfo_t>(vtable[10]);
    DebugLog("SteamNetworking: GetConnectionInfo = %p", GetConnectionInfo);
    SetConnectionUserData = reinterpret_cast<SetConnectionUserData_t>(vtable[11]);
    DebugLog("SteamNetworking: SetConnectionUserData = %p", SetConnectionUserData);
    GetConnectionUserData = reinterpret_cast<GetConnectionUserData_t>(vtable[12]);
    DebugLog("SteamNetworking: GetConnectionUserData = %p", GetConnectionUserData);

    // Also get RunCallbacks from steam_api64.dll since game already initialized it
    if (steamApiModule) {
        RunCallbacks = reinterpret_cast<SteamAPI_RunCallbacks_t>(GetProcAddress(steamApiModule, "SteamAPI_RunCallbacks"));
        DebugLog("SteamNetworking: SteamAPI_RunCallbacks from game = %p", RunCallbacks);
    }

    if (!CreateListenSocketP2P || !CreateP2PConnectionSocket || !AcceptConnection ||
        !CloseConnection || !CloseListenSocket || !SendMessageToConnection ||
        !ReceiveMessagesOnConnection || !GetConnectionInfo) {
        DebugLog("SteamNetworking: Some vtable functions are null - Steam API version mismatch");
        return false;
    }

    DebugLog("SteamNetworking: SteamAPI::Load() success");
    return true;
}

bool SteamNetworkingSocketsImpl::Initialize(u64 localSteamID_) {
    DebugLog("SteamNetworking: Initialize called, localSteamID=%llu", localSteamID_);
    if (localSteamID_ == 0) {
        DebugLog("SteamNetworking: localSteamID is 0, skipping network init (offline mode)");
        localSteamID = 0;
        return true;
    }
    if (!steam.Load()) {
        DebugLog("SteamNetworking: steam.Load() failed");
        return false;
    }
    DebugLog("SteamNetworking: steam.Load() OK");
    // Don't call steam.Init() - game already initialized SteamAPI

    localSteamID = localSteamID_;
    DebugLog("SteamNetworking: Initialize success");
    return true;
}

void SteamNetworkingSocketsImpl::Shutdown() {
    std::lock_guard<std::mutex> lock(connMutex);

    for (auto& [id, conn] : connections) {
        if (conn.connection && steam.CloseConnection) {
            steam.CloseConnection(conn.connection, 0, "Shutdown", true);
        }
    }
    connections.clear();

    if (listenSocket.socket && steam.CloseListenSocket) {
        steam.CloseListenSocket(listenSocket.socket);
        listenSocket.socket = nullptr;
    }

    // Don't call steam.Shutdown() - game manages SteamAPI lifecycle
    localSteamID = 0;
    isHostMode = false;
}

bool SteamNetworkingSocketsImpl::Connect(u64 remoteSteamID) {
    if (!steam.CreateP2PConnectionSocket) return false;

    std::lock_guard<std::mutex> lock(connMutex);

    auto it = connections.find(remoteSteamID);
    if (it != connections.end()) {
        return it->second.isConnected;
    }

    void* conn = steam.CreateP2PConnectionSocket(remoteSteamID, 0, STEAM_NETWORKING_CHANNEL, nullptr);
    if (!conn) return false;

    ConnectionInfo info;
    info.connection = conn;
    info.steamID = remoteSteamID;
    info.isOutgoing = true;
    info.isConnected = false;
    connections[remoteSteamID] = info;

    if (steam.SetConnectionUserData) {
        steam.SetConnectionUserData(conn, static_cast<int64_t>(remoteSteamID));
    }

    return true;
}

void SteamNetworkingSocketsImpl::Disconnect(u64 remoteSteamID, const char* reason) {
    std::lock_guard<std::mutex> lock(connMutex);

    auto it = connections.find(remoteSteamID);
    if (it != connections.end() && it->second.connection) {
        steam.CloseConnection(it->second.connection, 0, reason ? reason : "Disconnected", true);
        connections.erase(it);
    }
}

bool SteamNetworkingSocketsImpl::Send(u64 remoteSteamID, const void* data, size_t size, bool reliable) {
    if (!steam.SendMessageToConnection) return false;

    std::lock_guard<std::mutex> lock(connMutex);
    auto it = connections.find(remoteSteamID);
    if (it == connections.end() || !it->second.isConnected) return false;

    int flags = reliable ? 2 : 0; // k_nSteamNetworkingSend_Reliable = 2
    int64_t msgNum = 0;
    int result = steam.SendMessageToConnection(it->second.connection, const_cast<void*>(data), static_cast<u32>(size), flags, &msgNum);
    return result == 0; // k_EResultOK = 0
}

void SteamNetworkingSocketsImpl::ReceiveMessages(const std::function<void(u64 remoteSteamID, const void* data, size_t size)>& handler) {
    if (!steam.ReceiveMessagesOnConnection || !handler) return;

    // Copy connections to avoid holding lock during callback
    std::vector<std::pair<u64, void*>> activeConnections;
    {
        std::lock_guard<std::mutex> lock(connMutex);
        for (auto& [id, conn] : connections) {
            if (conn.connection && conn.isConnected) {
                activeConnections.emplace_back(conn.steamID, conn.connection);
            }
        }
    }

    for (auto& [steamID, connection] : activeConnections) {
        // ISteamNetworkingMessage* array
        void* messages[32];
        int count = steam.ReceiveMessagesOnConnection(connection, messages, 32);
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                void* msgPtr = messages[i];
                if (!msgPtr) continue;

                // Get vtable for ISteamNetworkingMessage
                void** vtable = *static_cast<void***>(msgPtr);
                if (!vtable) continue;

                // GetData = vtable[3], GetSize = vtable[4], Release = vtable[2]
                using GetDataFn = const void*(*)(void*);
                using GetSizeFn = int32_t(*)(void*);
                using ReleaseFn = void(*)(void*);

                GetDataFn getData = reinterpret_cast<GetDataFn>(vtable[3]);
                GetSizeFn getSize = reinterpret_cast<GetSizeFn>(vtable[4]);
                ReleaseFn release = reinterpret_cast<ReleaseFn>(vtable[2]);

                if (getData && getSize && release) {
                    const void* data = getData(msgPtr);
                    int32_t size = getSize(msgPtr);
                    if (data && size > 0) {
                        handler(steamID, data, static_cast<size_t>(size));
                    }
                    release(msgPtr);
                }
            }
        }
    }
}

bool SteamNetworkingSocketsImpl::IsConnected(u64 remoteSteamID) const {
    std::lock_guard<std::mutex> lock(connMutex);
    auto it = connections.find(remoteSteamID);
    return it != connections.end() && it->second.isConnected;
}

void SteamNetworkingSocketsImpl::Poll() {
    if (steam.RunCallbacks) steam.RunCallbacks();

    std::lock_guard<std::mutex> lock(connMutex);
    // Check connection states
    for (auto& [id, conn] : connections) {
        if (!conn.connection || conn.isConnected) continue;

        // Check connection info
        struct SteamNetConnectionInfo {
            int state;
            int endReason;
            char endDebug[128];
            u64 identityRemote;
            int flags;
        } info;

        if (steam.GetConnectionInfo && steam.GetConnectionInfo(conn.connection, &info)) {
            // k_ESteamNetworkingConnectionState_Connecting = 1
            // k_ESteamNetworkingConnectionState_Connected = 2
            if (info.state == 2) {
                conn.isConnected = true;
            } else if (info.state >= 4) { // Closed/Problem
                conn.isConnected = false;
            }
        }
    }

    // Accept incoming connections if host
    if (isHostMode && listenSocket.socket && steam.AcceptConnection) {
        // Poll for new connections on listen socket
        // We need to call AcceptConnection in a loop
        void* newConn = nullptr;
        while ((newConn = steam.AcceptConnection(listenSocket.socket)) != nullptr) {
            // Get the remote SteamID from connection
            struct SteamNetConnectionInfo {
                int state;
                int endReason;
                char endDebug[128];
                u64 identityRemote;
                int flags;
            } connInfo;

            if (steam.GetConnectionInfo && steam.GetConnectionInfo(newConn, &connInfo)) {
                u64 remoteSteamID = connInfo.identityRemote;

                ConnectionInfo info;
                info.connection = newConn;
                info.steamID = remoteSteamID;
                info.isOutgoing = false;
                info.isConnected = true; // Accepted connections are immediately connected

                if (steam.SetConnectionUserData) {
                    steam.SetConnectionUserData(newConn, static_cast<int64_t>(remoteSteamID));
                }

                connections[remoteSteamID] = info;
            }
        }
    }
}

bool SteamNetworkingSocketsImpl::StartListening() {
    if (!steam.CreateListenSocketP2P) return false;
    listenSocket.socket = steam.CreateListenSocketP2P(STEAM_NETWORKING_CHANNEL, 0, nullptr);
    if (!listenSocket.socket) return false;
    isHostMode = true;
    return true;
}

void SteamNetworkingSocketsImpl::StopListening() {
    if (listenSocket.socket && steam.CloseListenSocket) {
        steam.CloseListenSocket(listenSocket.socket);
        listenSocket.socket = nullptr;
    }
    isHostMode = false;
}

} // namespace FrostMP::Network