#include "Memory.h"

namespace FrostMP::Game {

template<typename T>
std::optional<T> Memory::Read(std::uintptr_t address) const {
    T value{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(address), &value, sizeof(T), &bytesRead) ||
        bytesRead != sizeof(T)) {
        return std::nullopt;
    }
    return value;
}

template<typename T>
bool Memory::Write(std::uintptr_t address, const T& value) const {
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(process_, reinterpret_cast<void*>(address), &value, sizeof(T), &bytesWritten) &&
           bytesWritten == sizeof(T);
}

template<typename T>
bool Memory::Write(std::uintptr_t address, const T* data, size_t count) const {
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(process_, reinterpret_cast<void*>(address), data, sizeof(T) * count, &bytesWritten) &&
           bytesWritten == sizeof(T) * count;
}

bool Memory::Read(std::uintptr_t address, void* buffer, size_t size) const {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(process_, reinterpret_cast<const void*>(address), buffer, size, &bytesRead) &&
           bytesRead == size;
}

bool Memory::ReadString(std::uintptr_t address, std::string& out, size_t maxLen) const {
    std::vector<char> buffer(maxLen);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(address), buffer.data(), maxLen, &bytesRead)) {
        return false;
    }
    buffer[bytesRead < maxLen ? bytesRead : maxLen - 1] = '\0';
    out = buffer.data();
    return true;
}

bool Memory::ReadWideString(std::uintptr_t address, std::wstring& out, size_t maxLen) const {
    std::vector<wchar_t> buffer(maxLen);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(address), buffer.data(), maxLen * sizeof(wchar_t), &bytesRead)) {
        return false;
    }
    size_t charCount = bytesRead / sizeof(wchar_t);
    buffer[charCount < maxLen ? charCount : maxLen - 1] = L'\0';
    out = buffer.data();
    return true;
}

std::uintptr_t Memory::GetModuleBase(const wchar_t* moduleName) const {
    if (process_ == GetCurrentProcess()) {
        return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(moduleName));
    }

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(process_)));
    if (!snapshot) return 0;

    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, moduleName) == 0) {
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            }
        } while (Module32NextW(snapshot, &module));
    }
    return 0;
}

std::uintptr_t Memory::ResolveRVA(std::uintptr_t rva) const {
    std::uintptr_t base = GetModuleBase(L"Frostpunk.exe");
    return base ? base + rva : 0;
}

HANDLE Memory::GetProcessHandle() const { return process_; }

// LocalMemory implementations
template<typename T>
std::optional<T> LocalMemory::ReadLocal(std::uintptr_t address) const {
    if (!address) return std::nullopt;
    T value{};
    if (ReadLocalRaw(address, &value, sizeof(T))) {
        return value;
    }
    return std::nullopt;
}

template<typename T>
bool LocalMemory::WriteLocal(std::uintptr_t address, const T& value) const {
    if (!address) return false;
    return WriteLocalRaw(address, &value, sizeof(T));
}

std::uintptr_t LocalMemory::GetModuleBaseLocal(const wchar_t* moduleName) const {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(moduleName));
}

std::uintptr_t LocalMemory::ResolveRVALocal(std::uintptr_t rva) const {
    std::uintptr_t base = GetModuleBaseLocal(L"Frostpunk.exe");
    return base ? base + rva : 0;
}

bool LocalMemory::ReadLocalRaw(std::uintptr_t address, void* buffer, size_t size) const {
    __try {
        std::memcpy(buffer, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LocalMemory::WriteLocalRaw(std::uintptr_t address, const void* buffer, size_t size) const {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), buffer, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Explicit template instantiations for common types
template std::optional<uint8_t> Memory::Read<uint8_t>(std::uintptr_t) const;
template std::optional<int8_t> Memory::Read<int8_t>(std::uintptr_t) const;
template std::optional<uint16_t> Memory::Read<uint16_t>(std::uintptr_t) const;
template std::optional<int16_t> Memory::Read<int16_t>(std::uintptr_t) const;
template std::optional<uint32_t> Memory::Read<uint32_t>(std::uintptr_t) const;
template std::optional<int32_t> Memory::Read<int32_t>(std::uintptr_t) const;
template std::optional<uint64_t> Memory::Read<uint64_t>(std::uintptr_t) const;
template std::optional<int64_t> Memory::Read<int64_t>(std::uintptr_t) const;
template std::optional<float> Memory::Read<float>(std::uintptr_t) const;
template std::optional<double> Memory::Read<double>(std::uintptr_t) const;
template std::optional<std::uintptr_t> Memory::Read<std::uintptr_t>(std::uintptr_t) const;

template bool Memory::Write<uint8_t>(std::uintptr_t, const uint8_t&) const;
template bool Memory::Write<int8_t>(std::uintptr_t, const int8_t&) const;
template bool Memory::Write<uint16_t>(std::uintptr_t, const uint16_t&) const;
template bool Memory::Write<int16_t>(std::uintptr_t, const int16_t&) const;
template bool Memory::Write<uint32_t>(std::uintptr_t, const uint32_t&) const;
template bool Memory::Write<int32_t>(std::uintptr_t, const int32_t&) const;
template bool Memory::Write<uint64_t>(std::uintptr_t, const uint64_t&) const;
template bool Memory::Write<int64_t>(std::uintptr_t, const int64_t&) const;
template bool Memory::Write<float>(std::uintptr_t, const float&) const;
template bool Memory::Write<double>(std::uintptr_t, const double&) const;
template bool Memory::Write<std::uintptr_t>(std::uintptr_t, const std::uintptr_t&) const;

// Array Write instantiations
template bool Memory::Write<uint8_t>(std::uintptr_t, const uint8_t*, size_t) const;
template bool Memory::Write<int8_t>(std::uintptr_t, const int8_t*, size_t) const;
template bool Memory::Write<uint16_t>(std::uintptr_t, const uint16_t*, size_t) const;
template bool Memory::Write<int16_t>(std::uintptr_t, const int16_t*, size_t) const;
template bool Memory::Write<uint32_t>(std::uintptr_t, const uint32_t*, size_t) const;
template bool Memory::Write<int32_t>(std::uintptr_t, const int32_t*, size_t) const;
template bool Memory::Write<uint64_t>(std::uintptr_t, const uint64_t*, size_t) const;
template bool Memory::Write<int64_t>(std::uintptr_t, const int64_t*, size_t) const;
template bool Memory::Write<float>(std::uintptr_t, const float*, size_t) const;
template bool Memory::Write<double>(std::uintptr_t, const double*, size_t) const;
template bool Memory::Write<std::uintptr_t>(std::uintptr_t, const std::uintptr_t*, size_t) const;

template std::optional<uint8_t> LocalMemory::ReadLocal<uint8_t>(std::uintptr_t) const;
template std::optional<int8_t> LocalMemory::ReadLocal<int8_t>(std::uintptr_t) const;
template std::optional<uint16_t> LocalMemory::ReadLocal<uint16_t>(std::uintptr_t) const;
template std::optional<int16_t> LocalMemory::ReadLocal<int16_t>(std::uintptr_t) const;
template std::optional<uint32_t> LocalMemory::ReadLocal<uint32_t>(std::uintptr_t) const;
template std::optional<int32_t> LocalMemory::ReadLocal<int32_t>(std::uintptr_t) const;
template std::optional<uint64_t> LocalMemory::ReadLocal<uint64_t>(std::uintptr_t) const;
template std::optional<int64_t> LocalMemory::ReadLocal<int64_t>(std::uintptr_t) const;
template std::optional<float> LocalMemory::ReadLocal<float>(std::uintptr_t) const;
template std::optional<double> LocalMemory::ReadLocal<double>(std::uintptr_t) const;
template std::optional<std::uintptr_t> LocalMemory::ReadLocal<std::uintptr_t>(std::uintptr_t) const;

template bool LocalMemory::WriteLocal<uint8_t>(std::uintptr_t, const uint8_t&) const;
template bool LocalMemory::WriteLocal<int8_t>(std::uintptr_t, const int8_t&) const;
template bool LocalMemory::WriteLocal<uint16_t>(std::uintptr_t, const uint16_t&) const;
template bool LocalMemory::WriteLocal<int16_t>(std::uintptr_t, const int16_t&) const;
template bool LocalMemory::WriteLocal<uint32_t>(std::uintptr_t, const uint32_t&) const;
template bool LocalMemory::WriteLocal<int32_t>(std::uintptr_t, const int32_t&) const;
template bool LocalMemory::WriteLocal<uint64_t>(std::uintptr_t, const uint64_t&) const;
template bool LocalMemory::WriteLocal<int64_t>(std::uintptr_t, const int64_t&) const;
template bool LocalMemory::WriteLocal<float>(std::uintptr_t, const float&) const;
template bool LocalMemory::WriteLocal<double>(std::uintptr_t, const double&) const;
template bool LocalMemory::WriteLocal<std::uintptr_t>(std::uintptr_t, const std::uintptr_t&) const;

} // namespace FrostMP::Game