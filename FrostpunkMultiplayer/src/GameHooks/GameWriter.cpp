#include "GameWriter.h"

namespace FrostMP::Game {

GameWriter::GameWriter(const LocalMemory& mem) : mem_(mem) {}

bool GameWriter::Initialize() {
    // Resolve function pointers
    setSpeed_ = reinterpret_cast<SetSpeedFn>(mem_.ResolveRVALocal(RVA_SET_SPEED));
    changeResource_ = reinterpret_cast<ChangeResourceFn>(mem_.ResolveRVALocal(RVA_CHANGE_RESOURCE));
    pauseAddReason_ = reinterpret_cast<PauseAddReasonFn>(mem_.ResolveRVALocal(RVA_PAUSE_ADD_REASON));
    pauseRemoveReason_ = reinterpret_cast<PauseRemoveReasonFn>(mem_.ResolveRVALocal(RVA_PAUSE_REMOVE_REASON));
    nativeSave_ = reinterpret_cast<NativeSaveFn>(mem_.ResolveRVALocal(RVA_NATIVE_SAVE));
    nativeLoad_ = reinterpret_cast<NativeLoadFn>(mem_.ResolveRVALocal(RVA_NATIVE_LOAD));
    selectMap_ = reinterpret_cast<SelectMapFn>(mem_.ResolveRVALocal(RVA_SELECT_MAP));
    endlessStart_ = reinterpret_cast<EndlessStartFn>(mem_.ResolveRVALocal(RVA_ENDLESS_START));
    setButtonEnabled_ = reinterpret_cast<SetButtonEnabledFn>(mem_.ResolveRVALocal(RVA_SET_BUTTON_ENABLED));
    bindButtonCallback_ = reinterpret_cast<BindButtonCallbackFn>(mem_.ResolveRVALocal(RVA_BIND_BUTTON_CALLBACK));
    setUiState_ = reinterpret_cast<SetUiStateFn>(mem_.ResolveRVALocal(RVA_SET_UI_STATE));

    auto timePtr = mem_.ReadLocal<std::uintptr_t>(mem_.ResolveRVALocal(OFFSET_GLOBAL_TIME));
    if (timePtr) timeController_ = *timePtr;
    auto econPtr = mem_.ReadLocal<std::uintptr_t>(mem_.ResolveRVALocal(OFFSET_GLOBAL_ECONOMY));
    if (econPtr) economy_ = *econPtr;
    auto savePtr = mem_.ReadLocal<std::uintptr_t>(mem_.ResolveRVALocal(OFFSET_GLOBAL_SAVE_OWNER));
    if (savePtr) saveOwner_ = *savePtr;

    DebugLog("GameWriter initialized: setSpeed=%p, changeResource=%p, pauseAdd=%p, pauseRemove=%p",
             setSpeed_, changeResource_, pauseAddReason_, pauseRemoveReason_);

    return setSpeed_ && changeResource_ && pauseAddReason_ && pauseRemoveReason_;
}

bool GameWriter::SetGameSpeed(int speedMode) {
    if (!setSpeed_ || !timeController_ || speedMode < 0 || speedMode > 2) return false;
    return setSpeed_(timeController_, speedMode);
}

bool GameWriter::SetPaused(bool paused) {
    if (!timeController_) return false;

    static std::uintptr_t networkPauseToken = 0;
    if (paused) {
        if (!networkPauseToken && pauseAddReason_) {
            networkPauseToken = pauseAddReason_(timeController_, reinterpret_cast<void*>(&networkPauseToken));
        }
    } else {
        if (networkPauseToken && pauseRemoveReason_) {
            pauseRemoveReason_(timeController_, reinterpret_cast<void*>(networkPauseToken));
            networkPauseToken = 0;
        }
    }
    return true;
}

bool GameWriter::ChangeResource(ResourceID id, int delta) {
    if (!changeResource_ || !economy_) return false;

    auto container = mem_.ReadLocal<std::uintptr_t>(economy_ + EconomyOffsets::RESOURCE_CONTAINER);
    auto count = mem_.ReadLocal<int32_t>(economy_ + EconomyOffsets::CONTAINER_COUNT);
    if (!container || !count || *count <= 0) return false;

    const char* targetTelemetry = ResourceIDToTelemetryKey(id);
    for (int i = 0; i < *count; ++i) {
        std::uintptr_t recordAddr = *container + i * EconomyOffsets::RECORD_STRIDE;
        auto entryPtr = mem_.ReadLocal<std::uintptr_t>(recordAddr + EconomyOffsets::ResourceRecord::resourceEntry);
        if (!entryPtr) continue;

        std::string telemetryKey;
        if (!mem_.ReadString(*entryPtr + EconomyOffsets::ResourceEntry::telemetryKey, telemetryKey)) continue;

        if (telemetryKey == targetTelemetry) {
            return changeResource_(economy_, *entryPtr, delta, nullptr, 0, false);
        }
    }
    return false;
}

bool GameWriter::SetResource(ResourceID id, int amount) {
    auto current = GetResourceAmount(id);
    if (!current) return false;
    return ChangeResource(id, amount - *current);
}

std::optional<int> GameWriter::GetResourceAmount(ResourceID id) const {
    if (!economy_) return std::nullopt;

    auto container = mem_.ReadLocal<std::uintptr_t>(economy_ + EconomyOffsets::RESOURCE_CONTAINER);
    auto count = mem_.ReadLocal<int32_t>(economy_ + EconomyOffsets::CONTAINER_COUNT);
    if (!container || !count || *count <= 0) return std::nullopt;

    const char* targetTelemetry = ResourceIDToTelemetryKey(id);
    for (int i = 0; i < *count; ++i) {
        std::uintptr_t recordAddr = *container + i * EconomyOffsets::RECORD_STRIDE;
        auto entryPtr = mem_.ReadLocal<std::uintptr_t>(recordAddr + EconomyOffsets::ResourceRecord::resourceEntry);
        if (!entryPtr) continue;

        std::string telemetryKey;
        if (!mem_.ReadString(*entryPtr + EconomyOffsets::ResourceEntry::telemetryKey, telemetryKey)) continue;

        if (telemetryKey == targetTelemetry) {
            auto amount = mem_.ReadLocal<int32_t>(recordAddr + EconomyOffsets::ResourceRecord::amount);
            return amount;
        }
    }
    return std::nullopt;
}

bool GameWriter::SaveGame(const char* saveName, bool isMultiplayer) {
    if (!nativeSave_ || !saveOwner_) return false;
    return nativeSave_(saveOwner_, isMultiplayer, false, saveName);
}

bool GameWriter::LoadGame(const char* saveName) {
    if (!nativeLoad_ || !saveOwner_) return false;
    return nativeLoad_(saveOwner_, saveName);
}

bool GameWriter::SelectMap(int mapIndex) {
    if (!selectMap_ || !timeController_) return false;
    return selectMap_(timeController_, mapIndex);
}

bool GameWriter::StartEndlessGame() {
    if (!endlessStart_ || !timeController_) return false;
    return endlessStart_(timeController_);
}

bool GameWriter::EnableButton(std::uintptr_t buttonPtr, bool enable, bool animated) {
    if (!setButtonEnabled_ || !buttonPtr) return false;
    return setButtonEnabled_(buttonPtr, enable, animated);
}

bool GameWriter::BindButtonCallback(std::uintptr_t buttonPtr, std::uintptr_t owner, void* callback, int type, int argument, int flags) {
    if (!bindButtonCallback_ || !buttonPtr) return false;
    return bindButtonCallback_(buttonPtr, owner, callback, type, argument, flags);
}

bool GameWriter::SetUiElementVisibility(std::uintptr_t elementPtr, bool visible) {
    if (!setUiState_ || !elementPtr) return false;
    return setUiState_(elementPtr, visible, 0, 0);
}

} // namespace FrostMP::Game