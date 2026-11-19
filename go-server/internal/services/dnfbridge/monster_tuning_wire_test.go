package dnfbridge

import (
	"bytes"
	"encoding/binary"
	"math"
	"testing"

	"longheng.io/server/internal/modules/dnf/monster"
)

func TestMonsterTuningStartMapTrailer(t *testing.T) {
	global := monster.Multipliers{HP: 2, Defense: 3, Attack: 4, MoveSpeed: 0.5, AttackSpeed: 1.25, Aggression: 0}
	stage := monster.Multipliers{HP: 1, Defense: 1, Attack: 1, MoveSpeed: 1, AttackSpeed: 1, Aggression: 1}
	rules, err := monster.CompileTuningRules([]monster.TuningRule{
		{Scope: "global", Multipliers: global},
		{Scope: "level", MinLevel: 88, MaxLevel: 255, Multipliers: stage},
		{Scope: "map", MapID: 24, Multipliers: stage},
	})
	if err != nil {
		t.Fatal(err)
	}
	packet := currentDungeonStartMap{MapID: 23, PartyMemberIndex: 0xff}
	base, err := packet.Build()
	if err != nil {
		t.Fatal(err)
	}
	packet.monsterTuning = rules
	extended, err := packet.Build()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(extended, base) {
		t.Fatal("legacy payload changed")
	}
	trailer := extended[len(base):]
	if len(trailer) != 108 ||
		binary.LittleEndian.Uint32(trailer[100:]) != 100 ||
		string(trailer[104:]) != "MAT1" {
		t.Fatalf("invalid trailer: %x", trailer)
	}
	checkRow := func(row []byte, min, max byte, want monster.Multipliers) {
		t.Helper()
		if row[0] != min || row[1] != max {
			t.Fatalf("row interval = %d..%d, want %d..%d", row[0], row[1], min, max)
		}
		for index, rate := range []float64{want.HP, want.Defense, want.Attack, want.MoveSpeed, want.AttackSpeed, want.Aggression} {
			got := math.Float64frombits(binary.LittleEndian.Uint64(row[2+8*index:]))
			if got != rate {
				t.Fatalf("rate %d = %v, want %v", index, got, rate)
			}
		}
	}
	checkRow(trailer[:50], 0, 87, global)
	checkRow(trailer[50:100], 88, 255, stage)
	cached := appendMonsterTuningTrailer(make([]byte, 16), rules, 24)
	if len(cached) != 74 {
		t.Fatalf("cached packet length = %d", len(cached))
	}
	checkRow(cached[16:66], 0, 255, stage)
}
