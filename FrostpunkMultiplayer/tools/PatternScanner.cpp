#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <iomanip>
#include <filesystem>

struct Pattern {
    const char* name;
    const char* pattern;  // e.g. "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20"
    uintptr_t expectedRVA;
};

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open file");
    size_t size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

std::vector<uint8_t> ParsePattern(const char* pattern) {
    std::vector<uint8_t> bytes;
    std::string p(pattern);
    size_t pos = 0;
    while (pos < p.size()) {
        while (pos < p.size() && isspace(p[pos])) pos++;
        if (pos >= p.size()) break;
        if (p[pos] == '?') {
            bytes.push_back(0xFF); // wildcard marker
            pos++;
            if (pos < p.size() && p[pos] == '?') pos++;
        } else {
            char buf[3] = { p[pos], p[pos+1], 0 };
            bytes.push_back(static_cast<uint8_t>(strtoul(buf, nullptr, 16)));
            pos += 2;
        }
    }
    return bytes;
}

bool MatchPattern(const uint8_t* data, size_t dataSize, size_t offset, const std::vector<uint8_t>& pattern) {
    if (offset + pattern.size() > dataSize) return false;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] != 0xFF && data[offset + i] != pattern[i]) return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: PatternScanner.exe <Frostpunk.exe>\n";
        return 1;
    }

    std::filesystem::path exePath(argv[1]);
    std::cout << "Scanning: " << exePath << "\n";

    auto data = ReadFile(exePath);
    std::cout << "File size: " << data.size() << " bytes\n";

    // Known patterns for key functions (from 1.6.1 analysis)
    Pattern patterns[] = {
        { "RVA_SET_SPEED", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA", 0x11BD080 },
        { "RVA_CHANGE_RESOURCE", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0x168E340 },
        { "RVA_PAUSE_ADD_REASON", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2 48 8B D9", 0xF6C530 },
        { "RVA_PAUSE_REMOVE_REASON", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0xF6CA80 },
        { "RVA_NATIVE_SAVE", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0x11B5F80 },
        { "RVA_NATIVE_LOAD", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0x11B5E30 },
        { "RVA_SELECT_MAP", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA", 0x1A8B880 },
        { "RVA_ENDLESS_START", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9", 0x1A8ADE0 },
    };

    auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(data.data());
    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(data.data() + dosHeader->e_lfanew);
    uintptr_t imageBase = ntHeaders->OptionalHeader.ImageBase;
    uintptr_t codeBase = imageBase + ntHeaders->OptionalHeader.BaseOfCode;
    size_t codeSize = ntHeaders->OptionalHeader.SizeOfCode;

    std::cout << "ImageBase: 0x" << std::hex << imageBase << "\n";
    std::cout << "CodeBase: 0x" << codeBase << ", Size: " << std::dec << codeSize << "\n\n";

    for (auto& pat : patterns) {
        auto patternBytes = ParsePattern(pat.pattern);
        bool found = false;
        for (size_t i = 0; i < codeSize - patternBytes.size(); i++) {
            if (MatchPattern(data.data(), data.size(),
                           ntHeaders->OptionalHeader.BaseOfCode + i, patternBytes)) {
                uintptr_t rva = ntHeaders->OptionalHeader.BaseOfCode + i;
                std::cout << pat.name << ": expected RVA 0x" << std::hex << pat.expectedRVA
                          << " -> found at RVA 0x" << rva
                          << (rva == pat.expectedRVA ? " MATCH" : " DIFFERENT") << "\n";
                found = true;
                break;
            }
        }
        if (!found) {
            std::cout << pat.name << ": expected RVA 0x" << std::hex << pat.expectedRVA << " -> NOT FOUND\n";
        }
    }

    return 0;
}