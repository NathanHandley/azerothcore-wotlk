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

#include "MapWorlds.h"
#include "Log.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace
{
    struct MapWorldRange
    {
        uint32 FirstMapID = 0;
        uint32 LastMapID = 0;
        uint8 World = MAP_WORLD_AZEROTH;
    };

    std::shared_mutex RegistryMutex;
    std::unordered_map<uint32, uint8> WorldsByMapID;
    std::vector<MapWorldRange> Ranges; // In registration order: the last one a map falls in wins
}

void MapWorlds::SetMapWorld(uint32 mapID, uint8 world)
{
    if (world >= MAX_MAP_WORLDS)
    {
        LOG_ERROR("maps", "MapWorlds::SetMapWorld: world {} for map {} is out of range", world, mapID);
        return;
    }
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    WorldsByMapID[mapID] = world;
}

void MapWorlds::SetMapRangeWorld(uint32 firstMapID, uint32 lastMapID, uint8 world)
{
    if (world >= MAX_MAP_WORLDS || lastMapID < firstMapID)
    {
        LOG_ERROR("maps", "MapWorlds::SetMapRangeWorld: world {} for maps {} to {} is out of range", world, firstMapID, lastMapID);
        return;
    }
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    MapWorldRange range;
    range.FirstMapID = firstMapID;
    range.LastMapID = lastMapID;
    range.World = world;
    Ranges.push_back(range);
}

void MapWorlds::ClearWorld(uint8 world)
{
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    for (auto mapIter = WorldsByMapID.begin(); mapIter != WorldsByMapID.end();)
    {
        if (mapIter->second == world)
            mapIter = WorldsByMapID.erase(mapIter);
        else
            ++mapIter;
    }
    Ranges.erase(std::remove_if(Ranges.begin(), Ranges.end(), [world](MapWorldRange const& range) { return range.World == world; }), Ranges.end());
}

uint8 MapWorlds::GetWorldOfMap(uint32 mapID)
{
    std::shared_lock<std::shared_mutex> lock(RegistryMutex);
    auto mapIter = WorldsByMapID.find(mapID);
    if (mapIter != WorldsByMapID.end())
        return mapIter->second;
    for (auto rangeIter = Ranges.rbegin(); rangeIter != Ranges.rend(); ++rangeIter)
        if (mapID >= rangeIter->FirstMapID && mapID <= rangeIter->LastMapID)
            return rangeIter->World;
    return MAP_WORLD_AZEROTH;
}
