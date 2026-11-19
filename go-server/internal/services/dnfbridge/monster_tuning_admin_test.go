package dnfbridge

import (
	"bytes"
	"context"
	"testing"

	"longheng.io/server/internal/modules/dnf/adminapi"
	"longheng.io/server/internal/modules/dnf/monster"
)

func TestMonsterTuningMySQLRevision(t *testing.T) {
	f := newReplacementMySQLFixture(t)
	ctx := context.Background()
	if err := f.api.ensureAccountSecuritySchema(ctx); err != nil {
		t.Fatal(err)
	}
	cfg, err := f.api.readMonsterTuning(ctx)
	if err != nil || cfg.Revision != 0 || cfg.Rules == nil || len(cfg.Rules) != 0 {
		t.Fatalf("initial configuration = %+v, error = %v", cfg, err)
	}
	firstRun, err := f.service.loadDungeonMonsterTuning(ctx, nil)
	if err != nil || firstRun != nil {
		t.Fatalf("empty rules must leave first run unchanged: %v", err)
	}
	cfg.Rules = append(cfg.Rules, monster.TuningRule{
		Scope: "map",
		MapID: 24,
		Multipliers: monster.Multipliers{
			HP: 2, Attack: 1.25, Defense: 0.5, MoveSpeed: 0.75, AttackSpeed: 1.5, Aggression: 0,
		},
	})
	save := adminapi.Request{Action: "monster_tuning_save", MonsterTuning: cfg}
	out := adminapi.Response{OK: true}
	f.api.handleMonsterTuningAdmin(ctx, save, &out)
	if !out.OK || out.MonsterTuning == nil || out.MonsterTuning.Revision != 1 {
		t.Fatalf("save failed: %+v", out)
	}
	out = adminapi.Response{OK: true}
	f.api.handleMonsterTuningAdmin(ctx, save, &out)
	if out.OK || out.ErrorCode != "monster_tuning_conflict" {
		t.Fatalf("stale update accepted: %+v", out)
	}
	if err := f.api.ensureAccountSecuritySchema(ctx); err != nil {
		t.Fatal(err)
	}
	got, err := f.api.readMonsterTuning(ctx)
	if err != nil || got.Revision != 1 || len(got.Rules) != 1 || got.Rules[0] != cfg.Rules[0] {
		t.Fatalf("persisted configuration = %+v, error = %v", got, err)
	}
	partyMember, err := f.service.loadDungeonMonsterTuning(ctx, &runtimePartyDungeonEntryPlan{monsterTuning: firstRun})
	if err != nil || partyMember != firstRun {
		t.Fatalf("party member picked up rules saved during the existing run: %v", err)
	}
	newRun, err := f.service.loadDungeonMonsterTuning(ctx, nil)
	if err != nil || newRun == nil || newRun.Match(24, 1) != cfg.Rules[0].Multipliers {
		t.Fatalf("new run did not load the latest rules: %v", err)
	}
	packet := currentDungeonStartMap{MapID: 24, PartyMemberIndex: 0xff, monsterTuning: firstRun}
	originalBody, err := packet.Build()
	if err != nil {
		t.Fatal(err)
	}
	packet.monsterTuning = newRun
	newBody, err := packet.Build()
	if err != nil || !bytes.HasPrefix(newBody, originalBody) || !bytes.HasSuffix(newBody, []byte("MAT1")) {
		t.Fatalf("new run did not produce extended START_MAP: %x, %v", newBody, err)
	}
	out = adminapi.Response{OK: true}
	f.api.handleMonsterTuningAdmin(ctx, adminapi.Request{
		Action: "monster_tuning_save",
		MonsterTuning: &monster.TuningConfiguration{
			Revision: got.Revision,
			Rules:    []monster.TuningRule{},
		},
	}, &out)
	if !out.OK || out.MonsterTuning.Revision != 2 {
		t.Fatalf("clear failed: %+v", out)
	}
	afterClear, err := f.service.loadDungeonMonsterTuning(ctx, nil)
	if err != nil || afterClear != nil {
		t.Fatalf("new run inherited cleared rules: %v", err)
	}
	partyMember, err = f.service.loadDungeonMonsterTuning(ctx, &runtimePartyDungeonEntryPlan{monsterTuning: newRun})
	if err != nil || partyMember != newRun {
		t.Fatalf("party member lost the running instance's snapshot: %v", err)
	}
	packet.monsterTuning = afterClear
	clearedBody, err := packet.Build()
	if err != nil || !bytes.Equal(clearedBody, originalBody) {
		t.Fatalf("clear must restore the native packet: %x, %v", clearedBody, err)
	}
}
