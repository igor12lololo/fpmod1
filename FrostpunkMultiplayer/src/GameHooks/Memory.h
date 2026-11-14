#pragma once
#include "Common.h"
#include "GameStructs.h"
#include <tlhelp32.h>

namespace FrostMP::Game {

class Memory {
public:
    explicit Memory(HANDLE process = GetCurrentProcess()) : process_(process) {}

    template<typename T>
    std::optional<T> Read(std::uintptr_t address) const;

    template<typename T>
    bool Write(std::uintptr_t address, const T& value) const;

    template<typename T>
    bool Write(std::uintptr_t address, const T* data, size_t count) const;

    bool Read(std::uintptr_t address, void* buffer, size_t size) const;

    bool ReadString(std::uintptr_t address, std::string& out, size_t maxLen = 256) const;

    bool ReadWideString(std::uintptr_t address, std::wstring& out, size_t maxLen = 256) const;

    std::uintptr_t GetModuleBase(const wchar_t* moduleName) const;

    std::uintptr_t ResolveRVA(std::uintptr_t rva) const;

    HANDLE GetProcessHandle() const;

private:
    HANDLE process_;
};

// Local process memory (for injected DLL) - uses SEH for safe access
class LocalMemory : public Memory {
public:
    LocalMemory() : Memory(GetCurrentProcess()) {}

    template<typename T>
    std::optional<T> ReadLocal(std::uintptr_t address) const;

    template<typename T>
    bool WriteLocal(std::uintptr_t address, const T& value) const;

    std::uintptr_t GetModuleBaseLocal(const wchar_t* moduleName) const;

    std::uintptr_t ResolveRVALocal(std::uintptr_t rva) const;

    // SEH-safe raw read - cannot be template/inline due to __try
    bool ReadLocalRaw(std::uintptr_t address, void* buffer, size_t size) const;

    // SEH-safe raw write - cannot be template/inline due to __try
    bool WriteLocalRaw(std::uintptr_t address, const void* buffer, size_t size) const;
};

// Global instance for DLL
inline LocalMemory g_Memory;

} // namespace FrostMP::Game