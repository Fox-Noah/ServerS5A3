package dnfbridge

import (
	"context"
	"encoding/json"
	"os"

	"longheng.io/server/internal/modules/dnf/adminapi"
	"longheng.io/server/internal/modules/dnf/monster"
)

func (s *Service) monsterTuningEnabled() bool {
	return s.options.currentClientVersion == "official-24.9" &&
		os.Getenv("DNFBRIDGE_MONSTER_TUNING") == "paired-v1"
}

func (a *accountAPIServer) readMonsterTuning(ctx context.Context) (*monster.TuningConfiguration, error) {
	var cfg monster.TuningConfiguration
	var raw []byte
	err := a.db.QueryRowContext(ctx, "SELECT revision,rules_json FROM "+a.table+"_monster_tuning WHERE id=1").Scan(&cfg.Revision, &raw)
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(raw, &cfg.Rules); err != nil {
		return nil, err
	}
	return &cfg, nil
}

// The party entry plan carries the leader's snapshot, including the case
// where there are no matching rules. Ordinary room transitions keep it.
func (s *Service) loadDungeonMonsterTuning(ctx context.Context, partyPlan *runtimePartyDungeonEntryPlan) (*monster.TuningRules, error) {
	if partyPlan != nil {
		return partyPlan.monsterTuning, nil
	}
	s.mu.Lock()
	api := s.accountAPI
	s.mu.Unlock()
	if api == nil {
		return nil, nil
	}
	cfg, err := api.readMonsterTuning(ctx)
	if err != nil {
		return nil, err
	}
	if len(cfg.Rules) == 0 {
		return nil, nil
	}
	return monster.CompileTuningRules(cfg.Rules)
}

func (a *accountAPIServer) handleMonsterTuningAdmin(ctx context.Context, req adminapi.Request, out *adminapi.Response) {
	if req.Action == "monster_tuning" {
		cfg, err := a.readMonsterTuning(ctx)
		if err == nil {
			out.MonsterTuning = cfg
			return
		}
		out.OK, out.ErrorCode, out.Error = false, "monster_tuning_read_failed", "读取怪物 AI 规则失败"
		return
	}
	if req.MonsterTuning == nil || req.MonsterTuning.Rules == nil {
		out.OK, out.ErrorCode, out.Error = false, "invalid_monster_tuning", "缺少规则列表"
		return
	}
	cfg := *req.MonsterTuning
	if _, err := monster.CompileTuningRules(cfg.Rules); err != nil {
		out.OK, out.ErrorCode, out.Error = false, "invalid_monster_tuning", err.Error()
		return
	}
	raw, err := json.Marshal(cfg.Rules)
	if err == nil {
		result, writeErr := a.db.ExecContext(ctx, "UPDATE "+a.table+"_monster_tuning SET rules_json=?,revision=revision+1 WHERE id=1 AND revision=?", raw, cfg.Revision)
		err = writeErr
		if err == nil {
			var changed int64
			changed, err = result.RowsAffected()
			if err == nil {
				if changed == 0 {
					out.OK, out.ErrorCode, out.Error = false, "monster_tuning_conflict", "规则已变化，请刷新后保存"
					return
				}
				cfg.Revision++
				out.MonsterTuning = &cfg
				return
			}
		}
	}
	out.OK, out.ErrorCode, out.Error = false, "monster_tuning_write_failed", "保存怪物 AI 规则失败"
}
