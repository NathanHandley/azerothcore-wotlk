/*
 * Author: Nathan Handley (nathanhandley@protonmail.com) + Automation
 * Copyright (c) 2026 Nathan Handley + Automation
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "EQMount.h"
#include "EQZoneMap.h"
#include "SpellAuraEffects.h"
#include "Unit.h"

bool EQMount::IsRidingEQHorse(Unit const* unit)
{
    if (unit == nullptr || unit->HasAuraType(SPELL_AURA_MOUNTED) == false || sEQZoneMapMgr->IsEverQuestMap(unit->GetMapId()) == false)
        return false;
    for (AuraEffect const* dummyEffect : unit->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        if (dummyEffect->GetMiscValue() == DUMMY_MISC_VALUE)
            return true;
    return false;
}
