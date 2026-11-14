#pragma once
#include "Common.h"
#include "GameStructs.h"
#include "Memory.h"

namespace FrostMP::Game {

class GameWriter {
public:
    explicit GameWriter(const LocalMemory& mem);

    bool Initialize();

    // Game Speed (0=1x, 1=3x, 2=12x)
    bool SetGameSpeed(int speedMode);

    // Pause/Unpause
    bool SetPaused(bool paused);

    // Resource manipulation
    bool ChangeResource(ResourceID id, int delta);
    bool SetResource(ResourceID id, int amount);
    std::optional<int> GetResourceAmount(ResourceID id) const;

    // Save/Load
    bool SaveGame(const char* saveName, bool isMultiplayer = true);
    bool LoadGame(const char* saveName);

    // Map/Scenario selection (Host only)
    bool SelectMap(int mapIndex);
    bool StartEndlessGame();

    // UI Helpers
    bool EnableButton(std::uintptr_t buttonPtr, bool enable, bool animated = true);
    bool BindButtonCallback(std::uintptr_t buttonPtr, std::uintptr_t owner, void* callback, int type, int argument, int flags);
    bool SetUiElementVisibility(std::uintptr_t elementPtr, bool visible);

private:
    const LocalMemory& mem_;

    // Function pointers
    using SetSpeedFn = bool(*)(std::uintptr_t timeController, int speedMode);
    using ChangeResourceFn = bool(*)(std::uintptr_t economy, std::uintptr_t resourceEntry, int delta, void* eventContext, int changeType, bool option);
    using PauseAddReasonFn = std::uintptr_t(*)(std::uintptr_t timeController, void* reason);
    using PauseRemoveReasonFn = void(*)(std::uintptr_t timeController, void* reason);
    using NativeSaveFn = bool(*)(std::uintptr_t saveOwner, bool multiplayer, bool overwrite, const char* name);
    using NativeLoadFn = bool(*)(std::uintptr_t saveOwner, const char* name);
    using SelectMapFn = bool(*)(std::uintptr_t timeController, int mapIndex);
    using EndlessStartFn = bool(*)(std::uintptr_t timeController);
    using SetButtonEnabledFn = bool(*)(std::uintptr_t button, bool enable, bool animated);
    using BindButtonCallbackFn = bool(*)(std::uintptr_t button, std::uintptr_t owner, void* callback, int type, int argument, int flags);
    using SetUiStateFn = bool(*)(std::uintptr_t element, bool visible, int state, int flags);

    SetSpeedFn setSpeed_ = nullptr;
    ChangeResourceFn changeResource_ = nullptr;
    PauseAddReasonFn pauseAddReason_ = nullptr;
    PauseRemoveReasonFn pauseRemoveReason_ = nullptr;
    NativeSaveFn nativeSave_ = nullptr;
    NativeLoadFn nativeLoad_ = nullptr;
    SelectMapFn selectMap_ = nullptr;
    EndlessStartFn endlessStart_ = nullptr;
    SetButtonEnabledFn setButtonEnabled_ = nullptr;
    BindButtonCallbackFn bindButtonCallback_ = nullptr;
    SetUiStateFn setUiState_ = nullptr;

    std::uintptr_t timeController_ = 0;
    std::uintptr_t economy_ = 0;
    std::uintptr_t saveOwner_ = 0;
};

} // namespace FrostMP::Game