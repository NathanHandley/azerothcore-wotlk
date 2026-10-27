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

#ifndef _EQ_AREA_TRIGGER_HOOKS_H
#define _EQ_AREA_TRIGGER_HOOKS_H

#include "Define.h"

class Player;
struct AreaTrigger;

// MergedWorlds: game masters skip area trigger scripts, but an EverQuest zone line's script is what keeps the player's place along the
// line and their facing, so mod-everquest takes zone lines for game masters through this
namespace EverQuestAreaTriggerHooks
{
    // Returns true when it handled the trigger (the player was sent through the zone line)
    typedef bool (*GameMasterZoneLineHandlerFunction)(Player* player, AreaTrigger const* trigger);

    AC_GAME_API extern GameMasterZoneLineHandlerFunction GameMasterZoneLineHandler;
}

#endif
