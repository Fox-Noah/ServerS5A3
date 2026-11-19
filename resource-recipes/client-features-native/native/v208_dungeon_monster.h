#pragma once

#include <cstdint>

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error The V208 native interface requires MSVC x86.
#endif

namespace v208 {

// Pinned image ABI. The packager verifies the original executable digest
// before these addresses can be used. Call on the native game thread.
struct PacketReader {
    const std::uint8_t* cursor;
    std::int32_t& remaining;
};

inline PacketReader ActivePacketReader() noexcept {
    return {*reinterpret_cast<const std::uint8_t* const*>(0x057f9ad0),
            *reinterpret_cast<std::int32_t*>(0x057f9ad4)};
}

using ScalarReader = void (__thiscall*)(const void*, const void*, std::int32_t*);
using ScalarWriter = void (__thiscall*)(void*, const std::int32_t*, void*);
using HealthReader = void (__thiscall*)(const void*, const void*, std::int64_t*);
using HealthWriter = void (__thiscall*)(void*, const std::int64_t*, void*);

inline unsigned ScalarCodec(const void* field) noexcept {
    const auto alignment = reinterpret_cast<std::uintptr_t>(field) & 15;
    return alignment % 4 == 0 ? alignment / 4 : 4;
}

inline std::int32_t ReadScalar(const std::uint8_t* field) noexcept {
    constexpr std::uintptr_t readers[] = {
        0x037df550, 0x037df5c0, 0x037df630, 0x037df6a0, 0x037df7f0,
    };
    std::int32_t value = 0;
    reinterpret_cast<ScalarReader>(readers[ScalarCodec(field)])(field, field + 4, &value);
    return value;
}

inline void WriteScalar(std::uint8_t* field, std::int32_t value) noexcept {
    constexpr std::uintptr_t writers[] = {
        0x037df130, 0x037df1b0, 0x037df230, 0x037df2b0, 0x037df010,
    };
    reinterpret_cast<ScalarWriter>(writers[ScalarCodec(field)])(field, &value, field + 4);
}

inline std::int64_t ReadHealth(const std::uint8_t* field) noexcept {
    std::int64_t value = 0;
    reinterpret_cast<HealthReader>(0x037df870)(field, field + 8, &value);
    return value;
}

inline void WriteHealth(std::uint8_t* field, std::int64_t value) noexcept {
    reinterpret_cast<HealthWriter>(0x037df090)(field, &value, field + 8);
}

enum class MonsterScalar : unsigned {
    MoveSpeed = 0x2a0,
    AttackSpeed = 0x2b0,
    PhysicalAttack = 0x338,
    PhysicalDefense = 0x348,
    MagicalAttack = 0x358,
    MagicalDefense = 0x368,
};

// Borrows a live actor. Check HasSupportedLayout before accessing attributes.
// The view is valid only while the native actor remains alive.
class MonsterView {
public:
    explicit MonsterView(void* actor) noexcept : object_(static_cast<std::uint8_t*>(actor)) {}

    bool HasSupportedLayout() const noexcept {
        if (!object_) return false;
        const auto* table = *reinterpret_cast<const std::uintptr_t* const*>(object_);
        return table[0x50 / 4] == 0x02677900 && table[0x670 / 4] == 0x026d13c0 &&
            table[0x694 / 4] == 0x026d13f0 &&
            *reinterpret_cast<const std::uint32_t*>(object_ + 0xa8) == 0x211;
    }

    static constexpr unsigned AttributeBlockCount = 4;
    std::int32_t Level() const noexcept { return ReadScalar(Block(0)); }
    // Block indices must be below AttributeBlockCount.
    std::uint8_t* Scalar(unsigned block, MonsterScalar stat) const noexcept {
        return Block(block) + static_cast<unsigned>(stat);
    }
    std::uint8_t* MaximumHealth(unsigned block) const noexcept { return Block(block) + 8; }
    std::uint8_t* CurrentHealth() const noexcept { return object_ + 0x2dd0; }
    std::int32_t& Aggression() const noexcept {
        return *reinterpret_cast<std::int32_t*>(object_ + 0x41d8);
    }

private:
    std::uint8_t* Block(unsigned index) const noexcept { return object_ + 0x10d0 + index * 0x740; }
    std::uint8_t* object_;
};

}  // namespace v208
