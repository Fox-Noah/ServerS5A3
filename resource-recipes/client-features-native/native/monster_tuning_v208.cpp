// This adapter is for the pinned v208 PE32 image only. A candidate packager
// must verify the complete original image digest and opcode registration.
#include "monster_tuning.h"

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error The v208 START_MAP adapter requires MSVC x86.
#endif

// The additive payload uses no CRT. MSVC emits this marker for binary64 data.
#ifdef MONSTER_TUNING_NO_CRT
extern "C" { int _fltused = 0; }
#endif

namespace {

constexpr std::uintptr_t kPacketCursor = 0x057f9ad0;
constexpr std::uintptr_t kPacketRemaining = 0x057f9ad4;
monster_tuning::RoomRules roomRules;

using ReadHealth = void (__thiscall*)(const void*, const void*, std::int64_t*);
using WriteHealth = void (__thiscall*)(void*, const std::int64_t*, void*);

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

using ScalarReader = void (__thiscall*)(const void*, const void*, std::int32_t*);
using ScalarWriter = void (__thiscall*)(void*, const std::int32_t*, void*);

unsigned ScalarCodec(void* field) noexcept {
    const auto alignment = reinterpret_cast<std::uintptr_t>(field) & 15;
    return alignment % 4 == 0 ? alignment / 4 : 4;
}

std::int32_t ReadMonsterScalar(void* field) noexcept {
    constexpr std::uintptr_t readers[] = {
        0x037df550, 0x037df5c0, 0x037df630, 0x037df6a0, 0x037df7f0,
    };
    std::int32_t value = 0;
    reinterpret_cast<ScalarReader>(readers[ScalarCodec(field)])(
        field, static_cast<std::uint8_t*>(field) + 4, &value);
    return value;
}

void ScaleMonsterScalar(void* field, double rate) noexcept {
    if (rate == 1.0) {
        return;
    }
    const auto value = ReadMonsterScalar(field);
    const auto scaled = value * rate;
    constexpr auto maxValue = (std::numeric_limits<std::int32_t>::max)();
    constexpr auto minValue = (std::numeric_limits<std::int32_t>::min)();
    const auto result = scaled >= maxValue ? maxValue :
        scaled <= minValue ? minValue : static_cast<std::int32_t>(TruncateToInt64(scaled));
    if (result == value) {
        return;
    }
    constexpr std::uintptr_t writers[] = {
        0x037df130, 0x037df1b0, 0x037df230, 0x037df2b0, 0x037df010,
    };
    reinterpret_cast<ScalarWriter>(writers[ScalarCodec(field)])(
        field, &result, static_cast<std::uint8_t*>(field) + 4);
}
}  // namespace

extern "C" __declspec(noinline) void __cdecl PrepareMonsterTuningStartMap() noexcept {
    const auto cursor = *reinterpret_cast<const std::uint8_t* const*>(kPacketCursor);
    auto& remaining = *reinterpret_cast<std::int32_t*>(kPacketRemaining);
    monster_tuning::PrepareStartMap(roomRules, cursor, remaining);
}

extern "C" __declspec(dllexport) void __cdecl
CopyMonsterTuningRatesV208(std::uint8_t level, monster_tuning::Multipliers* rates) noexcept {
    *rates = roomRules.ForLevel(level);
}

static void ScaleMonsterHealthField(void* field, double multiplier) noexcept {
    if (multiplier == 1.0) {
        return;
    }
    auto* encoded = static_cast<std::uint8_t*>(field) + 8;
    const auto readHealth = reinterpret_cast<ReadHealth>(0x037df870);
    const auto writeHealth = reinterpret_cast<WriteHealth>(0x037df090);
    std::int64_t health = 0;
    readHealth(field, encoded, &health);
    if (health <= 0) {
        return;
    }
    const auto scaled = ScalePositiveHealth(health, multiplier);
    if (scaled != health) {
        writeHealth(field, &scaled, encoded);
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void MonsterTuningStartMapV208() {
    __asm {
        pushfd
        pushad
        call PrepareMonsterTuningStartMap
        popad
        popfd
        push 01ff8d80h
        ret
    }
}

extern "C" __declspec(noinline) void __cdecl ApplyMonsterSpawnTuning(void* actor) noexcept {
    auto* object = static_cast<std::uint8_t*>(actor);
    const auto* table = *reinterpret_cast<const std::uintptr_t* const*>(object);
    // These three native accessors establish the kind, stat layout and HP layout.
    if (table[0x50 / 4] != 0x02677900 || table[0x670 / 4] != 0x026d13c0 ||
        table[0x694 / 4] != 0x026d13f0 ||
        *reinterpret_cast<const std::uint32_t*>(object + 0xa8) != 0x211) {
        return;
    }
    const auto level = ReadMonsterScalar(object + 0x10d0);
    if (level < 0 || level > 255) {
        return;
    }
    const auto rates = roomRules.ForLevel(static_cast<std::uint8_t>(level));
    for (unsigned block = 0; block < 4; ++block) {
        auto* attributes = object + 0x10d0 + block * 0x740;
        ScaleMonsterHealthField(attributes + 8, rates.hp);
        ScaleMonsterScalar(attributes + 0x2a0, rates.moveSpeed);
        ScaleMonsterScalar(attributes + 0x2b0, rates.attackSpeed);
        // Native attack and defense calculations read the equipment stat bases
        // for physical and magical damage independently.
        ScaleMonsterScalar(attributes + 0x338, rates.attack);
        ScaleMonsterScalar(attributes + 0x348, rates.defense);
        ScaleMonsterScalar(attributes + 0x358, rates.attack);
        ScaleMonsterScalar(attributes + 0x368, rates.defense);
    }
    ScaleMonsterHealthField(object + 0x2dd0, rates.hp);
    if (rates.aggression != 1.0) {
        auto& warlike = *reinterpret_cast<std::int32_t*>(object + 0x41d8);
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
