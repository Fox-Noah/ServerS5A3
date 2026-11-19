package monster

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
)

// Multipliers contains independent factors relative to the original monster.
type Multipliers struct {
	HP          float64 `json:"hp"`
	Defense     float64 `json:"defense"`
	Attack      float64 `json:"attack"`
	MoveSpeed   float64 `json:"moveSpeed"`
	AttackSpeed float64 `json:"attackSpeed"`
	Aggression  float64 `json:"aggression"`
}

func (m *Multipliers) UnmarshalJSON(data []byte) error {
	var fields struct {
		HP          *float64 `json:"hp"`
		Defense     *float64 `json:"defense"`
		Attack      *float64 `json:"attack"`
		MoveSpeed   *float64 `json:"moveSpeed"`
		AttackSpeed *float64 `json:"attackSpeed"`
		Aggression  *float64 `json:"aggression"`
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&fields); err != nil {
		return err
	}
	if fields.HP == nil || fields.Defense == nil || fields.Attack == nil ||
		fields.MoveSpeed == nil || fields.AttackSpeed == nil || fields.Aggression == nil {
		return fmt.Errorf("all six monster multipliers are required")
	}
	*m = Multipliers{
		HP: *fields.HP, Defense: *fields.Defense, Attack: *fields.Attack,
		MoveSpeed: *fields.MoveSpeed, AttackSpeed: *fields.AttackSpeed,
		Aggression: *fields.Aggression,
	}
	return nil
}

func (m Multipliers) validate() error {
	for _, field := range []struct {
		name string
		rate float64
	}{
		{"hp", m.HP},
		{"defense", m.Defense},
		{"attack", m.Attack},
		{"moveSpeed", m.MoveSpeed},
		{"attackSpeed", m.AttackSpeed},
		{"aggression", m.Aggression},
	} {
		if math.IsNaN(field.rate) || math.IsInf(field.rate, 0) || field.rate < 0 ||
			(field.name == "hp" && field.rate == 0) {
			return fmt.Errorf("%s multiplier is invalid", field.name)
		}
	}
	return nil
}

type TuningRule struct {
	Scope       string      `json:"scope"`
	MapID       uint32      `json:"mapId,omitempty"`
	MinLevel    uint8       `json:"minLevel,omitempty"`
	MaxLevel    uint8       `json:"maxLevel,omitempty"`
	Multipliers Multipliers `json:"multipliers"`
}

type TuningConfiguration struct {
	Revision uint64       `json:"revision"`
	Rules    []TuningRule `json:"rules"`
}

// TuningRules is immutable after compilation. A dungeon run can retain a
// snapshot even when administrators publish a replacement.
type TuningRules struct {
	global Multipliers
	maps   map[uint32]Multipliers
	levels [256]*Multipliers
}

func CompileTuningRules(rules []TuningRule) (*TuningRules, error) {
	out := &TuningRules{
		global: Multipliers{HP: 1, Defense: 1, Attack: 1, MoveSpeed: 1, AttackSpeed: 1, Aggression: 1},
		maps:   make(map[uint32]Multipliers),
	}
	hasGlobal := false
	for index, rule := range rules {
		if err := out.add(rule, &hasGlobal); err != nil {
			return nil, fmt.Errorf("monster tuning rule %d: %w", index+1, err)
		}
	}
	return out, nil
}

func (r *TuningRules) add(rule TuningRule, hasGlobal *bool) error {
	if err := rule.Multipliers.validate(); err != nil {
		return err
	}
	switch rule.Scope {
	case "global":
		if *hasGlobal || rule.MapID != 0 || rule.MinLevel != 0 || rule.MaxLevel != 0 {
			return fmt.Errorf("invalid or duplicate global rule")
		}
		r.global = rule.Multipliers
		*hasGlobal = true
	case "map":
		if rule.MapID == 0 || rule.MinLevel != 0 || rule.MaxLevel != 0 {
			return fmt.Errorf("invalid map selector")
		}
		if _, exists := r.maps[rule.MapID]; exists {
			return fmt.Errorf("duplicate map ID %d", rule.MapID)
		}
		r.maps[rule.MapID] = rule.Multipliers
	case "level":
		if rule.MapID != 0 || rule.MinLevel == 0 || rule.MinLevel > rule.MaxLevel {
			return fmt.Errorf("invalid level range")
		}
		for level := int(rule.MinLevel); level <= int(rule.MaxLevel); level++ {
			if r.levels[level] != nil {
				return fmt.Errorf("overlapping level range at level %d", level)
			}
			r.levels[level] = &rule.Multipliers
		}
	default:
		return fmt.Errorf("unknown scope %q", rule.Scope)
	}
	return nil
}

// Match applies the most specific rule. Map overrides level, which overrides
// global. The level refers to the effective level in the spawn record.
func (r *TuningRules) Match(mapID uint32, level uint8) Multipliers {
	if m, exists := r.maps[mapID]; exists {
		return m
	}
	if m := r.levels[level]; m != nil {
		return *m
	}
	return r.global
}
