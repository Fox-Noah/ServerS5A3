#include "monster_tuning.h"

#include <cassert>
#include <vector>

namespace {

void AppendWord(std::vector<std::uint8_t>& packet, std::uint64_t value, unsigned byteCount) {
    for (unsigned i = 0; i < byteCount; ++i) {
        packet.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
}

void AppendRow(std::vector<std::uint8_t>& packet, std::uint8_t first, std::uint8_t last,
               const std::array<double, 6>& values) {
    packet.push_back(first);
    packet.push_back(last);
    for (double value : values) {
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        AppendWord(packet, bits, 8);
    }
}

}  // namespace

int main() {
    constexpr std::size_t nativeSize = 16;
    std::vector<std::uint8_t> packet(nativeSize, 0);
    AppendRow(packet, 0, 87, {2, 3, 4, 0.5, 1.25, 0});
    AppendRow(packet, 88, 255, {1, 1, 1, 1, 1, 1});
    AppendWord(packet, 100, 4);
    AppendWord(packet, 0x3154414d, 4);

    monster_tuning::RoomRules rules;
    assert(rules.ReadStartMap(packet.data(), packet.size()) == nativeSize);
    const auto selected = rules.ForLevel(87);
    assert(selected.hp == 2 && selected.defense == 3 && selected.attack == 4);
    assert(selected.moveSpeed == 0.5 && selected.attackSpeed == 1.25 && selected.aggression == 0);
    assert(rules.ForLevel(88).hp == 1 && rules.ForLevel(255).hp == 1);

    auto malformed = packet;
    malformed[nativeSize + 50] = 87;
    assert(rules.ReadStartMap(malformed.data(), malformed.size()) == malformed.size());
    assert(rules.ForLevel(87).hp == 1);

    malformed = packet;
    malformed[malformed.size() - 8] = 101;
    assert(rules.ReadStartMap(malformed.data(), malformed.size()) == malformed.size());
    assert(rules.ForLevel(87).hp == 1);

    malformed = packet;
    std::memset(malformed.data() + nativeSize + 2, 0xff, 8);
    assert(rules.ReadStartMap(malformed.data(), malformed.size()) == malformed.size());
    assert(rules.ForLevel(87).hp == 1);

    assert(rules.ReadStartMap(packet.data(), packet.size()) == nativeSize);
    assert(rules.ReadStartMap(packet.data(), nativeSize) == nativeSize);
    assert(rules.ForLevel(0).hp == 1);

    assert(rules.ReadStartMap(packet.data(), packet.size()) == nativeSize);
    rules.Reset();
    assert(rules.ForLevel(87).hp == 1);

    auto remaining = static_cast<std::int32_t>(packet.size());
    monster_tuning::PrepareStartMap(rules, packet.data(), remaining);
    assert(remaining == nativeSize && rules.ForLevel(87).hp == 2);
    remaining = -1;
    monster_tuning::PrepareStartMap(rules, packet.data(), remaining);
    assert(remaining == -1 && rules.ForLevel(87).hp == 1);
}
