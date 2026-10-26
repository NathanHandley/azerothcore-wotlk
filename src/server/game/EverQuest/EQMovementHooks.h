/*
 * Author: Nathan Handley (nathanhandley@protonmail.com)
 * Copyright (c) 2026 Nathan Handley + Automation
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

#ifndef _EQ_MOVEMENT_HOOKS_H
#define _EQ_MOVEMENT_HOOKS_H

#include "Define.h"

class Creature;
class MovementGenerator;

// MergedWorlds: lets mod-everquest give creatures on EverQuest maps their EverQuest idle movement (grids and roam boxes), which it keeps the
// data for.  Only consulted for creatures on EverQuest maps (maps with an EQ zone map)
namespace EverQuestMovementHooks
{
    // Returns a new idle movement generator for the creature, or nullptr to use the creature's normal one
    typedef MovementGenerator* (*IdleMovementSelectorFunction)(Creature* creature);

    AC_GAME_API extern IdleMovementSelectorFunction IdleMovementSelector;
}

#endif
