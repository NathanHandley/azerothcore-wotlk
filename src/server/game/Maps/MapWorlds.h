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

#ifndef _MAP_WORLDS_H
#define _MAP_WORLDS_H

#include "Define.h"

// MergedWorlds map worlds (Nathan 2026-11-14, E:\MW\SHARED.md section 8): which game world a map belongs to.  Every module whose game has maps of
// its own registers them here, and every module that cares asks the core (GetWorldOfMap) instead of deciding alone, so a map one module doesn't
// know (another game's) is never taken for Azeroth.  A map nobody registered is Azeroth's (MAP_WORLD_AZEROTH, WoW's own maps).
//
// World ids are SHARED.md's one numbering (1 Ragnarok Online, 3 Meridian 59, 4 GunBound, 5 EverQuest's Norrath, ...); the core defines only
// Azeroth's.  A module registers single maps (SetMapWorld) or a range of map IDs (SetMapRangeWorld), and can take all of its world's maps back
// (ClearWorld) before registering them again on a reload.  A single map's registration wins over any range it falls in, and of two ranges the
// later registered.
//
// Threads: registration happens at startup or on a module's reload; queries come from any thread (map updates, sessions), so both take a lock.

#define MAP_WORLD_AZEROTH           0   // Every map no module registered: WoW's own
#define MAX_MAP_WORLDS              16

namespace MapWorlds
{
    // A map's world.  world must be under MAX_MAP_WORLDS
    void SetMapWorld(uint32 mapID, uint8 world);

    // Every map ID from firstMapID to lastMapID, both included, is world's
    void SetMapRangeWorld(uint32 firstMapID, uint32 lastMapID, uint8 world);

    // Takes back every map and range the world registered (a module reloading its map list)
    void ClearWorld(uint8 world);

    // The world a map belongs to, MAP_WORLD_AZEROTH when no module registered it
    uint8 GetWorldOfMap(uint32 mapID);

    inline bool IsMapOfWorld(uint32 mapID, uint8 world) { return GetWorldOfMap(mapID) == world; }
}

#endif
