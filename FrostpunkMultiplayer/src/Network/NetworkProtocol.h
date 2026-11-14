#pragma once
#include "Common.h"

namespace FrostMP::Network {

inline constexpr u32 PROTOCOL_MAGIC = 0x464D5046; // "FPMF" - FrostPunk Multiplayer Framework
inline constexpr u16 PROTOCOL_VERSION = 1;
inline constexpr u32 MAX_PACKET_SIZE = 64 * 1024;
inline constexpr int STEAM_NETWORKING_CHANNEL = 1;

enum class PacketType : u16 {
    // Connection
    Hello = 1,
    HelloAck = 2,
    Heartbeat = 3,
    Disconnect = 4,

    // Game state sync
    GameState = 10,
    GameStateAck = 11,
    GameStateDelta = 12,

    // Player actions
    PlayerAction = 20,
    PlayerActionAck = 21,

    // Resources
    ResourceSync = 30,
    ResourceTransfer = 31,
    ResourceTransferResult = 32,

    // Time/Pause/Speed
    TimeSync = 40,
    PauseState = 41,
    SpeedState = 42,

    // Chat/Save
    ChatMessage = 50,
    SaveRequest = 51,
    SaveData = 52,
    LoadRequest = 53,
    LoadResponse = 54,

    // Map/Scenario
    MapSelect = 60,
    MapSelected = 61,
    GameStart = 62,
    GameStarted = 63,
};

#pragma pack(push, 1)

struct PacketHeader {
    u32 magic = PROTOCOL_MAGIC;
    u16 version = PROTOCOL_VERSION;
    PacketType type = PacketType::Hello;
    u32 sequence = 0;
    u32 payloadSize = 0;
    u64 senderSteamID = 0;
    u64 timestampMs = 0;
};

struct HelloPayload {
    char playerName[64] = {};
    char cityName[64] = {};
    u64 sessionStartedAtMs = 0;
    u8 isHost = 0; // 0=false, 1=true
};

struct HeartbeatPayload {
    u64 uptimeMs = 0;
    u32 localSequence = 0;
    u32 remoteSequence = 0;
};

struct DisconnectPayload {
    char reason[128] = {};
};

struct GameStatePayload {
    // Map/Scenario info
    i32 mapIndex = -1;
    i32 scenarioType = 0; // 0=Endless, 1=Story
    i32 difficulty = 0;
    u32 randomSeed = 0;

    // Time
    i64 gameTimeMs = 0;
    u8 isPaused = 0; // 0=false, 1=true (use u8 for consistent packing)
    i32 gameSpeed = 0; // 0=1x, 1=3x, 2=12x

    // Resources (host only sends full state)
    i32 coal = 0;
    i32 wood = 0;
    i32 steel = 0;
    i32 steamCores = 0;
    i32 rawFood = 0;
    i32 foodRations = 0;
    i32 population = 0;
    i32 temperature = 0;
    i32 hope = -1; // -1 = not synced
    i32 discontent = -1;

    // Player info
    u64 hostSteamID = 0;
    u64 clientSteamID = 0;
};

struct GameStateDeltaPayload {
    u64 gameTimeMs = 0;
    u8 isPaused = 0; // 0=false, 1=true
    i32 gameSpeed = -1; // -1 = no change

    // Optional resource deltas
    u8 hasResources = 0; // 0=false, 1=true
    i32 coal = 0;
    i32 wood = 0;
    i32 steel = 0;
    i32 steamCores = 0;
    i32 rawFood = 0;
    i32 foodRations = 0;
    i32 population = 0;
    i32 temperature = 0;
    i32 hope = -1;
    i32 discontent = -1;
};

struct PlayerActionPayload {
    enum class ActionType : u8 {
        Move = 1,
        Build = 2,
        Interact = 3,
        Select = 4,
        Cancel = 5,
    } actionType = ActionType::Move;

    u32 entityID = 0;
    float x = 0, y = 0, z = 0;
    float rotX = 0, rotY = 0, rotZ = 0, rotW = 1;
    u32 targetEntityID = 0;
    char extraData[64] = {};
};

struct ResourceTransferPayload {
    u32 transferID = 0;
    enum class ResourceType : u8 {
        Coal = 0, Wood = 1, Steel = 2, SteamCores = 3, RawFood = 4, FoodRations = 5
    } resource = ResourceType::Coal;
    i32 amount = 0;
    u64 fromPlayer = 0;
    u64 toPlayer = 0;
};

struct ResourceTransferResultPayload {
    u32 transferID = 0;
    enum class Result : u8 {
        Success = 0,
        InsufficientResources = 1,
        InvalidResource = 2,
        NotConnected = 3,
        AlreadyProcessing = 4,
    } result = Result::Success;
    i32 appliedAmount = 0;
};

struct TimeSyncPayload {
    u64 hostGameTimeMs = 0;
    u64 hostRealTimeMs = 0;
    u32 sequence = 0;
};

struct PauseStatePayload {
    u32 requestID = 0;
    u8 paused = 1; // 0=false, 1=true
    u32 delayMs = 0;
};

struct SpeedStatePayload {
    u32 revision = 0;
    i32 speedMode = 0; // 0=1x, 1=3x, 2=12x
};

struct ChatMessagePayload {
    char senderName[64] = {};
    char message[512] = {};
    u64 timestampMs = 0;
};

struct SaveRequestPayload {
    char saveName[128] = {};
    u8 isMultiplayerSave = 1; // 0=false, 1=true
};

struct LoadRequestPayload {
    char saveName[128] = {};
};

struct SaveDataPayload {
    char saveName[128] = {};
    u32 dataSize = 0;
    u32 dataCRC32 = 0;
    // Data follows immediately after this struct
};

struct MapSelectPayload {
    i32 mapIndex = -1;
    i32 scenarioType = 0;
    i32 difficulty = 0;
    u32 randomSeed = 0;
};

struct MapSelectedPayload {
    i32 mapIndex = -1;
    i32 scenarioType = 0;
    i32 difficulty = 0;
    u32 randomSeed = 0;
    bool confirmed = false;
};

struct GameStartPayload {
    u32 startToken = 0;
};

struct GameStartedPayload {
    u32 startToken = 0;
    u8 success = 1; // 0=false, 1=true
    char errorMessage[128] = {};
};

struct LoadResponsePayload {
    char saveName[128] = {};
    u8 success = 0; // 0=false, 1=true
    char errorMessage[128] = {};
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 32, "PacketHeader must be 32 bytes");
static_assert(sizeof(HelloPayload) == 137, "HelloPayload size mismatch");
static_assert(sizeof(GameStatePayload) == 85, "GameStatePayload size mismatch");

inline bool ValidateHeader(const PacketHeader& header) {
    return header.magic == PROTOCOL_MAGIC && header.version == PROTOCOL_VERSION;
}

inline size_t GetPacketSize(const PacketHeader& header) {
    return sizeof(PacketHeader) + header.payloadSize;
}

} // namespace FrostMP::Network