#pragma once
#include "Common.h"

namespace FrostMP::Game {

// Frostpunk Liquid Engine structures (based on reverse engineering)
// These are offsets for Frostpunk 1.6.2 (SHA: 031dbfe529256b7bf532aa11bfaf8ef75ef0618aa0492a970e532566342ba19d)
// Verified via pattern scanning on 2026-11-13

// Global pointers (1.6.2)
inline constexpr std::uintptr_t OFFSET_GLOBAL_TIME = 0x2B5ABF0;           // GameplayTimer* (verified - most referenced)
inline constexpr std::uintptr_t OFFSET_GLOBAL_ECONOMY = 0x3FC93B0;        // FrostpunkEconomy* (TBD - keep 1.6.1 value)
inline constexpr std::uintptr_t OFFSET_GLOBAL_DISCONTENT = 0x3FD1410;     // HopeSystem* (TBD)
inline constexpr std::uintptr_t OFFSET_GLOBAL_HOPE = 0x3FD3DE0;           // HopeSystem* (TBD)
inline constexpr std::uintptr_t OFFSET_GLOBAL_SAVE_OWNER = 0x2B68120;     // SaveOwner* (TBD)
inline constexpr std::uintptr_t OFFSET_GLOBAL_LOADING_FLAG = 0x2B68120;   // Loading flag (TBD)

// VTables (TBD - keep 1.6.1 values for now)
inline constexpr std::uintptr_t VTABLE_TIME_CONTROLLER = 0x1DFCB80;
inline constexpr std::uintptr_t VTABLE_TIMER = 0x1D716F0;
inline constexpr std::uintptr_t VTABLE_ECONOMY = 0x1FA66A0;
inline constexpr std::uintptr_t VTABLE_HOPE = 0x2013278;
inline constexpr std::uintptr_t VTABLE_DISCONTENT = 0x1FF0140;
inline constexpr std::uintptr_t VTABLE_SAVE_OWNER = 0x1DF9AE0;
inline constexpr std::uintptr_t VTABLE_UI_BUTTON = 0x1D75E08;
inline constexpr std::uintptr_t VTABLE_MAIN_MENU = 0x204C488;
inline constexpr std::uintptr_t VTABLE_SCENARIOS_PANEL = 0x1D716F0;

// Function RVAs (1.6.2 - verified where marked)
inline constexpr std::uintptr_t RVA_SET_SPEED = 0xD8E8FF;                 // VERIFIED - LEA RCX,[RSI+30] pattern
inline constexpr std::uintptr_t RVA_CALENDAR_CONVERT = 0x11AE360;         // TBD
inline constexpr std::uintptr_t RVA_CHANGE_RESOURCE = 0xE55880;           // VERIFIED - MOV RCX,[RCX] pattern
inline constexpr std::uintptr_t RVA_PAUSE_ADD_REASON = 0xF6C530;          // TBD - keep 1.6.1
inline constexpr std::uintptr_t RVA_PAUSE_REMOVE_REASON = 0x120A4E0;      // VERIFIED - TEST RDX,RDX / MOV RCX,RDX
inline constexpr std::uintptr_t RVA_NATIVE_SAVE = 0x16A2850;              // VERIFIED - CALL + MOV RCX,[R14] pattern
inline constexpr std::uintptr_t RVA_NATIVE_LOAD = 0x11B5E30;              // TBD - keep 1.6.1
inline constexpr std::uintptr_t RVA_SELECT_MAP = 0x1A8B880;               // TBD - keep 1.6.1
inline constexpr std::uintptr_t RVA_ENDLESS_START = 0xDCDA00;             // VERIFIED - unique binary search pattern
inline constexpr std::uintptr_t RVA_SET_BUTTON_ENABLED = 0xFCD100;
inline constexpr std::uintptr_t RVA_BIND_BUTTON_CALLBACK = 0xFCCF20;
inline constexpr std::uintptr_t RVA_SET_UI_STATE = 0xF75DA0;
inline constexpr std::uintptr_t RVA_STRING_CTOR = 0xE48BA0;
inline constexpr std::uintptr_t RVA_STRING_DTOR = 0xE47C30;
inline constexpr std::uintptr_t RVA_FIND_CHILD = 0xF77CC0;
inline constexpr std::uintptr_t RVA_SET_UI_TEXT = 0x106B5F0;
inline constexpr std::uintptr_t RVA_SET_VISIBILITY = 0xFCD9F0;
inline constexpr std::uintptr_t RVA_SET_UI_TRANSFORM = 0x10504B0;
inline constexpr std::uintptr_t RVA_LOADING_PREDICATE = 0xF283C0;

// Menu callbacks
inline constexpr std::uintptr_t RVA_MENU_UPDATE = 0x1A770F0;
inline constexpr std::uintptr_t RVA_MENU_BIND_BUTTONS = 0x1A76B80;
inline constexpr std::uintptr_t RVA_MENU_PANEL_CALLBACK = 0x1A748E0;
inline constexpr std::uintptr_t RVA_SCENARIOS_UPDATE = 0x1A7B180;
inline constexpr std::uintptr_t RVA_SCENARIOS_START = 0x1A7BEA0;
inline constexpr std::uintptr_t RVA_SCENARIO_ROW_CALLBACK = 0x1A7BAF0;
inline constexpr std::uintptr_t RVA_SET_SCENARIO_VIEW = 0x1A79390;
inline constexpr std::uintptr_t RVA_SELECT_ENDLESS_MODE = 0x1A879C0;
inline constexpr std::uintptr_t RVA_ENDLESS_BUILD = 0x1A871E0;
inline constexpr std::uintptr_t RVA_ENDLESS_SHOW = 0x1A89900;

// Offsets within structures
struct GameplayTimerOffsets {
    static constexpr std::uintptr_t SPEED_MODES = 0x2B70CB4;      // float[3] = {1, 3, 12}
    static constexpr std::uintptr_t TIMER_PTR = 0x2B70CC0;        // int32* for hour scale
    static constexpr std::uintptr_t USER_PAUSE_BYTE = 0xE0;
    static constexpr std::uintptr_t REASONS_ARRAY = 0xB8;
    static constexpr std::uintptr_t REASONS_COUNT = 0xC0;
    static constexpr std::uintptr_t CALENDAR_OFFSET = 0xB0;
    static constexpr std::uintptr_t SPEED_MULTIPLIER = 0xD0;
};

struct EconomyOffsets {
    static constexpr std::uintptr_t RESOURCE_CONTAINER = 0x2240;
    static constexpr std::uintptr_t CONTAINER_COUNT = 0x2248;
    static constexpr std::uintptr_t CONTAINER_CAPACITY = 0x224C;
    static constexpr std::uintptr_t RECORD_STRIDE = 0x70;

    struct ResourceRecord {
        inline static constexpr std::uintptr_t resourceEntry = 0x00;  // FrostpunkResourceEntry*
        inline static constexpr std::uintptr_t amount = 0x08;
        inline static constexpr std::uintptr_t capacity = 0x0C;
        // unknown 0x10..0x6F
    };

    struct ResourceEntry {
        inline static constexpr std::uintptr_t internalName = 0x00;     // const char*
        inline static constexpr std::uintptr_t guid = 0x08;             // 16-byte GUID
        inline static constexpr std::uintptr_t localizationKey = 0x28;  // const char*
        inline static constexpr std::uintptr_t telemetryKey = 0xC0;     // const char*
    };
};

struct HopeSystemOffsets {
    static constexpr std::uintptr_t INITIALIZED = 0x2160;
    static constexpr std::uintptr_t ENABLED = 0x2161;
    static constexpr std::uintptr_t TOTAL = 0x2164;       // float
    static constexpr std::uintptr_t CITIZEN_COUNT = 0x2168; // int32
    static constexpr std::uintptr_t GET_AVERAGE = 0x1827450; // Hope, 0x17A88A0 for Discontent
};

struct SaveOwnerOffsets {
    static constexpr std::uintptr_t SAVE_DATA_START = 0x201E; // 0x20 1E 00 00
    static constexpr std::uintptr_t SAVE_NAME_OFFSET = 0x00;
};

struct MenuOffsets {
    struct MainMenuPanel {
        static constexpr std::uintptr_t CONTINUE_BTN = 0xD0;
        static constexpr std::uintptr_t NEW_GAME_BTN = 0xD8;
        static constexpr std::uintptr_t LOAD_BTN = 0xE0;
        static constexpr std::uintptr_t SCENARIO_BTN = 0xE8;
        static constexpr std::uintptr_t CUSTOM_SCENARIO_BTN = 0xF0;
        static constexpr std::uintptr_t ENDLESS_BTN = 0xF8;
        static constexpr std::uintptr_t DLC_BTN = 0x100;
    };
};

// Resource internal names (stable identifiers)
enum class ResourceID : int {
    Coal = 0,
    Wood = 1,
    Steel = 2,
    SteamCores = 3,
    RawFood = 4,
    FoodRations = 5,
    Count = 6
};

inline const char* ResourceIDToName(ResourceID id) {
    static const char* names[] = {
        "Coal", "Wood", "Steel", "Steam Cores", "Raw Food", "Food Rations"
    };
    return (id >= ResourceID::Coal && id < ResourceID::Count) ? names[static_cast<int>(id)] : "Unknown";
}

inline const char* ResourceIDToTelemetryKey(ResourceID id) {
    static const char* keys[] = {
        "res_coal", "res_wood", "res_steel", "res_cores", "res_food", "res_food_rations"
    };
    return (id >= ResourceID::Coal && id < ResourceID::Count) ? keys[static_cast<int>(id)] : "";
}

// Game state snapshot for synchronization
struct GameStateSnapshot {
    // Map/Scenario
    int mapIndex = -1;
    int scenarioType = 0; // 0=Endless, 1=Story
    int difficulty = 0;
    unsigned int randomSeed = 0;

    // Time
    long long gameTimeMs = 0;
    bool isPaused = false;
    int gameSpeed = 0;

    // Resources
    int resources[static_cast<int>(ResourceID::Count)] = {0};
    int population = 0;
    int temperature = 0;
    int hope = -1;
    int discontent = -1;

    // Players
    unsigned long long hostSteamID = 0;
    unsigned long long clientSteamID = 0;

    // Metadata
    unsigned long long snapshotTime = 0;
    unsigned int sequence = 0;
};

} // namespace FrostMP::Game