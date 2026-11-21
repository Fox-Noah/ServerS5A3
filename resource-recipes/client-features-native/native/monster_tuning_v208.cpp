// This adapter is for the pinned v208 PE32 image only. A candidate packager
// must verify the complete original image digest and opcode registration.
#include "monster_tuning.h"
#include "v208_dungeon_monster.h"

// Diagnostic logging is compiled only into the DLL plugin build (which has a
// CRT). The static payload build defines MONSTER_TUNING_NO_CRT and stays clean.
#ifndef MONSTER_TUNING_NO_CRT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <cstdarg>
#include <cwchar>
#endif

// The additive payload uses no CRT. MSVC emits this marker for binary64 data.
#ifdef MONSTER_TUNING_NO_CRT
extern "C" { int _fltused = 0; }
#endif

// Runtime plugin hook target. The static packager patches the registration
// immediate in the EXE before launch; a DLL loaded after startup must instead
// redirect the original callback entry itself and tail-call a trampoline that
// replays the original prologue.
extern "C" void* g_pOriginalStartMap = nullptr;

namespace {

monster_tuning::RoomRules roomRules;

#ifndef MONSTER_TUNING_NO_CRT
void DiagLog(const char* fmt, ...) {
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
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fputc('\n', f);
        fclose(f);
    }
}
#endif

std::int64_t TruncateToInt64(double scaled) noexcept {
    // The payload links without the MSVC runtime. Preserve the client's x87
    // control word while doing the truncation required by integer conversion.
    std::uint16_t originalControl;
    std::uint16_t truncateControl;
    std::int64_t result;
    __asm {
        fnstcw originalControl
        mov ax, originalControl
        or ax, 0c00h
        mov truncateControl, ax
        fldcw truncateControl
        fld scaled
        fistp result
        fldcw originalControl
    }
    return result;
}

std::int64_t ScalePositiveHealth(std::int64_t health, double multiplier) noexcept {
    const double scaled = static_cast<double>(health) * multiplier;
    if (scaled >= 9223372036854775808.0) {
        return (std::numeric_limits<std::int64_t>::max)();
    }
    if (scaled < 1.0) {
        return 1;
    }

    return TruncateToInt64(scaled);
}

void ScaleMonsterScalar(std::uint8_t* field, double rate) noexcept {
    if (rate == 1.0) {
        return;
    }
    const auto value = v208::ReadScalar(field);
    const auto scaled = value * rate;
    constexpr auto maxValue = (std::numeric_limits<std::int32_t>::max)();
    constexpr auto minValue = (std::numeric_limits<std::int32_t>::min)();
    const auto result = scaled >= maxValue ? maxValue :
        scaled <= minValue ? minValue : static_cast<std::int32_t>(TruncateToInt64(scaled));
    if (result == value) {
        return;
    }
    v208::WriteScalar(field, result);
}
}  // namespace

extern "C" __declspec(noinline) void __cdecl PrepareMonsterTuningStartMap() noexcept {
    const auto packet = v208::ActivePacketReader();
#ifndef MONSTER_TUNING_NO_CRT
    static unsigned long startMapCalls = 0;
    ++startMapCalls;
    const bool hasMat1 = packet.cursor && packet.remaining >= 24 &&
        std::memcmp(packet.cursor + packet.remaining - 4, "MAT1", 4) == 0;
    DiagLog("START_MAP #%lu: remaining=%d cursor=%p MAT1=%s",
            startMapCalls, packet.remaining, (const void*)packet.cursor,
            hasMat1 ? "yes" : "no");
#endif
    monster_tuning::PrepareStartMap(roomRules, packet.cursor, packet.remaining);
#ifndef MONSTER_TUNING_NO_CRT
    const auto probe = roomRules.ForLevel(1);
    DiagLog("START_MAP #%lu: active=%s hp=%g def=%g atk=%g remainAfter=%d",
            startMapCalls, probe.hp != 1.0 ? "yes" : "no",
            probe.hp, probe.defense, probe.attack, packet.remaining);
#endif
}

extern "C" __declspec(dllexport) void __cdecl
CopyMonsterTuningRatesV208(std::uint8_t level, monster_tuning::Multipliers* rates) noexcept {
    *rates = roomRules.ForLevel(level);
}

static void ScaleMonsterHealthField(std::uint8_t* field, double multiplier) noexcept {
    if (multiplier == 1.0) {
        return;
    }
    const auto health = v208::ReadHealth(field);
    if (health <= 0) {
        return;
    }
    const auto scaled = ScalePositiveHealth(health, multiplier);
    if (scaled != health) {
        v208::WriteHealth(field, scaled);
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void MonsterTuningStartMapV208() {
    __asm {
        pushfd
        pushad
        call PrepareMonsterTuningStartMap
        popad
        popfd
        push dword ptr [g_pOriginalStartMap]
        ret
    }
}

extern "C" __declspec(noinline) void __cdecl ApplyMonsterSpawnTuning(void* actor) noexcept {
    const v208::MonsterView monster(actor);
#ifndef MONSTER_TUNING_NO_CRT
    static unsigned long spawnCalls = 0;
    const bool supported = monster.HasSupportedLayout();
    if (++spawnCalls <= 12) {
        DiagLog("SPAWN #%lu: actor=%p layout=%s", spawnCalls, actor,
                supported ? "ok" : "REJECT");
    }
#endif
    if (!monster.HasSupportedLayout()) {
        return;
    }
    const auto level = monster.Level();
    if (level < 0 || level > 255) {
        return;
    }
    const auto rates = roomRules.ForLevel(static_cast<std::uint8_t>(level));
#ifndef MONSTER_TUNING_NO_CRT
    if (spawnCalls <= 12) {
        DiagLog("SPAWN #%lu: level=%d hp=%g def=%g atk=%g move=%g",
                spawnCalls, level, rates.hp, rates.defense, rates.attack, rates.moveSpeed);
    }
#endif
    using v208::MonsterScalar;
    for (unsigned block = 0; block < v208::MonsterView::AttributeBlockCount; ++block) {
        ScaleMonsterHealthField(monster.MaximumHealth(block), rates.hp);
        ScaleMonsterScalar(monster.Scalar(block, MonsterScalar::MoveSpeed), rates.moveSpeed);
        ScaleMonsterScalar(monster.Scalar(block, MonsterScalar::AttackSpeed), rates.attackSpeed);
        ScaleMonsterScalar(monster.Scalar(block, MonsterScalar::PhysicalAttack), rates.attack);
        ScaleMonsterScalar(monster.Scalar(block, MonsterScalar::PhysicalDefense), rates.defense);
        ScaleMonsterScalar(monster.Scalar(block, MonsterScalar::MagicalAttack), rates.attack);
        ScaleMonsterScalar(monster.Scalar(block, MonsterScalar::MagicalDefense), rates.defense);
    }
    ScaleMonsterHealthField(monster.CurrentHealth(), rates.hp);
    if (rates.aggression != 1.0) {
        auto& warlike = monster.Aggression();
        const auto scaled = warlike * rates.aggression;
        warlike = scaled >= 100.0 ? 100 : scaled <= 0.0 ? 0 : static_cast<std::int32_t>(TruncateToInt64(scaled));
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void MonsterTuningSpawnV208() {
    __asm {
        pushfd
        pushad
        push esi
        call ApplyMonsterSpawnTuning
        add esp, 4
        popad
        popfd
        mov eax, [esi]
        mov edx, [eax + 434h]
        push 02628ee5h
        ret
    }
}
