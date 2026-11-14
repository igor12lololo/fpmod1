#include "NetworkManager.h"
#include <iostream>

namespace FrostMP::Network {

NetworkManager::NetworkManager() {
    steam = std::make_unique<SteamNetworkingSocketsImpl>();
}

NetworkManager::~NetworkManager() {
    Shutdown();
}

bool NetworkManager::Initialize(u64 localSteamID_) {
    localSteamID = localSteamID_;
    return steam->Initialize(localSteamID_);
}

void NetworkManager::Shutdown() {
    if (state != State::Disconnected) {
        Disconnect("Shutdown");
    }
    steam->Shutdown();
    role = Role::None;
    state = State::Disconnected;
    hostSteamID = 0;
    clientSteamID = 0;
    {
        std::lock_guard<std::mutex> lock(packetMutex);
        while (!incomingQueue.empty()) incomingQueue.pop();
    }
}

bool NetworkManager::StartHost() {
    if (state != State::Disconnected) return false;

    role = Role::Host;
    hostSteamID = localSteamID;
    SetState(State::Connecting);

    if (!steam->StartListening()) {
        SetState(State::Disconnected);
        return false;
    }

    return true;
}

bool NetworkManager::ConnectToHost(u64 hostSteamID_) {
    if (state != State::Disconnected) return false;

    role = Role::Client;
    hostSteamID = hostSteamID_;
    SetState(State::Connecting);

    if (!steam->Connect(hostSteamID_)) {
        SetState(State::Disconnected);
        return false;
    }

    // Send initial hello
    SendHello(hostSteamID_);

    return true;
}

void NetworkManager::Disconnect(const char* reason) {
    if (state == State::Disconnected) return;

    if (role == Role::Host) {
        // Disconnect all clients
        // In a real implementation, iterate through connected clients
    } else if (role == Role::Client && hostSteamID) {
        steam->Disconnect(hostSteamID, reason);
    }

    steam->StopListening();
    SetState(State::Disconnected);
}

void NetworkManager::Poll() {
    steam->Poll();

    // Receive and process all incoming messages
    steam->ReceiveMessages([this](u64 senderSteamID, const void* data, size_t size) {
        ProcessPacket(senderSteamID, data, size);
    });

    HandleIncomingPackets();

    // Send periodic heartbeats
    u64 now = GetTimestampMs();
    if (now - lastHeartbeatSent.load() > 5000) { // Every 5 seconds
        if (state == State::Connected || state == State::Syncing || state == State::InGame) {
            HeartbeatPayload hb{};
            hb.uptimeMs = now;
            hb.localSequence = localSequence.load();
            hb.remoteSequence = remoteSequence.load();

            if (role == Role::Host) {
                SendToAll(&hb, sizeof(hb), false);
            } else if (hostSteamID) {
                SendTo(hostSteamID, &hb, sizeof(hb), false);
            }
            lastHeartbeatSent.store(now);
        }
    }

    // Check for timeout
    if (state != State::Disconnected && state != State::Connecting) {
        if (now - lastHeartbeatReceived.load() > 15000) { // 15 second timeout
            Disconnect("Connection timeout");
        }
    }
}

void NetworkManager::HandleIncomingPackets() {
    std::vector<std::tuple<u64, std::vector<u8>>> packets;
    {
        std::lock_guard<std::mutex> lock(packetMutex);
        while (!incomingQueue.empty()) {
            packets.push_back(std::move(incomingQueue.front()));
            incomingQueue.pop();
        }
    }

    for (auto& [senderID, data] : packets) {
        if (data.size() < sizeof(PacketHeader)) continue;

        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(data.data());
        if (!ValidateHeader(*header)) continue;

        const void* payload = data.data() + sizeof(PacketHeader);
        size_t payloadSize = header->payloadSize;

        // Update remote sequence
        remoteSequence.store(std::max(remoteSequence.load(), header->sequence + 1));

        // Handle packet
        auto it = handlers.find(header->type);
        if (it != handlers.end()) {
            it->second(senderID, *header, payload);
        }

        // Built-in handlers
        switch (header->type) {
            case PacketType::Hello:
                HandleHello(senderID, static_cast<const HelloPayload*>(payload));
                break;
            case PacketType::HelloAck:
                HandleHelloAck(senderID, payload);
                break;
            case PacketType::Heartbeat:
                HandleHeartbeat(senderID, static_cast<const HeartbeatPayload*>(payload));
                break;
            case PacketType::Disconnect:
                HandleDisconnect(senderID, static_cast<const DisconnectPayload*>(payload));
                break;
        }
    }
}

void NetworkManager::ProcessPacket(u64 senderSteamID, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(packetMutex);
    incomingQueue.emplace(senderSteamID, std::vector<u8>(static_cast<const u8*>(data), static_cast<const u8*>(data) + size));
}

void NetworkManager::SendToAll(const void* data, size_t size, bool reliable) {
    // In a real implementation, iterate through all connected peers
    // For now, this is a placeholder for the host sending to client
    if (role == Role::Host && clientSteamID) {
        // Assume data is already a complete packet with header
        steam->Send(clientSteamID, data, size, reliable);
        localSequence.fetch_add(1);
    }
}

void NetworkManager::SendTo(u64 steamID, const void* data, size_t size, bool reliable) {
    // Assume data is already a complete packet with header
    if (steam->Send(steamID, data, size, reliable)) {
        localSequence.fetch_add(1);
    }
}

void NetworkManager::SendPacket(u64 steamID, PacketType type, const void* payload, size_t payloadSize, bool reliable) {
    PacketHeader header{};
    header.type = type;
    header.payloadSize = static_cast<u32>(payloadSize);
    header.senderSteamID = localSteamID;
    header.timestampMs = GetTimestampMs();
    header.sequence = localSequence.load();

    std::vector<u8> buffer(sizeof(PacketHeader) + payloadSize);
    std::memcpy(buffer.data(), &header, sizeof(PacketHeader));
    if (payload && payloadSize > 0) {
        std::memcpy(buffer.data() + sizeof(PacketHeader), payload, payloadSize);
    }

    steam->Send(steamID, buffer.data(), buffer.size(), reliable);
    localSequence.fetch_add(1);
}

void NetworkManager::SendHello(u64 steamID) {
    HelloPayload hello{};
    // Fill player name from Steam or use default
    std::strncpy(hello.playerName, "Player", sizeof(hello.playerName) - 1);
    std::strncpy(hello.cityName, "New City", sizeof(hello.cityName) - 1);
    hello.sessionStartedAtMs = GetTimestampMs();
    hello.isHost = (role == Role::Host);

    SendPacket(steamID, PacketType::Hello, &hello, sizeof(hello), true);
}

void NetworkManager::HandleHello(u64 senderSteamID, const HelloPayload* payload) {
    if (!payload) return;

    if (role == Role::Host) {
        if (clientSteamID == 0) {
            clientSteamID = senderSteamID;
            // Send hello ack with host info
            HelloPayload ack{};
            std::strncpy(ack.playerName, "Host", sizeof(ack.playerName) - 1);
            std::strncpy(ack.cityName, "Host City", sizeof(ack.cityName) - 1);
            ack.sessionStartedAtMs = GetTimestampMs();
            ack.isHost = true;
            SendPacket(senderSteamID, PacketType::HelloAck, &ack, sizeof(ack), true);
            SetState(State::Connected);
        } else {
            // Already have a client, reject
            DisconnectPayload dc{};
            std::strncpy(dc.reason, "Server full", sizeof(dc.reason) - 1);
            SendPacket(senderSteamID, PacketType::Disconnect, &dc, sizeof(dc), true);
            steam->Disconnect(senderSteamID, "Server full");
        }
    } else if (role == Role::Client) {
        // Received hello from host
        hostSteamID = senderSteamID;
        SendPacket(senderSteamID, PacketType::HelloAck, nullptr, 0, true);
        SetState(State::Connected);
    }
}

void NetworkManager::HandleHelloAck(u64 senderSteamID, const void* payload) {
    lastHeartbeatReceived.store(GetTimestampMs());
    if (state == State::Connecting) {
        SetState(State::Connected);
    }
}

// Called by Mod when host sends initial game state to client
void NetworkManager::OnInitialGameStateSent() {
    if (role == Role::Host && state == State::Connected) {
        SetState(State::Syncing);
    }
}

// Called by Mod when client receives and applies initial game state
void NetworkManager::OnInitialGameStateReceived() {
    if (role == Role::Client && state == State::Syncing) {
        SetState(State::InGame);
    }
}

// Called by Mod when host confirms client is in game
void NetworkManager::OnClientInGame() {
    if (role == Role::Host && state == State::Syncing) {
        SetState(State::InGame);
    }
}

void NetworkManager::HandleHeartbeat(u64 senderSteamID, const HeartbeatPayload* payload) {
    lastHeartbeatReceived.store(GetTimestampMs());
}

void NetworkManager::HandleDisconnect(u64 senderSteamID, const DisconnectPayload* payload) {
    if (role == Role::Host && senderSteamID == clientSteamID) {
        clientSteamID = 0;
        SetState(State::Connected); // Back to waiting for client
    } else if (role == Role::Client && senderSteamID == hostSteamID) {
        Disconnect(payload ? payload->reason : "Host disconnected");
    }
}

void NetworkManager::RegisterHandler(PacketType type, PacketHandler handler) {
    handlers[type] = std::move(handler);
}

void NetworkManager::SetStateChangeHandler(StateChangeHandler handler) {
    stateChangeHandler = std::move(handler);
}

void NetworkManager::SendChatMessage(const char* message) {
    if (!message || state != State::InGame) return;

    ChatMessagePayload chat{};
    // Get local player name - would need Steam API, using placeholder
    if (role == Role::Host) {
        std::strncpy(chat.senderName, "Host", sizeof(chat.senderName) - 1);
    } else {
        std::strncpy(chat.senderName, "Client", sizeof(chat.senderName) - 1);
    }
    std::strncpy(chat.message, message, sizeof(chat.message) - 1);
    chat.timestampMs = GetTimestampMs();

    if (role == Role::Host) {
        SendToAll(&chat, sizeof(chat), true);
    } else if (hostSteamID) {
        SendTo(hostSteamID, &chat, sizeof(chat), true);
    }
}

void NetworkManager::SetState(State newState) {
    if (state == newState) return;
    State oldState = state;
    state = newState;
    if (stateChangeHandler) {
        stateChangeHandler(oldState, newState);
    }
}

} // namespace FrostMP::Network