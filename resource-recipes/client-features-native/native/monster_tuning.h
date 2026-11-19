#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace monster_tuning {

struct Multipliers {
    double hp = 1;
    double defense = 1;
    double attack = 1;
    double moveSpeed = 1;
    double attackSpeed = 1;
    double aggression = 1;
};

class RoomRules {
public:
    // Returns the native START_MAP body length. The call replaces the rules
    // for the room, including when the incoming packet has no extension.
    std::size_t ReadStartMap(const std::uint8_t* body, std::size_t length) noexcept {
        active_ = false;
        constexpr std::size_t kFooter = 8;
        constexpr std::size_t kRow = 50;
        constexpr std::size_t kMinNativeBody = 16;
        if (!body || length < kMinNativeBody + kFooter) {
            return length;
        }
        const auto* footer = body + length - kFooter;
        if (std::memcmp(footer + 4, "MAT1", 4) != 0) {
            return length;
        }
        const auto rowBytes = ReadUnsigned<std::uint32_t>(footer);
        if (rowBytes == 0 || rowBytes % kRow != 0 ||
            rowBytes > kRow * rules_.size() ||
            rowBytes > length - kMinNativeBody - kFooter) {
            return length;
        }

        const auto* row = footer - rowBytes;
        unsigned expectedLevel = 0;
        for (; row != footer; row += kRow) {
            const unsigned first = row[0];
            const unsigned last = row[1];
            if (first != expectedLevel || first > last) {
                return length;
            }
            std::array<double, 6> rates;
            for (std::size_t index = 0; index < rates.size(); ++index) {
                const auto bits = ReadUnsigned<std::uint64_t>(row + 2 + index * 8);
                constexpr std::uint64_t exponent = 0x7ff0000000000000;
                constexpr std::uint64_t sign = 0x8000000000000000;
                const auto magnitude = bits & ~sign;
                if ((bits & exponent) == exponent ||
                    ((bits & sign) != 0 && magnitude != 0) ||
                    (index == 0 && magnitude == 0)) {
                    return length;
                }
                std::memcpy(&rates[index], &bits, sizeof(double));
            }
            const Multipliers value{rates[0], rates[1], rates[2], rates[3], rates[4], rates[5]};
            for (auto level = first; level <= last; ++level) {
                rules_[level] = value;
            }
            expectedLevel = last + 1;
        }
        if (expectedLevel != rules_.size()) {
            return length;
        }
        active_ = true;
        return length - kFooter - rowBytes;
    }

    Multipliers ForLevel(std::uint8_t level) const noexcept {
        return active_ ? rules_[level] : Multipliers{};
    }

    void Reset() noexcept {
        active_ = false;
    }

private:
    template <typename UInt>
    static UInt ReadUnsigned(const std::uint8_t* bytes) noexcept {
        UInt result = 0;
        for (std::size_t i = 0; i < sizeof(UInt); ++i) {
            result |= static_cast<UInt>(bytes[i]) << (i * 8);
        }
        return result;
    }

    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    std::array<Multipliers, 256> rules_{};
    bool active_ = false;
};

// The original receiver retains its current body cursor. Only the length it
// can read changes when a complete extension follows the native payload.
inline void PrepareStartMap(RoomRules& rules, const std::uint8_t* cursor,
                            std::int32_t& remaining) noexcept {
    if (!cursor || remaining < 0) {
        rules.Reset();
        return;
    }
    remaining = static_cast<std::int32_t>(
        rules.ReadStartMap(cursor, static_cast<std::size_t>(remaining)));
}

}  // namespace monster_tuning
