#include "HookManager.h"
#include "Memory.h"
#include "Common/Common.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>

namespace FrostMP::Game {

using FrostMP::TraceToFile;

// ============================================================
// ThreadSuspender - Safe thread suspension for hook install/remove
// ============================================================

bool HookManager::ThreadSuspender::SuspendAllExceptCurrent() {
    threads.clear();
    DWORD currentThreadId = GetCurrentThreadId();
    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snap) return false;

    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != currentThreadId) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h) {
                    if (SuspendThread(h) != (DWORD)-1) {
                        threads.push_back(h);
                    } else {
                        CloseHandle(h);
                    }
                }
            }
        } while (Thread32Next(snap, &te));
    }
    return true;
}

void HookManager::ThreadSuspender::ResumeAll() {
    for (HANDLE h : threads) {
        ResumeThread(h);
        CloseHandle(h);
    }
    threads.clear();
}

// ============================================================
// Minimal x64 Disassembler
// ============================================================

size_t HookManager::Disassembler::Decode(const u8* code, size_t maxLen, Instruction& out) {
    out = Instruction{};
    if (maxLen < 1) return 0;

    // REX prefix
    bool rex_w = false;
    size_t pos = 0;
    if (maxLen >= 1 && (code[0] & 0xF0) == 0x40) {
        rex_w = (code[0] & 0x08) != 0;
        pos = 1;
        if (maxLen < 2) return 0;
    }

    u8 opcode = code[pos];

    // 1-byte opcodes
    switch (opcode) {
        // CALL rel32 (E8 cd)
        case 0xE8:
        // JMP rel32 (E9 cd)
        case 0xE9:
            if (maxLen >= pos + 5) {
                out.length = pos + 5;
                out.isRelative = true;
                out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 1);
                out.relOffsetPos = pos + 1;
            }
            return out.length;

        // Short Jcc (70-7F) - 2 bytes
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            out.length = pos + 2;
            return out.length;

        // JMP short (EB cb)
        case 0xEB:
            out.length = pos + 2;
            return out.length;

        // RET (C3, C2, CA, CB)
        case 0xC3:
        case 0xCB:
            out.length = pos + 1;
            return out.length;
        case 0xC2:
        case 0xCA:
            out.length = pos + 3;
            return out.length;

        // INT3 (CC)
        case 0xCC:
            out.length = pos + 1;
            return out.length;

        // NOP (90)
        case 0x90:
            out.length = pos + 1;
            return out.length;

        // PUSH RAX-RDI (50-57)
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            out.length = pos + 1;
            return out.length;

        // POP RAX-RDI (58-5F)
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            out.length = pos + 1;
            return out.length;
    }

    // 2-byte opcodes (0F prefix)
    if (opcode == 0x0F) {
        if (maxLen < pos + 2) return 0;
        u8 opcode2 = code[pos + 1];

        // Long Jcc (0F 80-8F) - 6 bytes
        if (opcode2 >= 0x80 && opcode2 <= 0x8F) {
            if (maxLen >= pos + 6) {
                out.length = pos + 6;
                out.isRelative = true;
                out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 2);
                out.relOffsetPos = pos + 2;
            }
            return out.length;
        }
    }

    // ModR/M byte handling for common instructions
    if (maxLen >= pos + 2) {
        u8 modrm = code[pos + 1];
        u8 mod = (modrm >> 6) & 3;
        u8 reg = (modrm >> 3) & 7;
        u8 rm = modrm & 7;

        // Check for RIP-relative addressing (mod=00, rm=101)
        bool ripRelative = (mod == 0 && rm == 5);

        // MOV r64, imm64 (REX.W + B8+rd io) - 10 bytes
        if (rex_w && opcode >= 0xB8 && opcode <= 0xBF) {
            out.length = pos + 10;
            out.isRipRelative = false;
            return out.length;
        }

        // MOV r/m64, imm32 (C7 /0 id) - variable
        if (opcode == 0xC7 && reg == 0) {
            if (mod == 3) out.length = pos + 6;
            else if (mod == 0 && rm == 5) { out.length = pos + 10; out.isRipRelative = true; out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 2); out.relOffsetPos = pos + 2; }
            else out.length = pos + 6;
            return out.length;
        }

        // LEA r64, m (8D /r) - RIP-relative possible
        if (opcode == 0x8D) {
            if (ripRelative) {
                out.length = pos + 7;
                out.isRipRelative = true;
                out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 3);
                out.relOffsetPos = pos + 3;
            } else {
                out.length = pos + 3;
                if (mod == 1) out.length += 1;
                else if (mod == 2) out.length += 4;
            }
            return out.length;
        }

        // MOV r64, r/m64 (8B /r) and MOV r/m64, r64 (89 /r)
        if (opcode == 0x8B || opcode == 0x89) {
            if (ripRelative) {
                out.length = pos + 7;
                out.isRipRelative = true;
                out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 3);
                out.relOffsetPos = pos + 3;
            } else {
                out.length = pos + 3;
                if (mod == 1) out.length += 1;
                else if (mod == 2) out.length += 4;
            }
            return out.length;
        }

        // CMP r64, r/m64 (3B /r), TEST, etc.
        if (opcode == 0x3B || opcode == 0x39 || opcode == 0x85) {
            if (ripRelative) {
                out.length = pos + 7;
                out.isRipRelative = true;
                out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 3);
                out.relOffsetPos = pos + 3;
            } else {
                out.length = pos + 3;
                if (mod == 1) out.length += 1;
                else if (mod == 2) out.length += 4;
            }
            return out.length;
        }

        // ADD/SUB/AND/OR/XOR r/m64, imm8/32 (80-83 /0-7)
        if (opcode >= 0x80 && opcode <= 0x83) {
            if (mod == 3) out.length = pos + 3 + (opcode & 1 ? 4 : 1);
            else if (ripRelative) { out.length = pos + 7 + (opcode & 1 ? 4 : 1); out.isRipRelative = true; out.relOffset = *reinterpret_cast<const int32_t*>(code + pos + 3); out.relOffsetPos = pos + 3; }
            else out.length = pos + 3 + (opcode & 1 ? 4 : 1);
            return out.length;
        }
    }

    // Default fallback - treat as 1 byte
    out.length = 1;
    return 1;
}

bool HookManager::Disassembler::IsRelativeControlFlow(const u8* code, size_t len, int32_t& relOffset, size_t& relOffsetPos) {
    if (len < 1) return false;
    u8 opcode = code[0];

    if (opcode == 0xE8 && len >= 5) {
        relOffset = *reinterpret_cast<const int32_t*>(code + 1);
        relOffsetPos = 1;
        return true;
    }
    if (opcode == 0xE9 && len >= 5) {
        relOffset = *reinterpret_cast<const int32_t*>(code + 1);
        relOffsetPos = 1;
        return true;
    }
    if ((opcode >= 0x70 && opcode <= 0x7F) && len >= 2) {
        relOffset = static_cast<int8_t>(code[1]);
        relOffsetPos = 1;
        return true;
    }
    if (opcode == 0xEB && len >= 2) {
        relOffset = static_cast<int8_t>(code[1]);
        relOffsetPos = 1;
        return true;
    }
    if (opcode == 0x0F && len >= 6) {
        u8 opcode2 = code[1];
        if (opcode2 >= 0x80 && opcode2 <= 0x8F) {
            relOffset = *reinterpret_cast<const int32_t*>(code + 2);
            relOffsetPos = 2;
            return true;
        }
    }
    return false;
}

bool HookManager::Disassembler::IsRipRelative(const u8* code, size_t len, int32_t& dispOffset, size_t& dispPos) {
    Instruction insn;
    size_t decoded = Decode(code, len, insn);
    if (decoded > 0 && insn.isRipRelative) {
        dispOffset = insn.relOffset;
        dispPos = insn.relOffsetPos;
        return true;
    }
    return false;
}

size_t HookManager::Disassembler::Relocate(const u8* src, u8* dst, size_t maxDst,
                                         std::uintptr_t srcAddr, std::uintptr_t dstAddr) {
    Instruction insn;
    size_t len = Decode(src, maxDst, insn);
    if (len == 0 || len > maxDst) return 0;

    std::memcpy(dst, src, len);

    if (insn.isRelative) {
        int64_t originalTarget = static_cast<int64_t>(srcAddr) + insn.relOffsetPos + 4 + insn.relOffset;
        int32_t newOffset = static_cast<int32_t>(originalTarget - (static_cast<int64_t>(dstAddr) + insn.relOffsetPos + 4));
        *reinterpret_cast<int32_t*>(dst + insn.relOffsetPos) = newOffset;
    }

    if (insn.isRipRelative) {
        int64_t originalEA = static_cast<int64_t>(srcAddr) + insn.relOffsetPos + 4 + insn.relOffset;
        int32_t newDisp = static_cast<int32_t>(originalEA - (static_cast<int64_t>(dstAddr) + insn.relOffsetPos + 4));
        *reinterpret_cast<int32_t*>(dst + insn.relOffsetPos) = newDisp;
    }

    return len;
}

// ============================================================
// HookManager Implementation
// ============================================================

HookManager::HookManager(const LocalMemory& mem) : mem_(mem) {
    hooks_.fill(Hook{});
}

HookManager::~HookManager() {
    RemoveAllHooks();
}

bool HookManager::AllocateHookIndex(size_t& outIndex) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    for (size_t i = 0; i < MAX_HOOKS; ++i) {
        if (hooks_[i].name.empty() && !hooks_[i].installing) {
            hooks_[i].installing = true;
            outIndex = i;
            return true;
        }
    }
    return false;
}

void HookManager::FreeHookIndex(size_t index) {
    if (index < MAX_HOOKS) {
        hooks_[index] = Hook{};
        hooks_[index].installing = false;
    }
}

size_t HookManager::FindPrologueSize(std::uintptr_t target, size_t minSize) {
    std::vector<u8> buffer(64);
    if (!mem_.Read(target, buffer.data(), buffer.size())) {
        return minSize;
    }

    size_t total = 0;
    while (total < minSize && total < buffer.size()) {
        Disassembler::Instruction insn;
        size_t len = Disassembler::Decode(buffer.data() + total, buffer.size() - total, insn);
        if (len == 0) break;
        total += len;
    }
    return std::max(total, minSize);
}

void* HookManager::BuildTrampoline(std::uintptr_t target, const std::vector<u8>& originalBytes, size_t prologueSize) {
    void* trampolineMem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampolineMem) return nullptr;

    u8* trampoline = static_cast<u8*>(trampolineMem);
    u8* trampolinePtr = trampoline;

    size_t copied = 0;
    while (copied < prologueSize && copied < originalBytes.size()) {
        Disassembler::Instruction insn;
        size_t remaining = originalBytes.size() - copied;
        size_t len = Disassembler::Decode(originalBytes.data() + copied, remaining, insn);
        if (len == 0) len = 1;
        if (copied + len > prologueSize) break;

        size_t relocated = Disassembler::Relocate(
            originalBytes.data() + copied,
            trampolinePtr,
            4096 - (trampolinePtr - trampoline),
            target + copied,
            reinterpret_cast<std::uintptr_t>(trampolinePtr)
        );
        if (relocated == 0) {
            VirtualFree(trampolineMem, 0, MEM_RELEASE);
            return nullptr;
        }

        trampolinePtr += relocated;
        copied += len;
    }

    // JMP back to original function after prologue
    if (trampolinePtr + 14 <= trampoline + 4096) {
        trampolinePtr[0] = 0xFF; trampolinePtr[1] = 0x25;
        *reinterpret_cast<uint32_t*>(&trampolinePtr[2]) = 0;
        *reinterpret_cast<std::uintptr_t*>(&trampolinePtr[6]) = target + prologueSize;
        trampolinePtr += 14;
    } else {
        VirtualFree(trampolineMem, 0, MEM_RELEASE);
        return nullptr;
    }

    DWORD oldProtect;
    VirtualProtect(trampolineMem, 4096, PAGE_EXECUTE_READ, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), trampolineMem, trampolinePtr - trampoline);

    return trampolineMem;
}

void* HookManager::BuildDetour(size_t hookIndex, void* trampoline) {
    void* detourMem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!detourMem) return nullptr;

    u8* detour = static_cast<u8*>(detourMem);

    std::vector<u8> detourCode;
    detourCode.reserve(128);

    // sub rsp, 0x68 (104 bytes, 104%16==8, so RSP%16 goes from 8 to 0 — aligned before call rax)
    detourCode.insert(detourCode.end(), {0x48, 0x83, 0xEC, 0x68});

    // Save volatile registers
    detourCode.insert(detourCode.end(), {0x48, 0x89, 0x4C, 0x24, 0x20}); // rcx
    detourCode.insert(detourCode.end(), {0x48, 0x89, 0x54, 0x24, 0x28}); // rdx
    detourCode.insert(detourCode.end(), {0x4C, 0x89, 0x44, 0x24, 0x30}); // r8
    detourCode.insert(detourCode.end(), {0x4C, 0x89, 0x4C, 0x24, 0x38}); // r9

    // Save return address
    detourCode.insert(detourCode.end(), {0x48, 0x8D, 0x44, 0x24, 0x68}); // lea rax, [rsp+0x68] (return address in original stack)
    detourCode.insert(detourCode.end(), {0x48, 0x89, 0x44, 0x24, 0x40}); // mov [rsp+0x40], rax (save return address)

    // RCX = return address
    detourCode.insert(detourCode.end(), {0x48, 0x8B, 0x4C, 0x24, 0x68}); // mov rcx, [rsp+0x68] (load return address VALUE)
    // RDX = context (saved args)
    detourCode.insert(detourCode.end(), {0x48, 0x8D, 0x54, 0x24, 0x20});
    // R8 = userData (placeholder)
    detourCode.push_back(0x49); detourCode.push_back(0xB8);
    size_t userDataPos = detourCode.size();
    detourCode.resize(detourCode.size() + 8);
    *reinterpret_cast<std::uintptr_t*>(&detourCode[userDataPos]) = 0;

    // Call StaticCallback<hookIndex>
    detourCode.push_back(0x48); detourCode.push_back(0xB8);
    size_t callbackPos = detourCode.size();
    detourCode.resize(detourCode.size() + 8);
    *reinterpret_cast<std::uintptr_t*>(&detourCode[callbackPos]) = 0;

    // call rax
    detourCode.insert(detourCode.end(), {0xFF, 0xD0});

    // test al, al
    detourCode.insert(detourCode.end(), {0x84, 0xC0});

    // je skip_trampoline — offset from next instruction (pos 70) to skip_trampoline (pos 106) = 36 = 0x24
    detourCode.insert(detourCode.end(), {0x74, 0x24});

    // Restore registers and jump to trampoline
    detourCode.insert(detourCode.end(), {0x48, 0x8B, 0x4C, 0x24, 0x20});
    detourCode.insert(detourCode.end(), {0x48, 0x8B, 0x54, 0x24, 0x28});
    detourCode.insert(detourCode.end(), {0x4C, 0x8B, 0x44, 0x24, 0x30});
    detourCode.insert(detourCode.end(), {0x4C, 0x8B, 0x4C, 0x24, 0x38});

    // Restore stack pointer (undo sub rsp, 0x68 at detour entry)
    detourCode.insert(detourCode.end(), {0x48, 0x83, 0xC4, 0x68}); // add rsp, 0x68 (undo sub rsp, 0x68)

    // mov rax, trampoline
    detourCode.push_back(0x48); detourCode.push_back(0xB8);
    size_t trampolinePos = detourCode.size();
    detourCode.resize(detourCode.size() + 8);
    *reinterpret_cast<std::uintptr_t*>(&detourCode[trampolinePos]) = reinterpret_cast<std::uintptr_t>(trampoline);

    // jmp rax
    detourCode.insert(detourCode.end(), {0xFF, 0xE0});

    // skip_trampoline: add rsp, 0x68; ret (must match sub rsp, 0x68 at entry)
    detourCode.insert(detourCode.end(), {0x48, 0x83, 0xC4, 0x68, 0xC3});

    // Patch placeholders
    *reinterpret_cast<std::uintptr_t*>(&detourCode[userDataPos]) = reinterpret_cast<std::uintptr_t>(hooks_[hookIndex].userData);

    using CallbackFn = bool(*)(std::uintptr_t, void*, void*);
    CallbackFn callbackFn = nullptr;
    switch (hookIndex) {
        #define CASE_HOOK(N) case N: callbackFn = &StaticCallback<N>; break
        CASE_HOOK(0); CASE_HOOK(1); CASE_HOOK(2); CASE_HOOK(3); CASE_HOOK(4);
        CASE_HOOK(5); CASE_HOOK(6); CASE_HOOK(7); CASE_HOOK(8); CASE_HOOK(9);
        CASE_HOOK(10); CASE_HOOK(11); CASE_HOOK(12); CASE_HOOK(13); CASE_HOOK(14);
        CASE_HOOK(15); CASE_HOOK(16); CASE_HOOK(17); CASE_HOOK(18); CASE_HOOK(19);
        CASE_HOOK(20); CASE_HOOK(21); CASE_HOOK(22); CASE_HOOK(23); CASE_HOOK(24);
        CASE_HOOK(25); CASE_HOOK(26); CASE_HOOK(27); CASE_HOOK(28); CASE_HOOK(29);
        CASE_HOOK(30); CASE_HOOK(31);
        #undef CASE_HOOK
    }
    *reinterpret_cast<std::uintptr_t*>(&detourCode[callbackPos]) = reinterpret_cast<std::uintptr_t>(callbackFn);

    if (detourCode.size() > 4096) {
        VirtualFree(detourMem, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(detour, detourCode.data(), detourCode.size());

    DWORD oldProtect;
    VirtualProtect(detourMem, 4096, PAGE_EXECUTE_READ, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), detourMem, detourCode.size());

    return detourMem;
}

bool HookManager::WriteProtected(std::uintptr_t address, const void* data, size_t size) {
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DebugLog("WriteProtected: VirtualProtect failed at 0x%llX, error=%lu", address, GetLastError());
        return false;
    }

    char wprotMsg[128];
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
        snprintf(wprotMsg, sizeof(wprotMsg), "WriteProtected: SEH exception writing to 0x%llX", address);
        TraceToFile(wprotMsg);
        return false;
    }

    if (!VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect)) {
        DebugLog("WriteProtected: VirtualProtect restore failed at 0x%llX", address);
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return true;
}

bool HookManager::VerifyWrite(std::uintptr_t address, const void* expected, size_t size) {
    std::vector<u8> actual(size);
    if (!mem_.Read(address, actual.data(), size)) return false;
    return std::memcmp(actual.data(), expected, size) == 0;
}

bool HookManager::InstallCallbackHook(const std::string& name, std::uintptr_t targetRVA,
                                      HookCallback callback, void* userData) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);

    auto it = nameToIndex_.find(name);
    if (it != nameToIndex_.end()) {
        DebugLog("InstallCallbackHook: Hook '%s' already exists", name.c_str());
        return false;
    }

    std::uintptr_t target = mem_.ResolveRVALocal(targetRVA);
    if (!target) {
        DebugLog("InstallCallbackHook: Failed to resolve RVA 0x%llX for %s", targetRVA, name.c_str());
        return false;
    }

    // Use minSize=32 to guarantee we copy whole instructions (never split one in half).
    // minSize=14 could truncate a short first instruction and split the next one.
    size_t prologueSize = FindPrologueSize(target, 32);
    DebugLog("InstallCallbackHook: %s at 0x%llX, prologueSize=%zu", name.c_str(), target, prologueSize);

    std::vector<u8> original(prologueSize);
    if (!mem_.ReadLocalRaw(target, original.data(), prologueSize)) {
        DebugLog("InstallCallbackHook: Failed to read original bytes at 0x%llX", target);
        return false;
    }

    size_t hookIndex;
    if (!AllocateHookIndex(hookIndex)) {
        DebugLog("InstallCallbackHook: No free hook slots");
        return false;
    }

    Hook& hook = hooks_[hookIndex];
    hook.name = name;
    hook.address = target;
    hook.originalBytes = std::move(original);
    hook.prologueSize = prologueSize;
    hook.callback = callback;
    hook.userData = userData;
    hook.enabled = false;
    hook.installing = false;

    hook.trampoline = BuildTrampoline(target, hook.originalBytes, prologueSize);
    if (!hook.trampoline) {
        DebugLog("InstallCallbackHook: BuildTrampoline failed for %s", name.c_str());
        FreeHookIndex(hookIndex);
        return false;
    }

    hook.detour = BuildDetour(hookIndex, hook.trampoline);
    if (!hook.detour) {
        DebugLog("InstallCallbackHook: BuildDetour failed for %s", name.c_str());
        VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        FreeHookIndex(hookIndex);
        return false;
    }

    hook.hookBytes.resize(14);
    hook.hookBytes[0] = 0xFF; hook.hookBytes[1] = 0x25;
    *reinterpret_cast<uint32_t*>(&hook.hookBytes[2]) = 0;
    *reinterpret_cast<std::uintptr_t*>(&hook.hookBytes[6]) = reinterpret_cast<std::uintptr_t>(hook.detour);

    ThreadSuspender suspender;
    if (!suspender.SuspendAllExceptCurrent()) {
        DebugLog("InstallCallbackHook: Thread suspension failed for %s", name.c_str());
        VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        VirtualFree(hook.detour, 0, MEM_RELEASE);
        FreeHookIndex(hookIndex);
        return false;
    }

    bool ok = WriteProtected(target, hook.hookBytes.data(), hook.hookBytes.size());
    if (ok) {
        ok = VerifyWrite(target, hook.hookBytes.data(), hook.hookBytes.size());
    }

    suspender.ResumeAll();

    if (!ok) {
        DebugLog("InstallCallbackHook: Write/verify failed for %s at 0x%llX", name.c_str(), target);
        VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        VirtualFree(hook.detour, 0, MEM_RELEASE);
        FreeHookIndex(hookIndex);
        return false;
    }

    hook.enabled = true;
    nameToIndex_[name] = hookIndex;
    DebugLog("InstallCallbackHook: Installed %s at 0x%llX (trampoline=%p, detour=%p)",
             name.c_str(), target, hook.trampoline, hook.detour);
    return true;
}

bool HookManager::InstallHook(const std::string& name, std::uintptr_t targetRVA, void* detour) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);

    auto it = nameToIndex_.find(name);
    if (it != nameToIndex_.end()) {
        DebugLog("InstallHook: Hook '%s' already exists", name.c_str());
        return false;
    }

    std::uintptr_t target = mem_.ResolveRVALocal(targetRVA);
    if (!target) {
        DebugLog("InstallHook: Failed to resolve RVA 0x%llX for %s", targetRVA, name.c_str());
        return false;
    }

    size_t hookSize = 14;
    size_t prologueSize = FindPrologueSize(target, hookSize);

    std::vector<u8> original(prologueSize);
    if (!mem_.ReadLocalRaw(target, original.data(), prologueSize)) {
        DebugLog("InstallHook: Failed to read original bytes at 0x%llX", target);
        return false;
    }

    std::vector<u8> jmp(14);
    jmp[0] = 0xFF; jmp[1] = 0x25;
    *reinterpret_cast<uint32_t*>(&jmp[2]) = 0;
    *reinterpret_cast<std::uintptr_t*>(&jmp[6]) = reinterpret_cast<std::uintptr_t>(detour);

    size_t hookIndex;
    if (!AllocateHookIndex(hookIndex)) {
        DebugLog("InstallHook: No free hook slots");
        return false;
    }

    Hook& hook = hooks_[hookIndex];
    hook.name = name;
    hook.address = target;
    hook.originalBytes = std::move(original);
    hook.hookBytes = std::move(jmp);
    hook.prologueSize = prologueSize;
    hook.enabled = false;
    hook.installing = false;

    ThreadSuspender suspender;
    if (!suspender.SuspendAllExceptCurrent()) {
        DebugLog("InstallHook: Thread suspension failed for %s", name.c_str());
        FreeHookIndex(hookIndex);
        return false;
    }

    bool ok = WriteProtected(target, hook.hookBytes.data(), hook.hookBytes.size());
    if (ok) {
        ok = VerifyWrite(target, hook.hookBytes.data(), hook.hookBytes.size());
    }

    suspender.ResumeAll();

    if (!ok) {
        DebugLog("InstallHook: Write/verify failed for %s at 0x%llX", name.c_str(), target);
        FreeHookIndex(hookIndex);
        return false;
    }

    hook.enabled = true;
    nameToIndex_[name] = hookIndex;
    DebugLog("InstallHook: Installed %s at 0x%llX", name.c_str(), target);
    return true;
}

bool HookManager::EnableHook(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    auto it = nameToIndex_.find(name);
    if (it == nameToIndex_.end()) return false;

    Hook& hook = hooks_[it->second];
    if (hook.enabled) return false;

    ThreadSuspender suspender;
    if (!suspender.SuspendAllExceptCurrent()) {
        DebugLog("EnableHook: Thread suspension failed for %s", name.c_str());
        return false;
    }

    bool ok = WriteProtected(hook.address, hook.hookBytes.data(), hook.hookBytes.size());
    if (ok) {
        ok = VerifyWrite(hook.address, hook.hookBytes.data(), hook.hookBytes.size());
    }

    suspender.ResumeAll();

    if (ok) {
        hook.enabled = true;
        DebugLog("EnableHook: Enabled %s at 0x%llX", name.c_str(), hook.address);
    } else {
        DebugLog("EnableHook: Failed to enable %s", name.c_str());
    }
    return ok;
}

bool HookManager::DisableHook(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    auto it = nameToIndex_.find(name);
    if (it == nameToIndex_.end()) return false;

    Hook& hook = hooks_[it->second];
    if (!hook.enabled) return false;

    ThreadSuspender suspender;
    if (!suspender.SuspendAllExceptCurrent()) {
        DebugLog("DisableHook: Thread suspension failed for %s", name.c_str());
        return false;
    }

    bool ok = WriteProtected(hook.address, hook.originalBytes.data(), hook.originalBytes.size());
    if (ok) {
        ok = VerifyWrite(hook.address, hook.originalBytes.data(), hook.originalBytes.size());
    }

    suspender.ResumeAll();

    if (ok) {
        hook.enabled = false;
        DebugLog("DisableHook: Disabled %s", name.c_str());
    } else {
        DebugLog("DisableHook: Failed to disable %s", name.c_str());
    }
    return ok;
}

void HookManager::RemoveHook(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    auto it = nameToIndex_.find(name);
    if (it == nameToIndex_.end()) return;

    size_t index = it->second;
    Hook& hook = hooks_[index];

    DisableHook(name);

    if (hook.trampoline) {
        VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        hook.trampoline = nullptr;
    }
    if (hook.detour) {
        VirtualFree(hook.detour, 0, MEM_RELEASE);
        hook.detour = nullptr;
    }

    nameToIndex_.erase(it);
    FreeHookIndex(index);
    DebugLog("RemoveHook: Removed %s", name.c_str());
}

void HookManager::RemoveAllHooks() {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    for (auto& [name, index] : nameToIndex_) {
        Hook& hook = hooks_[index];
        if (hook.enabled) {
            ThreadSuspender suspender;
            if (suspender.SuspendAllExceptCurrent()) {
                WriteProtected(hook.address, hook.originalBytes.data(), hook.originalBytes.size());
                suspender.ResumeAll();
            }
            hook.enabled = false;
        }
        if (hook.trampoline) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
        }
        if (hook.detour) {
            VirtualFree(hook.detour, 0, MEM_RELEASE);
            hook.detour = nullptr;
        }
    }
    nameToIndex_.clear();
    for (size_t i = 0; i < MAX_HOOKS; ++i) {
        hooks_[i] = Hook{};
    }
    DebugLog("RemoveAllHooks: All hooks removed");
}

bool HookManager::IsHooked(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    auto it = nameToIndex_.find(name);
    return it != nameToIndex_.end() && hooks_[it->second].enabled;
}

void* HookManager::GetTrampoline(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(hooksMutex_);
    auto it = nameToIndex_.find(name);
    if (it == nameToIndex_.end()) return nullptr;
    return hooks_[it->second].trampoline;
}

// SEH wrapper for invoking hook callbacks safely. No C++ objects with
// destructors inside this function so __try/__except compiles cleanly.
// NOTE: Only TraceToFile is used here — DebugLog uses std::ofstream with heap
// allocations which is unsafe from inside an exception handler.
static bool InvokeCallbackSafely(HookCallback callback, std::uintptr_t returnAddress,
                                  void* context, void* userData,
                                  const char* hookName, size_t hookIndex) {
    char crashMsg[256];
    snprintf(crashMsg, sizeof(crashMsg), "CRASH in hook '%s' (index=%zu)!", hookName, hookIndex);
    __try {
        return callback(returnAddress, context, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TraceToFile(crashMsg);
        return true;
    }
}

// Static callback trampolines - wrapped in SEH to catch crashes from any hook.
// Note: __try cannot contain objects with destructors (e.g. lock_guard), so we
// copy the hook pointer under lock first, then delegate to InvokeCallbackSafely.
template<size_t N>
bool HookManager::StaticCallback(std::uintptr_t returnAddress, void* context, void* userData) {
    if (!g_hookManager) return true;

    Hook* hookPtr = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_hookManager->hooksMutex_);
        for (const auto& [name, index] : g_hookManager->nameToIndex_) {
            if (index == N) {
                hookPtr = &g_hookManager->hooks_[index];
                break;
            }
        }
    }

    if (hookPtr && hookPtr->callback) {
        return InvokeCallbackSafely(hookPtr->callback, returnAddress, context, hookPtr->userData,
                                     hookPtr->name.c_str(), N);
    }
    return true;
}

// Explicit instantiations
#define INSTANTIATE(N) template bool HookManager::StaticCallback<N>(std::uintptr_t, void*, void*);
INSTANTIATE(0) INSTANTIATE(1) INSTANTIATE(2) INSTANTIATE(3) INSTANTIATE(4)
INSTANTIATE(5) INSTANTIATE(6) INSTANTIATE(7) INSTANTIATE(8) INSTANTIATE(9)
INSTANTIATE(10) INSTANTIATE(11) INSTANTIATE(12) INSTANTIATE(13) INSTANTIATE(14)
INSTANTIATE(15) INSTANTIATE(16) INSTANTIATE(17) INSTANTIATE(18) INSTANTIATE(19)
INSTANTIATE(20) INSTANTIATE(21) INSTANTIATE(22) INSTANTIATE(23) INSTANTIATE(24)
INSTANTIATE(25) INSTANTIATE(26) INSTANTIATE(27) INSTANTIATE(28) INSTANTIATE(29)
INSTANTIATE(30) INSTANTIATE(31)
#undef INSTANTIATE

// Global hook manager instance
HookManager* g_hookManager = nullptr;

} // namespace FrostMP::Game