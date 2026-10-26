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

#ifndef _EQ_PATH_BUILDER_H
#define _EQ_PATH_BUILDER_H

#include "Define.h"
#include "MoveSplineInitArgs.h"
#include <G3D/Vector3.h>

class EQZoneMap;
class WorldObject;

// MergedWorlds: EverQuest style paths for units on EverQuest maps (maps with an EQ zone map).  It is the EQ server's movement turned into
// WoW spline paths:
//  - A short trip in plain sight with no drops or walls in the way goes straight (the EQ pathfinders' direct line check)
//  - Anything else follows the route corners it is given (the navigation mesh's), or goes straight when there is no route, as EQ's
//    "run to target" stuck behaviour did
//  - Every leg is walked in steps of 10 EQ units with the height put back on the floor at each step (EQ's FixZ while moving), so the
//    path follows the ground exactly instead of cutting through hills or hanging in the air.  Steps on a straight slope are merged
//  - In liquid a swimmer keeps its height (between the floor and the surface) instead, and a flyer always goes straight
class EQPathBuilder
{
public:
    // The path from start to end, starting with start.  routeCorners is the route found by the navigation mesh between them (empty when
    // there is none, or when a direct path should be tried first).  Returns true when the path got cut short (too many points), so the
    // mover has to ask for the rest when it gets there
    static bool BuildPath(EQZoneMap const* zoneMap, WorldObject const* source, G3D::Vector3 const& start, G3D::Vector3 const& end,
        Movement::PointsArray const& routeCorners, Movement::PointsArray& pathPoints);

    // True when a unit can go straight between the points the EQ way (close enough, in line of sight, and no big drop or climb on the way)
    static bool CanGoDirectly(EQZoneMap const* zoneMap, WorldObject const* source, G3D::Vector3 const& start, G3D::Vector3 const& end);

    // The floor height under a point for a unit on an EQ map (or its own height when it flies or swims there)
    static float GetGroundZ(EQZoneMap const* zoneMap, WorldObject const* source, float x, float y, float z);

    static bool CanFly(WorldObject const* source);

private:
    static bool IsInLiquid(EQZoneMap const* zoneMap, float x, float y, float z, float& surfaceZ);
    static float SnapStepZ(EQZoneMap const* zoneMap, float x, float y, float expectedZ, float previousZ);
    static void AddLeg(EQZoneMap const* zoneMap, G3D::Vector3 const& from, G3D::Vector3 const& to, bool swims, Movement::PointsArray& pathPoints);
    static void SimplifyLeg(Movement::PointsArray const& legPoints, uint32 firstIndex, uint32 lastIndex, std::vector<bool>& keep);
};

#endif
