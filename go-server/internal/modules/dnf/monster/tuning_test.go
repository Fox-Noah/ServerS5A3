package monster

import (
	"encoding/json"
	"math"
	"testing"
)

func TestTuningJSONRequiresExplicitMultipliers(t *testing.T) {
	valid := `{"hp":2,"defense":1,"attack":1,"moveSpeed":1,"attackSpeed":1,"aggression":0}`
	var got Multipliers
	if err := json.Unmarshal([]byte(valid), &got); err != nil || got.Aggression != 0 || got.HP != 2 {
		t.Fatalf("valid explicit zero did not round-trip: %+v, %v", got, err)
	}
	for _, invalid := range []string{
		`{"hp":2}`,
		`{"hp":2,"defense":1,"attack":1,"moveSpeed":1,"attackSpeed":1,"aggression":null}`,
		`{"hp":2,"defense":1,"attack":1,"moveSpeed":1,"attackSpeed":1,"aggression":0,"extra":1}`,
	} {
		if err := json.Unmarshal([]byte(invalid), &got); err == nil {
			t.Fatalf("accepted incomplete or unknown values: %s", invalid)
		}
	}
}

func TestTuningRuleScope(t *testing.T) {
	global := Multipliers{HP: 2, Defense: 1, Attack: 1, MoveSpeed: 1, AttackSpeed: 1, Aggression: 1}
	stage := Multipliers{HP: 3, Defense: 2, Attack: 4, MoveSpeed: 0.5, AttackSpeed: 1.25, Aggression: 0}
	singleMap := Multipliers{HP: 1, Defense: 1, Attack: 1, MoveSpeed: 1, AttackSpeed: 1, Aggression: 1}
	rules := []TuningRule{
		{Scope: "global", Multipliers: global},
		{Scope: "level", MinLevel: 200, MaxLevel: 255, Multipliers: stage},
		{Scope: "map", MapID: 42, Multipliers: singleMap},
	}
	snapshot, err := CompileTuningRules(rules)
	if err != nil {
		t.Fatal(err)
	}
	rules[1].Multipliers.HP = 9
	for _, tc := range []struct {
		mapID uint32
		level uint8
		want  Multipliers
	}{
		{41, 199, global},
		{41, 200, stage},
		{41, 255, stage},
		{42, 200, singleMap},
	} {
		if got := snapshot.Match(tc.mapID, tc.level); got != tc.want {
			t.Errorf("Match(%d, %d) = %+v, want %+v", tc.mapID, tc.level, got, tc.want)
		}
	}
	empty, err := CompileTuningRules(nil)
	if err != nil || empty.Match(41, 200) != singleMap {
		t.Fatalf("empty rules must preserve base stats: %v", err)
	}
}

func TestTuningRulesRejectInvalidOrAmbiguousInputs(t *testing.T) {
	valid := Multipliers{HP: 1, Defense: 1, Attack: 1, MoveSpeed: 1, AttackSpeed: 1, Aggression: 1}
	global := TuningRule{Scope: "global", Multipliers: valid}
	singleMap := TuningRule{Scope: "map", MapID: 42, Multipliers: valid}
	stage := TuningRule{Scope: "level", MinLevel: 1, MaxLevel: 20, Multipliers: valid}
	for name, rules := range map[string][]TuningRule{
		"duplicate global": {global, global},
		"duplicate map":    {singleMap, singleMap},
		"overlap":          {stage, {Scope: "level", MinLevel: 20, MaxLevel: 30, Multipliers: valid}},
		"empty range":      {{Scope: "level", MinLevel: 10, MaxLevel: 9, Multipliers: valid}},
		"missing map":      {{Scope: "map", Multipliers: valid}},
		"wrong scope":      {{Scope: "global", MapID: 42, Multipliers: valid}},
		"zero HP":          {{Scope: "global"}},
		"nonfinite speed":  {{Scope: "global", Multipliers: Multipliers{HP: 1, MoveSpeed: math.Inf(1)}}},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := CompileTuningRules(rules); err == nil {
				t.Error("accepted invalid rules")
			}
		})
	}
}
