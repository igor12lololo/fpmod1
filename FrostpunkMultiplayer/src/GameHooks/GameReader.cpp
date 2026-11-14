#include "GameReader.h"

namespace FrostMP::Game {

GameReader::GameReader(const LocalMemory& mem) : mem_(mem) {}

bool GameReader::Initialize() {
    auto timePtr = ReadGlobalPtr(OFFSET_GLOBAL_TIME);
    if (timePtr) timeController_ = *timePtr;
    auto econPtr = ReadGlobalPtr(OFFSET_GLOBAL_ECONOMY);
    if (econPtr) economy_ = *econPtr;
    auto hopePtr = ReadGlobalPtr(OFFSET_GLOBAL_HOPE);
    if (hopePtr) hope_ = *hopePtr;
    auto discontentPtr = ReadGlobalPtr(OFFSET_GLOBAL_DISCONTENT);
    if (discontentPtr) discontent_ = *discontentPtr;
    auto savePtr = ReadGlobalPtr(OFFSET_GLOBAL_SAVE_OWNER);
    if (savePtr) saveOwner_ = *savePtr;

    // Verify vtables
    if (timeController_ && !ValidateVTable(timeController_, VTABLE_TIME_CONTROLLER)) {
        timeController_ = 0;
    }
    if (economy_ && !ValidateVTable(economy_, VTABLE_ECONOMY)) {
        economy_ = 0;
    }
    if (hope_ && !ValidateVTable(hope_, VTABLE_HOPE)) {
        hope_ = 0;
    }
    if (discontent_ && !ValidateVTable(discontent_, VTABLE_DISCONTENT)) {
        discontent_ = 0;
    }
    if (saveOwner_ && !ValidateVTable(saveOwner_, VTABLE_SAVE_OWNER)) {
        saveOwner_ = 0;
    }

    DebugLog("GameReader initialized: timeController=%llX, economy=%llX, hope=%llX, discontent=%llX, saveOwner=%llX",
             timeController_, economy_, hope_, discontent_, saveOwner_);

    return timeController_ != 0 || economy_ != 0;
}

std::optional<int> GameReader::GetGameSpeed() const {
    if (!timeController_) return std::nullopt;
    auto speedPtr = mem_.ReadLocal<float>(timeController_ + GameplayTimerOffsets::SPEED_MULTIPLIER);
    if (!speedPtr) return std::nullopt;

    float speed = *speedPtr;
    if (speed <= 1.5f) return 0; // 1x
    if (speed <= 5.0f) return 1; // 3x
    return 2; // 12x
}

std::optional<bool> GameReader::GetPaused() const {
    if (!timeController_) return std::nullopt;
    auto pauseByte = mem_.ReadLocal<uint8_t>(timeController_ + GameplayTimerOffsets::USER_PAUSE_BYTE);
    return pauseByte ? std::make_optional(*pauseByte != 0) : std::nullopt;
}

std::optional<int64_t> GameReader::GetGameTimeMs() const {
    if (!timeController_) return std::nullopt;

    auto calendarTicks = mem_.ReadLocal<int64_t>(timeController_ + GameplayTimerOffsets::CALENDAR_OFFSET);
    if (!calendarTicks) return std::nullopt;

    auto hourScale = mem_.ReadLocal<int32_t>(mem_.ResolveRVALocal(GameplayTimerOffsets::TIMER_PTR));
    if (!hourScale || *hourScale == 0) return std::nullopt;

    int64_t ticks = *calendarTicks;
    int32_t scale = *hourScale;
    if (scale <= 0 || scale > 100000) return std::nullopt;
    int64_t divisor = static_cast<int64_t>(scale) * (1LL << 31);
    if (divisor == 0) return std::nullopt;
    int64_t hours = ticks / divisor;
    return hours * 3600000;
}

std::optional<int> GameReader::GetResource(ResourceID id) const {
    if (!economy_) return std::nullopt;

    auto container = mem_.ReadLocal<std::uintptr_t>(economy_ + EconomyOffsets::RESOURCE_CONTAINER);
    auto count = mem_.ReadLocal<int32_t>(economy_ + EconomyOffsets::CONTAINER_COUNT);
    if (!container || !count || *count <= 0) return std::nullopt;

    for (int i = 0; i < *count; ++i) {
        std::uintptr_t recordAddr = *container + i * EconomyOffsets::RECORD_STRIDE;
        auto entryPtr = mem_.ReadLocal<std::uintptr_t>(recordAddr + EconomyOffsets::ResourceRecord::resourceEntry);
        if (!entryPtr) continue;

        std::string telemetryKey;
        if (!mem_.ReadString(*entryPtr + EconomyOffsets::ResourceEntry::telemetryKey, telemetryKey)) continue;

        const char* expectedKey = ResourceIDToTelemetryKey(id);
        if (telemetryKey == expectedKey) {
            auto amount = mem_.ReadLocal<int32_t>(recordAddr + EconomyOffsets::ResourceRecord::amount);
            return amount ? *amount : 0;
        }
    }
    return std::nullopt;
}

static constexpr int ResourceCount = 6;

std::array<std::optional<int>, ResourceCount> GameReader::GetAllResources() const {
    std::array<std::optional<int>, ResourceCount> result{};
    for (int i = 0; i < ResourceCount; ++i) {
        result[i] = GetResource(static_cast<ResourceID>(i));
    }
    return result;
}

std::optional<int> GameReader::GetPopulation() const {
    if (!economy_) return std::nullopt;
    return std::nullopt;
}

std::optional<int> GameReader::GetTemperature() const {
    return std::nullopt;
}

std::optional<int> GameReader::GetHope() const {
    if (!hope_) return std::nullopt;
    auto enabled = mem_.ReadLocal<uint8_t>(hope_ + HopeSystemOffsets::ENABLED);
    if (!enabled || *enabled == 0) return 0;

    auto total = mem_.ReadLocal<float>(hope_ + HopeSystemOffsets::TOTAL);
    auto count = mem_.ReadLocal<int32_t>(hope_ + HopeSystemOffsets::CITIZEN_COUNT);
    if (!total || !count || *count <= 0) return std::nullopt;

    int percent = static_cast<int>((*total / *count) * 100.0f + 0.5f);
    return std::clamp(percent, 0, 100);
}

std::optional<int> GameReader::GetDiscontent() const {
    if (!discontent_) return std::nullopt;
    auto enabled = mem_.ReadLocal<uint8_t>(discontent_ + HopeSystemOffsets::ENABLED);
    if (!enabled || *enabled == 0) return 0;

    auto total = mem_.ReadLocal<float>(discontent_ + HopeSystemOffsets::TOTAL);
    auto count = mem_.ReadLocal<int32_t>(discontent_ + HopeSystemOffsets::CITIZEN_COUNT);
    if (!total || !count || *count <= 0) return std::nullopt;

    int percent = static_cast<int>((*total / *count) * 100.0f + 0.5f);
    return std::clamp(percent, 0, 100);
}

std::optional<std::string> GameReader::GetCurrentSaveName() const {
    if (!saveOwner_) return std::nullopt;
    std::wstring saveName;
    if (mem_.ReadWideString(saveOwner_ + SaveOwnerOffsets::SAVE_NAME_OFFSET, saveName)) {
        return Narrow(saveName);
    }
    return std::nullopt;
}

std::optional<GameStateSnapshot> GameReader::GetFullSnapshot() const {
    GameStateSnapshot snapshot{};
    snapshot.snapshotTime = GetTimestampMs();

    __try {
        auto speed = GetGameSpeed();
        if (speed) snapshot.gameSpeed = *speed;

        auto paused = GetPaused();
        if (paused) snapshot.isPaused = *paused;

        auto time = GetGameTimeMs();
        if (time) snapshot.gameTimeMs = *time;

        auto resources = GetAllResources();
        for (int i = 0; i < ResourceCount; ++i) {
            if (resources[i]) snapshot.resources[i] = *resources[i];
        }

        auto hope = GetHope();
        if (hope) snapshot.hope = *hope;

        auto discontent = GetDiscontent();
        if (discontent) snapshot.discontent = *discontent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile("GAMEREADER: SEH exception in GetFullSnapshot - memory read failed");
        return std::nullopt;
    }

    return snapshot;
}

bool GameReader::IsInGame() const {
    return timeController_ != 0 && economy_ != 0;
}

bool GameReader::IsLoading() const {
    auto loadingFlag = mem_.ReadLocal<uint8_t>(OFFSET_GLOBAL_LOADING_FLAG);
    return loadingFlag && *loadingFlag != 0;
}

std::optional<std::uintptr_t> GameReader::ReadGlobalPtr(std::uintptr_t offset) const {
    auto addr = mem_.ResolveRVALocal(offset);
    if (!addr) return std::nullopt;
    return mem_.ReadLocal<std::uintptr_t>(addr);
}

bool GameReader::ValidateVTable(std::uintptr_t object, std::uintptr_t expectedVTableRVA) const {
    auto vtable = mem_.ReadLocal<std::uintptr_t>(object);
    if (!vtable) return false;
    std::uintptr_t expected = mem_.ResolveRVALocal(expectedVTableRVA);
    return *vtable == expected;
}

} // namespace FrostMP::Game