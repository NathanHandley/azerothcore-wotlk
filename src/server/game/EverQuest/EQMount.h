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

#ifndef _EQ_MOUNT_H
#define _EQ_MOUNT_H

#include "Define.h"

class Unit;

// MergedWorlds: an EverQuest horse.  Its spell carries a Dummy aura with this misc value, the EQ spell effect number.  On an EverQuest map a rider
// can fight, cast and keep a pet, which WoW mounts can't.  Anywhere else, or on any other mount, the core behaves as stock
namespace EQMount
{
    constexpr int32 DUMMY_MISC_VALUE = 113;

    AC_GAME_API bool IsRidingEQHorse(Unit const* unit);
}

#endif
