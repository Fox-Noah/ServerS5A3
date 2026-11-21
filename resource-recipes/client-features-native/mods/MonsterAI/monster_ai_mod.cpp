// Monster AI tuning as a runtime plugin, loaded by our RS.dll into mods\*.dll.
// Replaces the build-time static patches in build_monster_tuning_v208.py.
//
// Pinned to the original V208 image: SHA256 5ce2ce3f... / ImageBase 0x00400000.
// Every write is guarded by opcode fingerprints; if the image differs, no hook
// is installed and the game runs untouched.

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "kernel32.lib")

extern "C" void MonsterTuningStartMapV208();
extern "C" void MonsterTuningSpawnV208();
extern "C" { extern void* g_pOriginalStartMap; }

namespace {

constexpr std::uintptr_t kImageBase = 0x00400000;
constexpr std::uintptr_t kStartMapSlot = 0x02001F84;
constexpr std::uintptr_t kSpawnPatch = 0x02628EDD;
constexpr std::uint32_t kOriginalCallback = 0x01FF8D80;
constexpr unsigned char kOriginalSpawn[8] = {0x8B, 0x06, 0x8B, 0x90, 0x34, 0x04, 0x00, 0x00};
// Complete instruction boundary at the callback entry. Overwriting exactly
// these five bytes leaves the following `push imm32` intact.
constexpr unsigned char kCallbackPrologue[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};

struct Fingerprint {
    std::uintptr_t va;
    const unsigned char* bytes;
    std::size_t size;
};

// Same registration points the static packager verifies.
static const unsigned char fp_startmap_call[] = {
    0x6a, 0x00, 0x68, 0x80, 0x8d, 0xff, 0x01, 0x6a,
    0x1d, 0x8b, 0xce, 0xe8, 0x0f, 0x23, 0x52, 0x00,
};
static const unsigned char fp_callback_body[] = {
    0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x45, 0x57,
    0x0f, 0x04, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x81, 0xec,
};
static const unsigned char fp_packet_reader[] = {
    0x55, 0x8b, 0xec, 0xa1, 0xd0, 0x9a, 0x7f, 0x05,
    0x8a, 0x08, 0x8b, 0x55, 0x08, 0x40, 0xff, 0x0d,
    0xd4, 0x9a, 0x7f, 0x05,
};

constexpr Fingerprint kFingerprints[] = {
    {0x02001F81, fp_startmap_call, sizeof(fp_startmap_call)},
    {0x01FF8D80, fp_callback_body, sizeof(fp_callback_body)},
    {0x037E82E0, fp_packet_reader, sizeof(fp_packet_reader)},
};

void LogLine(const char* text)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    else exe[0] = 0;

    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%smonster_ai.log", exe);
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"a") == 0 && f) {
        fputs(text, f);
        fputc('\n', f);
        fclose(f);
    }
}

bool FingerprintsMatch()
{
    for (const auto& fp : kFingerprints) {
        if (std::memcmp(reinterpret_cast<const void*>(fp.va), fp.bytes, fp.size) != 0) {
            return false;
        }
    }
    if (*reinterpret_cast<const std::uint32_t*>(kStartMapSlot) != kOriginalCallback) {
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kSpawnPatch),
                    kOriginalSpawn, sizeof(kOriginalSpawn)) != 0) {
        return false;
    }
    return true;
}

bool WriteHook(void* address, const void* bytes, std::size_t size)
{
    DWORD oldProtect = 0;
    DWORD unused = 0;
    if (!VirtualProtect(address, size, PAGE_READWRITE, &oldProtect)) return false;
    std::memcpy(address, bytes, size);
    VirtualProtect(address, size, oldProtect, &unused);
    return true;
}

bool InstallStartMapHook()
{
    // The registration instruction at 0x02001F81 runs during early startup,
    // before this DLL can be loaded. Redirect the callback entry instead so
    // the already-registered dispatcher reaches our hook on every START_MAP.
    void* trampoline = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!trampoline) {
        LogLine("MonsterAI: START_MAP trampoline alloc failed");
        return false;
    }
    auto* code = static_cast<unsigned char*>(trampoline);
    std::memcpy(code, kCallbackPrologue, sizeof(kCallbackPrologue));
    code[5] = 0xE9;
    const std::uintptr_t resume =
        static_cast<std::uintptr_t>(kOriginalCallback) + sizeof(kCallbackPrologue);
    const std::int32_t back = static_cast<std::int32_t>(
        resume - (reinterpret_cast<std::uintptr_t>(code) + sizeof(kCallbackPrologue) + 5));
    std::memcpy(code + 6, &back, sizeof(back));

    DWORD oldProtect = 0;
    if (!VirtualProtect(trampoline, 16, PAGE_EXECUTE_READ, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        LogLine("MonsterAI: START_MAP trampoline protect failed");
        return false;
    }
    g_pOriginalStartMap = trampoline;

    unsigned char jump[5] = {0xE9, 0, 0, 0, 0};
    const std::int32_t relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(&MonsterTuningStartMapV208) -
        (static_cast<std::uintptr_t>(kOriginalCallback) + sizeof(jump)));
    std::memcpy(jump + 1, &relative, sizeof(relative));
    if (!WriteHook(reinterpret_cast<void*>(kOriginalCallback), jump, sizeof(jump))) {
        LogLine("MonsterAI: START_MAP callback hook write failed");
        return false;
    }
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl DnfModInit()
{
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL)) != kImageBase) {
        LogLine("MonsterAI: unexpected exe image base, hooks skipped");
        return;
    }
    if (!FingerprintsMatch()) {
        LogLine("MonsterAI: exe fingerprints differ from pinned V208, hooks skipped");
        return;
    }

    if (!InstallStartMapHook()) {
        return;
    }

    // Monster spawn: original 8 bytes -> E9 rel32 + 3 nops.
    unsigned char spawnJump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const std::intptr_t relative =
        reinterpret_cast<std::intptr_t>(&MonsterTuningSpawnV208) -
        (static_cast<std::intptr_t>(kSpawnPatch) + 5);
    std::memcpy(spawnJump + 1, &relative, sizeof(relative));
    if (!WriteHook(reinterpret_cast<void*>(kSpawnPatch), spawnJump, sizeof(spawnJump))) {
        LogLine("MonsterAI: spawn patch write failed");
        return;
    }

    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    LogLine("MonsterAI: hooks installed (START_MAP callback + spawn)");
}
