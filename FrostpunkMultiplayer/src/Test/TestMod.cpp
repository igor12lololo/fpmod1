// Test application for Frostpunk Multiplayer Mod
// This can be used to verify the mod components work correctly

#include "../Common/Common.h"
#include "../Common/Logger.h"
#include "../Network/NetworkProtocol.h"
#include "../Network/PacketSerializer.h"
#include "../GameHooks/GameStructs.h"

using namespace FrostMP;

int main() {
    // Initialize logger
    Logger::Instance().SetLevel(LogLevel::Debug);
    Logger::Instance().SetFile("FrostMP_Test.log");

    LOG_INFO("Starting Frostpunk Multiplayer Mod Test");

    // Test 1: Network Protocol
    {
        LOG_INFO("Test 1: Network Protocol");

        Network::GameStatePayload state{};
        state.mapIndex = 1;
        state.scenarioType = 0;
        state.difficulty = 1;
        state.randomSeed = 12345;
        state.gameTimeMs = 3600000; // 1 hour
        state.isPaused = false;
        state.gameSpeed = 1;
        state.coal = 500;
        state.wood = 300;
        state.steel = 100;
        state.steamCores = 5;
        state.rawFood = 200;
        state.foodRations = 150;
        state.population = 50;
        state.temperature = -20;
        state.hope = 75;
        state.discontent = 10;
        state.hostSteamID = 76561198000000001;
        state.clientSteamID = 76561198000000002;

        Network::PacketHeader header{};
        header.type = Network::PacketType::GameState;
        header.payloadSize = sizeof(state);
        header.senderSteamID = state.hostSteamID;
        header.timestampMs = GetTimestampMs();
        header.sequence = 1;

        auto buffer = Network::PacketSerializer::Serialize(header, &state);
        LOG_INFO("Serialized packet size: %zu bytes", buffer.size());

        Network::PacketHeader outHeader;
        const void* outPayload;
        bool ok = Network::PacketSerializer::Deserialize(buffer.data(), buffer.size(), outHeader, outPayload);
        LOG_INFO("Deserialize result: %s", ok ? "OK" : "FAILED");

        if (ok) {
            auto* outState = Network::PacketSerializer::GetPayload<Network::GameStatePayload>(outPayload);
            LOG_INFO("MapIndex: %d, Coal: %d, Speed: %d", outState->mapIndex, outState->coal, outState->gameSpeed);
        }
    }

    // Test 2: Packet Builder
    {
        LOG_INFO("Test 2: Packet Builder");

        Network::HelloPayload hello{};
        strcpy_s(hello.playerName, "TestPlayer");
        strcpy_s(hello.cityName, "New London");
        hello.sessionStartedAtMs = GetTimestampMs();
        hello.isHost = true;

        auto packet = Network::PacketBuilder(Network::PacketType::Hello, 76561198000000001, 1)
            .AddPayload(hello)
            .Build();

        LOG_INFO("Built packet size: %zu bytes", packet.size());
    }

    // Test 3: Game Structs
    {
        LOG_INFO("Test 3: Game Structs");

        Game::GameStateSnapshot snapshot{};
        snapshot.mapIndex = 2;
        snapshot.scenarioType = 0;
        snapshot.difficulty = 2;
        snapshot.randomSeed = 99999;
        snapshot.gameTimeMs = 7200000;
        snapshot.isPaused = true;
        snapshot.gameSpeed = 0;
        snapshot.resources[static_cast<int>(Game::ResourceID::Coal)] = 1000;
        snapshot.resources[static_cast<int>(Game::ResourceID::Wood)] = 800;
        snapshot.resources[static_cast<int>(Game::ResourceID::Steel)] = 300;
        snapshot.resources[static_cast<int>(Game::ResourceID::SteamCores)] = 10;
        snapshot.resources[static_cast<int>(Game::ResourceID::RawFood)] = 500;
        snapshot.resources[static_cast<int>(Game::ResourceID::FoodRations)] = 400;
        snapshot.population = 100;
        snapshot.temperature = -30;
        snapshot.hope = 85;
        snapshot.discontent = 5;
        snapshot.hostSteamID = 76561198000000001;
        snapshot.clientSteamID = 76561198000000002;
        snapshot.snapshotTime = GetTimestampMs();
        snapshot.sequence = 42;

        LOG_INFO("Snapshot: Map=%d, Time=%lldms, Coal=%d, Hope=%d%%",
                 snapshot.mapIndex, snapshot.gameTimeMs,
                 snapshot.resources[0], snapshot.hope);
    }

    // Test 4: Resource ID utilities
    {
        LOG_INFO("Test 4: Resource ID utilities");

        for (int i = 0; i < static_cast<int>(Game::ResourceID::Count); ++i) {
            auto id = static_cast<Game::ResourceID>(i);
            LOG_INFO("Resource %d: %s (telemetry: %s)",
                     i, Game::ResourceIDToName(id), Game::ResourceIDToTelemetryKey(id));
        }
    }

    // Test 5: SteamID parsing
    {
        LOG_INFO("Test 5: SteamID parsing");

        u64 steamID;
        bool ok = ParseSteamID64(L"76561198000000001", steamID);
        LOG_INFO("Parse valid SteamID64: %s -> %llu", ok ? "OK" : "FAILED", steamID);

        ok = ParseSteamID64(L"12345", steamID);
        LOG_INFO("Parse invalid SteamID64: %s", ok ? "OK" : "FAILED (expected)");
    }

    LOG_INFO("All tests completed successfully!");
    return 0;
}