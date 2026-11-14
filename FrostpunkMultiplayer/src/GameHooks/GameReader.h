#pragma once
#include "Common.h"
#include "GameStructs.h"
#include "Memory.h"
#include <array>

namespace FrostMP::Game {

class GameReader {
public:
    explicit GameReader(const LocalMemory& mem);

    bool Initialize();

    // Time/Game Speed
    std::optional<int> GetGameSpeed() const;
    std::optional<bool> GetPaused() const;
    std::optional<int64_t> GetGameTimeMs() const;

    // Resources
    std::optional<int> GetResource(ResourceID id) const;
    static constexpr int ResourceCount = 6;
    std::array<std::optional<int>, ResourceCount> GetAllResources() const;

    // Population
    std::optional<int> GetPopulation() const;

    // Temperature
    std::optional<int> GetTemperature() const;

    // Hope/Discontent
    std::optional<int> GetHope() const;
    std::optional<int> GetDiscontent() const;

    // Save/Load
    std::optional<std::string> GetCurrentSaveName() const;

    // Full state snapshot
    std::optional<GameStateSnapshot> GetFullSnapshot() const;

    bool IsInGame() const;
    bool IsLoading() const;

private:
    const LocalMemory& mem_;

    std::uintptr_t timeController_ = 0;
    std::uintptr_t economy_ = 0;
    std::uintptr_t hope_ = 0;
    std::uintptr_t discontent_ = 0;
    std::uintptr_t saveOwner_ = 0;

    std::optional<std::uintptr_t> ReadGlobalPtr(std::uintptr_t offset) const;
    bool ValidateVTable(std::uintptr_t object, std::uintptr_t expectedVTableRVA) const;
};

} // namespace FrostMP::Game