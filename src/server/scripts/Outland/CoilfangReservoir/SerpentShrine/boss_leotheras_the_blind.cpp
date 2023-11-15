/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CreatureGroups.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "serpent_shrine.h"
#include "TaskScheduler.h"

enum Talk
{
    SAY_AGGRO                           = 0,
    SAY_SWITCH_TO_DEMON                 = 1,
    SAY_INNER_DEMONS                    = 2,
    SAY_DEMON_SLAY                      = 3,
    SAY_NIGHTELF_SLAY                   = 4,
    SAY_FINAL_FORM                      = 5,
    SAY_DEATH                           = 6
};

enum Spells
{
    SPELL_DUAL_WIELD                    = 42459,
    SPELL_BANISH                        = 37546,
    SPELL_TAUNT                         = 37548,
    SPELL_BERSERK                       = 26662,
    SPELL_WHIRLWIND                     = 37640,
    SPELL_SUMMON_SHADOW_OF_LEOTHERAS    = 37781,

    // Demon Form
    SPELL_CHAOS_BLAST                   = 37674,
    SPELL_CHAOS_BLAST_TRIGGER           = 37675,
    SPELL_INSIDIOUS_WHISPER             = 37676,
    SPELL_METAMORPHOSIS                 = 37673,
    SPELL_SUMMON_INNER_DEMON            = 37735,
    SPELL_CONSUMING_MADNESS             = 37749,

    // Other
    SPELL_CLEAR_CONSUMING_MADNESS       = 37750,
    SPELL_SHADOW_BOLT                   = 39309
};

enum Misc
{
    NPC_SHADOW_OF_LEOTHERAS             = 21875,
    NPC_GREYHEART_SPELLBINDER           = 21806,

    ACTION_CHECK_SPELLBINDERS           = 1
};

enum Groups
{
    GROUP_COMBAT                        = 1,
    GROUP_DEMON                         = 2
};

struct boss_leotheras_the_blind : public BossAI
{
    boss_leotheras_the_blind(Creature* creature) : BossAI(creature, DATA_LEOTHERAS_THE_BLIND) { }

    void Reset() override
    {
        BossAI::Reset();
        DoCastSelf(SPELL_CLEAR_CONSUMING_MADNESS, true);
        DoCastSelf(SPELL_DUAL_WIELD, true);
        me->SetReactState(REACT_PASSIVE);
        _recentlySpoken = false;

        ScheduleHealthCheckEvent(15, [&]{
            me->RemoveAurasDueToSpell(SPELL_WHIRLWIND);

            if (me->GetDisplayId() != me->GetNativeDisplayId())
            {
                //is currently in metamorphosis
                me->LoadEquipment();
                me->RemoveAurasDueToSpell(SPELL_METAMORPHOSIS);
                scheduler.RescheduleGroup(GROUP_COMBAT, 10s);
            }

            me->SetUnitFlag(UNIT_FLAG_NOT_SELECTABLE);

            DoResetThreatList();
            me->ClearTarget();
            me->SendMeleeAttackStop();
            scheduler.CancelGroup(GROUP_DEMON);
            scheduler.DelayAll(10s);

            me->SetReactState(REACT_PASSIVE);
            me->SetStandState(UNIT_STAND_STATE_KNEEL);
            me->GetMotionMaster()->Clear();
            me->StopMoving();
            Talk(SAY_FINAL_FORM);

            scheduler.Schedule(4s, [this](TaskContext)
            {
                DoCastSelf(SPELL_SUMMON_SHADOW_OF_LEOTHERAS);
            }).Schedule(6s, [this](TaskContext)
            {
                me->RemoveUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
                me->SetStandState(UNIT_STAND_STATE_STAND);
                me->SetReactState(REACT_AGGRESSIVE);
                me->ResumeChasingVictim();

                if (me->GetVictim())
                {
                    me->SetTarget(me->GetVictim()->GetGUID());
                    me->SendMeleeAttackStart(me->GetVictim());
                }
            });
        });
    }

    void DoAction(int32 actionId) override
    {
        if (actionId == ACTION_CHECK_SPELLBINDERS)
        {
            if (CreatureGroup* formation = me->GetFormation())
            {
                if (!formation->IsAnyMemberAlive(true))
                {
                    me->RemoveAllAuras();
                    me->LoadEquipment();
                    me->SetReactState(REACT_AGGRESSIVE);
                    me->SetStandState(UNIT_STAND_STATE_STAND);
                    Talk(SAY_AGGRO);

                    scheduler.Schedule(10min, [this](TaskContext)
                    {
                        DoCastSelf(SPELL_BERSERK);
                    });

                    ElfTime();
                }
            }
        }
    }

    void ElfTime()
    {
        DoResetThreatList();
        me->InterruptNonMeleeSpells(false);
        scheduler.Schedule(25050ms, 32550ms, GROUP_COMBAT, [this](TaskContext context)
        {
            DoCastSelf(SPELL_WHIRLWIND);
            context.Repeat(30250ms, 34900ms);
        }).Schedule(60350ms, GROUP_DEMON, [this](TaskContext)
        {
            DoResetThreatList();
            Talk(SAY_SWITCH_TO_DEMON);
            DemonTime();
        });
    }

    void DemonTime()
    {
        DoResetThreatList();
        me->InterruptNonMeleeSpells(false);
        me->LoadEquipment(0, true);
        me->GetMotionMaster()->MoveChase(me->GetVictim(), 25.0f);
        DoCastSelf(SPELL_METAMORPHOSIS, true);

        scheduler.CancelGroup(GROUP_COMBAT);
        scheduler.Schedule(24250ms, GROUP_DEMON, [this](TaskContext)
        {
            Talk(SAY_INNER_DEMONS);
            me->CastCustomSpell(SPELL_INSIDIOUS_WHISPER, SPELLVALUE_MAX_TARGETS, 5, me, false);
        }).Schedule(60s, [this](TaskContext)
        {
            DoResetThreatList();
            me->LoadEquipment();
            me->GetMotionMaster()->MoveChase(me->GetVictim(), 0.0f);
            me->RemoveAurasDueToSpell(SPELL_METAMORPHOSIS);
            ElfTime();
        });
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        scheduler.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        if (me->GetDisplayId() == me->GetNativeDisplayId())
        {
            if (me->GetReactState() != REACT_PASSIVE)
            {
                DoMeleeAttackIfReady();
            }
        }
        else if (me->isAttackReady(BASE_ATTACK))
        {
            if (DoCastVictim(SPELL_CHAOS_BLAST) != SPELL_CAST_OK)
            {
                // Auto-attacks if there are no valid targets to cast his spell on f.e pet taunted.
                DoMeleeAttackIfReady();
            }
            else
            {
                me->setAttackTimer(BASE_ATTACK, 2000);
            }
        }
    }
private:
    bool _recentlySpoken;
};

struct npc_inner_demon : public ScriptedAI
{
    npc_inner_demon(Creature* creature) : ScriptedAI(creature)
    {
        _scheduler.SetValidator([this]
        {
            return !me->HasUnitState(UNIT_STATE_CASTING);
        });

        _instance = creature->GetInstanceScript();
    }

    void IsSummonedBy(WorldObject* summoner) override
    {
        if (!summoner)
            return;

        //summoner is always the affected player
        _affectedPlayerGUID = summoner->GetGUID();
        if (Unit* affectedPlayer = summoner->ToUnit())
        {
            me->Attack(affectedPlayer, true);
        }

        _scheduler.CancelAll();
        _scheduler.Schedule(4s, [this](TaskContext context)
        {
            DoCastVictim(SPELL_SHADOW_BOLT);
            context.Repeat(6s);
        });
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (Unit* affectedPlayer = ObjectAccessor::GetUnit(*me, _affectedPlayerGUID))
        {
            affectedPlayer->RemoveAurasDueToSpell(SPELL_INSIDIOUS_WHISPER);
        }
    }

    void DamageTaken(Unit* who, uint32& damage, DamageEffectType, SpellSchoolMask) override
    {
        if (!who || who->GetGUID() != _affectedPlayerGUID)
        {
            damage = 0;
        }
    }

    bool CanAIAttack(Unit const* who) const override
    {
        return who->GetGUID() == _affectedPlayerGUID;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
        {
            return;
        }

        _scheduler.Update(diff);

        DoMeleeAttackIfReady();
    }
private:
    TaskScheduler _scheduler;
    InstanceScript* _instance;
    ObjectGuid _affectedPlayerGUID;
};

class spell_leotheras_whirlwind : public SpellScriptLoader
{
public:
    spell_leotheras_whirlwind() : SpellScriptLoader("spell_leotheras_whirlwind") { }

    class spell_leotheras_whirlwind_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_leotheras_whirlwind_SpellScript);

        void HandleScriptEffect(SpellEffIndex effIndex)
        {
            PreventHitDefaultEffect(effIndex);
            GetCaster()->GetThreatMgr().ResetAllThreat();

            if (roll_chance_i(33))
                if (Unit* target = GetCaster()->GetAI()->SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                    target->CastSpell(GetCaster(), SPELL_TAUNT, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_leotheras_whirlwind_SpellScript::HandleScriptEffect, EFFECT_2, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    SpellScript* GetSpellScript() const override
    {
        return new spell_leotheras_whirlwind_SpellScript();
    }
};

class spell_leotheras_chaos_blast : public SpellScriptLoader
{
public:
    spell_leotheras_chaos_blast() : SpellScriptLoader("spell_leotheras_chaos_blast") { }

    class spell_leotheras_chaos_blast_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_leotheras_chaos_blast_SpellScript);

        void HandleDummy(SpellEffIndex effIndex)
        {
            PreventHitDefaultEffect(effIndex);
            if (Unit* target = GetHitUnit())
                GetCaster()->CastSpell(target, SPELL_CHAOS_BLAST_TRIGGER, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_leotheras_chaos_blast_SpellScript::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
        }
    };

    SpellScript* GetSpellScript() const override
    {
        return new spell_leotheras_chaos_blast_SpellScript();
    }
};

class spell_leotheras_insidious_whisper : public SpellScriptLoader
{
public:
    spell_leotheras_insidious_whisper() : SpellScriptLoader("spell_leotheras_insidious_whisper") { }

    class spell_leotheras_insidious_whisper_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_leotheras_insidious_whisper_SpellScript);

        void FilterTargets(std::list<WorldObject*>& unitList)
        {
            if (Unit* victim = GetCaster()->GetVictim())
            {
                unitList.remove_if(Acore::ObjectGUIDCheck(victim->GetGUID(), true));
            }
        }

        void Register() override
        {
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_leotheras_insidious_whisper_SpellScript::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        }
    };

    SpellScript* GetSpellScript() const override
    {
        return new spell_leotheras_insidious_whisper_SpellScript();
    }

    class spell_leotheras_insidious_whisper_AuraScript : public AuraScript
    {
        PrepareAuraScript(spell_leotheras_insidious_whisper_AuraScript)

        void HandleEffectApply(AuraEffect const*  /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            GetUnitOwner()->CastSpell(GetUnitOwner(), SPELL_SUMMON_INNER_DEMON, true);
        }

        void HandleEffectRemove(AuraEffect const*  /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT)
            {
                if (InstanceScript* instance = GetUnitOwner()->GetInstanceScript())
                {
                    if (Creature* leotheras = instance->GetCreature(DATA_LEOTHERAS_THE_BLIND))
                    {
                        leotheras->CastSpell(GetUnitOwner(), SPELL_CONSUMING_MADNESS, true);
                    }
                }
            }
        }

        void Register() override
        {
            AfterEffectApply += AuraEffectApplyFn(spell_leotheras_insidious_whisper_AuraScript::HandleEffectApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
            AfterEffectRemove += AuraEffectRemoveFn(spell_leotheras_insidious_whisper_AuraScript::HandleEffectRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        }
    };

    AuraScript* GetAuraScript() const override
    {
        return new spell_leotheras_insidious_whisper_AuraScript();
    }
};

class spell_leotheras_demon_link : public SpellScriptLoader
{
public:
    spell_leotheras_demon_link() : SpellScriptLoader("spell_leotheras_demon_link") { }

    class spell_leotheras_demon_link_AuraScript : public AuraScript
    {
        PrepareAuraScript(spell_leotheras_demon_link_AuraScript);

        void OnPeriodic(AuraEffect const* aurEff)
        {
            PreventDefaultAction();
            if (Unit* victim = GetUnitOwner()->GetVictim())
            {
                GetUnitOwner()->CastSpell(victim, GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell, true);
            }
        }

        void Register() override
        {
            OnEffectPeriodic += AuraEffectPeriodicFn(spell_leotheras_demon_link_AuraScript::OnPeriodic, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        }
    };

    AuraScript* GetAuraScript() const override
    {
        return new spell_leotheras_demon_link_AuraScript();
    }
};

class spell_leotheras_clear_consuming_madness : public SpellScriptLoader
{
public:
    spell_leotheras_clear_consuming_madness() : SpellScriptLoader("spell_leotheras_clear_consuming_madness") { }

    class spell_leotheras_clear_consuming_madness_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_leotheras_clear_consuming_madness_SpellScript);

        void HandleScriptEffect(SpellEffIndex effIndex)
        {
            PreventHitDefaultEffect(effIndex);
            if (Unit* target = GetHitUnit())
            {
                Unit::Kill(GetCaster(), target);
            }
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_leotheras_clear_consuming_madness_SpellScript::HandleScriptEffect, EFFECT_1, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    SpellScript* GetSpellScript() const override
    {
        return new spell_leotheras_clear_consuming_madness_SpellScript();
    }
};

void AddSC_boss_leotheras_the_blind()
{
    RegisterSerpentShrineAI(boss_leotheras_the_blind);
    RegisterSerpentShrineAI(npc_inner_demon);
    new spell_leotheras_whirlwind();
    new spell_leotheras_chaos_blast();
    new spell_leotheras_insidious_whisper();
    new spell_leotheras_demon_link();
    new spell_leotheras_clear_consuming_madness();
}
