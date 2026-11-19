package dnfbridge

import (
	"math"

	"longheng.io/server/internal/modules/dnf/monster"
)

const monsterTuningTrailerMagic = 0x3154414d // MAT1

// The paired receiver reads the footer before invoking the native START_MAP
// parser. Each row contains an inclusive level interval and six binary64
// rates. A room-scoped interval table also covers monsters spawned later.
func appendMonsterTuningTrailer(body []byte, rules *monster.TuningRules, mapID uint32) []byte {
	if rules == nil {
		return body
	}
	var rows packetWriter
	start := 0
	current := rules.Match(mapID, 0)
	for level := 1; level <= 256; level++ {
		var next monster.Multipliers
		if level < 256 {
			next = rules.Match(mapID, uint8(level))
			if next == current {
				continue
			}
		}
		rows.writeByte(byte(start))
		rows.writeByte(byte(level - 1))
		for _, rate := range [...]float64{
			current.HP, current.Defense, current.Attack,
			current.MoveSpeed, current.AttackSpeed, current.Aggression,
		} {
			rows.writeUint64(math.Float64bits(rate))
		}
		start = level
		current = next
	}
	var footer packetWriter
	footer.writeUint32(uint32(len(rows.bytes())))
	footer.writeUint32(monsterTuningTrailerMagic)
	body = append(body, rows.bytes()...)
	return append(body, footer.bytes()...)
}
