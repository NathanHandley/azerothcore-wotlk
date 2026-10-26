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

#include "EQPathBuilder.h"
#include "EQMovementHooks.h"
#include "EQZoneMap.h"
#include "Unit.h"
#include <cmath>

namespace
{
    // Distances in EQ units, which become yards through the zone's world scale
    constexpr float EQ_PATH_STEP_EQ = 10.0f;                 // The EQ server put a moving mob back on the floor every 10 units
    constexpr float EQ_PATH_DIRECT_MAX_DISTANCE_EQ = 200.0f; // Longest trip that can go straight without a route
    constexpr float EQ_PATH_HAZARD_STEP_EQ = 5.0f;           // How often a straight trip is checked for drops and climbs
    constexpr float EQ_PATH_HAZARD_MAX_JUMP_EQ = 12.0f;      // A height change bigger than this between checks is a hazard
    constexpr uint32 EQ_PATH_HAZARD_MAX_STEPS = 40;

    // Distances in yards
    constexpr float EQ_PATH_LOS_HEIGHT = 2.0f;               // Line of sight is checked this far above the floor at both ends
    constexpr float EQ_PATH_STEP_CLIMB = 2.0f;               // A step's floor search starts this far above the higher of the expected and last heights
    constexpr float EQ_PATH_STEP_SEARCH_DOWN = 10.0f;
    constexpr float EQ_PATH_STEP_SEARCH_UP = 6.0f;
    constexpr float EQ_PATH_STEP_MAX_CHANGE = 15.0f;         // A floor this far from the expected height is ignored (the expected height is used)
    constexpr float EQ_PATH_SIMPLIFY_TOLERANCE = 0.15f;      // Steps within this height of a straight line between their neighbours are merged
    constexpr float EQ_PATH_SWIM_FLOOR_CLEARANCE = 0.5f;
    constexpr float EQ_PATH_SWIM_SURFACE_CLEARANCE = 0.5f;
    constexpr float EQ_PATH_GROUND_SEARCH_UP = 3.0f;
    constexpr float EQ_PATH_GROUND_SEARCH_DOWN = 20.0f;
    constexpr uint32 EQ_PATH_MAX_POINTS = 400;
}

EverQuestMovementHooks::IdleMovementSelectorFunction EverQuestMovementHooks::IdleMovementSelector = nullptr;

bool EQPathBuilder::CanFly(WorldObject const* source)
{
    Unit const* unit = source ? source->ToUnit() : nullptr;
    if (!unit)
        return false;
    return unit->CanFly() || unit->IsFlying() || unit->IsLevitating();
}

bool EQPathBuilder::IsInLiquid(EQZoneMap const* zoneMap, float x, float y, float z, float& surfaceZ)
{
    EQZoneLiquidInfo liquidInfo;
    if (!zoneMap->GetLiquidInfo(x, y, z, 0.0f, liquidInfo))
        return false;
    if (z > liquidInfo.SurfaceZ || z < liquidInfo.BottomZ)
        return false;
    surfaceZ = liquidInfo.SurfaceZ;
    return true;
}

float EQPathBuilder::GetGroundZ(EQZoneMap const* zoneMap, WorldObject const* source, float x, float y, float z)
{
    if (CanFly(source))
        return z;
    float surfaceZ = 0.0f;
    if (IsInLiquid(zoneMap, x, y, z, surfaceZ))
    {
        float floorZ = zoneMap->GetFloorZ(x, y, z + EQ_PATH_SWIM_FLOOR_CLEARANCE, EQ_PATH_GROUND_SEARCH_DOWN);
        if (floorZ > EQ_ZONE_MAP_INVALID_HEIGHT && z < floorZ + EQ_PATH_SWIM_FLOOR_CLEARANCE)
            return floorZ + EQ_PATH_SWIM_FLOOR_CLEARANCE;
        return z;
    }
    float groundZ = zoneMap->FindBestZ(x, y, z, EQ_PATH_GROUND_SEARCH_UP, EQ_PATH_GROUND_SEARCH_DOWN);
    return groundZ > EQ_ZONE_MAP_INVALID_HEIGHT ? groundZ : z;
}

// The floor for one step of a leg.  The floor straight down (from a little above the higher of where the step was expected and where
// the last step was) and the surface straight up (when the step is under the ground) are the candidates, and the one closer to the
// expected height wins (the EQ server's FindClosestZ)
float EQPathBuilder::SnapStepZ(EQZoneMap const* zoneMap, float x, float y, float expectedZ, float previousZ)
{
    float referenceZ = std::max(expectedZ, previousZ);
    float downZ = zoneMap->GetFloorZ(x, y, referenceZ + EQ_PATH_STEP_CLIMB, EQ_PATH_STEP_CLIMB + EQ_PATH_STEP_SEARCH_DOWN);
    float upZ = zoneMap->GetCeilingZ(x, y, expectedZ, EQ_PATH_STEP_SEARCH_UP);
    bool hasDown = downZ > EQ_ZONE_MAP_INVALID_HEIGHT && std::fabs(downZ - referenceZ) <= EQ_PATH_STEP_MAX_CHANGE;
    bool hasUp = upZ > EQ_ZONE_MAP_INVALID_HEIGHT && upZ > referenceZ + EQ_PATH_STEP_CLIMB;
    if (hasDown && hasUp)
        return std::fabs(downZ - referenceZ) <= std::fabs(upZ - referenceZ) ? downZ : upZ;
    if (hasDown)
        return downZ;
    if (hasUp)
        return upZ;
    return expectedZ;
}

bool EQPathBuilder::CanGoDirectly(EQZoneMap const* zoneMap, WorldObject const* source, G3D::Vector3 const& start, G3D::Vector3 const& end)
{
    float scale = zoneMap->GetWorldScale();
    float dx = end.x - start.x;
    float dy = end.y - start.y;
    float distance2D = std::sqrt(dx * dx + dy * dy);
    if (distance2D > EQ_PATH_DIRECT_MAX_DISTANCE_EQ * scale)
        return false;
    if (!zoneMap->IsInLineOfSight(start.x, start.y, start.z + EQ_PATH_LOS_HEIGHT, end.x, end.y, end.z + EQ_PATH_LOS_HEIGHT))
        return false;
    if (CanFly(source))
        return true;

    // Both ends in liquid swim straight; otherwise look for drops and climbs on the way (a drop into liquid is fine)
    float startSurfaceZ = 0.0f;
    float endSurfaceZ = 0.0f;
    if (IsInLiquid(zoneMap, start.x, start.y, start.z, startSurfaceZ) && IsInLiquid(zoneMap, end.x, end.y, end.z, endSurfaceZ))
        return true;
    float hazardStep = EQ_PATH_HAZARD_STEP_EQ * scale;
    float maxJump = EQ_PATH_HAZARD_MAX_JUMP_EQ * scale;
    uint32 stepCount = std::min(EQ_PATH_HAZARD_MAX_STEPS, uint32(std::ceil(distance2D / hazardStep)));
    if (stepCount < 2)
        return true;
    float previousZ = start.z;
    for (uint32 step = 1; step <= stepCount; ++step)
    {
        float t = float(step) / float(stepCount);
        float x = start.x + dx * t;
        float y = start.y + dy * t;
        float expectedZ = start.z + (end.z - start.z) * t;
        float stepZ = SnapStepZ(zoneMap, x, y, expectedZ, previousZ);
        float surfaceZ = 0.0f;
        if (std::fabs(stepZ - previousZ) > maxJump && !IsInLiquid(zoneMap, x, y, stepZ + 0.1f, surfaceZ))
            return false;
        previousZ = stepZ;
    }
    return true;
}

// Douglas-Peucker on the step heights of one leg (the steps of a leg are on one line seen from above, so only the heights matter)
void EQPathBuilder::SimplifyLeg(Movement::PointsArray const& legPoints, uint32 firstIndex, uint32 lastIndex, std::vector<bool>& keep)
{
    if (lastIndex <= firstIndex + 1)
        return;
    G3D::Vector3 const& first = legPoints[firstIndex];
    G3D::Vector3 const& last = legPoints[lastIndex];
    float legLength = (last - first).xy().length();
    float worstDeviation = 0.0f;
    uint32 worstIndex = firstIndex;
    for (uint32 i = firstIndex + 1; i < lastIndex; ++i)
    {
        float t = legLength > 0.001f ? (legPoints[i] - first).xy().length() / legLength : 0.0f;
        float lineZ = first.z + (last.z - first.z) * t;
        float deviation = std::fabs(legPoints[i].z - lineZ);
        if (deviation > worstDeviation)
        {
            worstDeviation = deviation;
            worstIndex = i;
        }
    }
    if (worstDeviation <= EQ_PATH_SIMPLIFY_TOLERANCE)
        return;
    keep[worstIndex] = true;
    SimplifyLeg(legPoints, firstIndex, worstIndex, keep);
    SimplifyLeg(legPoints, worstIndex, lastIndex, keep);
}

// Adds the steps of one leg after its start (which is already in the path), with each step's height put on the floor
void EQPathBuilder::AddLeg(EQZoneMap const* zoneMap, G3D::Vector3 const& from, G3D::Vector3 const& to, bool swims, Movement::PointsArray& pathPoints)
{
    float scale = zoneMap->GetWorldScale();
    float stepLength = EQ_PATH_STEP_EQ * scale;
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float distance2D = std::sqrt(dx * dx + dy * dy);
    uint32 stepCount = std::max(1u, uint32(std::ceil(distance2D / stepLength)));

    Movement::PointsArray legPoints;
    legPoints.reserve(stepCount + 1);
    legPoints.push_back(from);
    float previousZ = from.z;
    for (uint32 step = 1; step <= stepCount; ++step)
    {
        float t = float(step) / float(stepCount);
        float x = from.x + dx * t;
        float y = from.y + dy * t;
        float expectedZ = from.z + (to.z - from.z) * t;
        float stepZ = expectedZ;
        float surfaceZ = 0.0f;
        if (swims && IsInLiquid(zoneMap, x, y, expectedZ, surfaceZ))
        {
            // Swimming keeps the expected height, kept off the floor and under the surface
            float floorZ = zoneMap->GetFloorZ(x, y, expectedZ + EQ_PATH_SWIM_FLOOR_CLEARANCE, EQ_PATH_STEP_SEARCH_DOWN);
            if (floorZ > EQ_ZONE_MAP_INVALID_HEIGHT)
                stepZ = std::max(stepZ, floorZ + EQ_PATH_SWIM_FLOOR_CLEARANCE);
            stepZ = std::min(stepZ, surfaceZ - EQ_PATH_SWIM_SURFACE_CLEARANCE);
        }
        else if (step == stepCount)
            stepZ = to.z; // The leg's end is already on the floor (or is a route corner the caller placed)
        else
            stepZ = SnapStepZ(zoneMap, x, y, expectedZ, previousZ);
        legPoints.push_back(G3D::Vector3(x, y, stepZ));
        previousZ = stepZ;
    }

    std::vector<bool> keep(legPoints.size(), false);
    keep.front() = true;
    keep.back() = true;
    SimplifyLeg(legPoints, 0, uint32(legPoints.size() - 1), keep);
    for (uint32 i = 1; i < legPoints.size(); ++i)
        if (keep[i])
            pathPoints.push_back(legPoints[i]);
}

bool EQPathBuilder::BuildPath(EQZoneMap const* zoneMap, WorldObject const* source, G3D::Vector3 const& start, G3D::Vector3 const& end,
    Movement::PointsArray const& routeCorners, Movement::PointsArray& pathPoints)
{
    pathPoints.clear();
    pathPoints.push_back(start);

    // Flyers go straight
    if (CanFly(source))
    {
        pathPoints.push_back(end);
        return false;
    }

    Unit const* unit = source ? source->ToUnit() : nullptr;
    bool swims = unit ? unit->CanSwim() : false;

    // The corners to walk between: the route, or a straight line.  The route's own corner heights come from the navigation mesh, so
    // they go back on the floor too
    Movement::PointsArray corners;
    corners.push_back(start);
    if (routeCorners.size() > 2)
    {
        for (uint32 i = 1; i + 1 < routeCorners.size(); ++i)
        {
            G3D::Vector3 corner = routeCorners[i];
            corner.z = GetGroundZ(zoneMap, source, corner.x, corner.y, corner.z);
            corners.push_back(corner);
        }
    }
    corners.push_back(end);

    bool cutShort = false;
    for (uint32 i = 1; i < corners.size(); ++i)
    {
        AddLeg(zoneMap, corners[i - 1], corners[i], swims, pathPoints);
        if (pathPoints.size() >= EQ_PATH_MAX_POINTS)
        {
            pathPoints.resize(EQ_PATH_MAX_POINTS);
            cutShort = true;
            break;
        }
    }
    return cutShort;
}
