#pragma once
#include "Common.h"
#include "GameStructs.h"
#include "Memory.h"
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace FrostMP::Game {

// Forward declaration for callback context
struct HookContext {
    std::uintptr_t rcx;   // 1st arg
    std::uintptr_t rdx;   // 2nd arg
    std::uintptr_t r8;    // 3rd arg
    std::uintptr_t r9;    // 4th arg
    std::uintptr_t retAddr; // return address
};

// User callback signature: return true to continue to original, false to skip original
using HookCallback = bool(*)(std::uintptr_t returnAddress, void* context, void* userData);

// Maximum number of concurrent hooks
inline constexpr size_t MAX_HOOKS = 32;

class HookManager {
public:
    struct Hook {
        std::uintptr_t address = 0;
        std::vector<u8> originalBytes;
        std::vector<u8> hookBytes;
        void* trampoline = nullptr;
        void* detour = nullptr;
        bool enabled = false;
        std::string name;
        HookCallback callback = nullptr;
        void* userData = nullptr;
        size_t prologueSize = 0;
        bool installing = false; // Protected by hooksMutex_
    };

    HookManager(const LocalMemory& mem);
    ~HookManager();

    // Delete copy/move
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;
    HookManager(HookManager&&) = delete;
    HookManager& operator=(HookManager&&) = delete;

    // Install a callback hook (inline hook with trampoline)
    // Returns true on success
    bool InstallCallbackHook(const std::string& name, std::uintptr_t targetRVA,
                             HookCallback callback, void* userData = nullptr);

    // Install a simple JMP hook (redirect to detour function)
    bool InstallHook(const std::string& name, std::uintptr_t targetRVA, void* detour);

    // Enable/disable installed hooks
    bool EnableHook(const std::string& name);
    bool DisableHook(const std::string& name);

    // Remove hook completely
    void RemoveHook(const std::string& name);
    void RemoveAllHooks();

    // Query
    bool IsHooked(const std::string& name) const;
    void* GetTrampoline(const std::string& name);

    // Static callback trampolines (one per hook slot, generated via template)
    template<size_t N>
    static bool StaticCallback(std::uintptr_t returnAddress, void* context, void* userData);

private:
    // Internal hook storage (fixed array for stable pointers)
    std::array<Hook, MAX_HOOKS> hooks_;
    std::atomic<size_t> hookCount_{0};
    std::unordered_map<std::string, size_t> nameToIndex_;
    mutable std::recursive_mutex hooksMutex_;

    const LocalMemory& mem_;

    // Thread suspension for safe hooking
    struct ThreadSuspender {
        std::vector<HANDLE> threads;
        bool SuspendAllExceptCurrent();
        void ResumeAll();
    };

    // Minimal x64 disassembler for instruction boundaries and relative instruction relocation
    struct Disassembler {
        struct Instruction {
            size_t length = 0;
            bool isRelative = false;      // CALL/JMP/Jcc with rel32
            bool isRipRelative = false;   // RIP-relative memory access
            int32_t relOffset = 0;        // For relative instructions
            size_t relOffsetPos = 0;      // Position of rel32 in instruction
        };

        // Decode single instruction, return length (0 = invalid/unknown)
        static size_t Decode(const u8* code, size_t maxLen, Instruction& out);

        // Check if instruction is a control flow instruction we need to relocate
        static bool IsRelativeControlFlow(const u8* code, size_t len, int32_t& relOffset, size_t& relOffsetPos);

        // Check if instruction uses RIP-relative addressing
        static bool IsRipRelative(const u8* code, size_t len, int32_t& dispOffset, size_t& dispPos);

        // Relocate instruction from src to dst (adjusting RIP-relative and relative offsets)
        // Returns new instruction length (may be different if we need to expand)
        static size_t Relocate(const u8* src, u8* dst, size_t maxDst,
                               std::uintptr_t srcAddr, std::uintptr_t dstAddr);
    };

    // Find suitable prologue size by disassembling instructions
    // Returns size >= minSize that ends on instruction boundary
    size_t FindPrologueSize(std::uintptr_t target, size_t minSize);

    // Build trampoline with relocated instructions
    void* BuildTrampoline(std::uintptr_t target, const std::vector<u8>& originalBytes,
                          size_t prologueSize);

    // Build detour that calls static callback
    void* BuildDetour(size_t hookIndex, void* trampoline);

    // Write hook bytes with proper memory protection
    bool WriteProtected(std::uintptr_t address, const void* data, size_t size);

    // Read back and verify
    bool VerifyWrite(std::uintptr_t address, const void* expected, size_t size);

    // Get next available hook index
    bool AllocateHookIndex(size_t& outIndex);
    void FreeHookIndex(size_t index);
};

// Global hook manager instance (set by ModMain)
extern HookManager* g_hookManager;

} // namespace FrostMP::Game