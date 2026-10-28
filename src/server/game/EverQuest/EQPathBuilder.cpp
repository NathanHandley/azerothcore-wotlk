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
    constexpr float EQ_PATH_STEP_CLIMB = 2.0f;               // A step's floor search starts this far above the last step's height
    constexpr float EQ_PATH_STEP_SEARCH_DOWN = 20.0f;        // How far under the last step's height a step's floor can be (a drop)
    constexpr float EQ_PATH_STEP_SEARCH_UP = 6.0f;           // How far over the search start a step's floor can be (a slope or stair steeper than the climb)
    constexpr float EQ_PATH_STEP_REACH_HEIGHT = 1.0f;        // A climb is reachable when the line between this far over both floors is clear
    constexpr float EQ_PATH_STEP_REACH_MIN_CLIMB = 0.5f;     // Climbs smaller than this aren't checked
    constexpr float EQ_PATH_STEP_REACH_UNDER_OFFSET = 0.01f; // The floor under an unreachable surface is looked for from just under it
    constexpr float EQ_PATH_STEP_GAP_DROP = 4.0f;            // A drop bigger than this with the floor back at the old height a step later is a gap
    constexpr float EQ_PATH_BEND_TOLERANCE = 0.25f;          // A step is split where the floor halfway along it is further than this off its line
    constexpr uint32 EQ_PATH_BEND_MAX_DEPTH = 3;             // A step is split at most this many times over (into eighths)
    constexpr float EQ_PATH_CORNER_SEARCH_START = 0.5f;      // A route corner's floor is looked for from this far above it
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

// The floor for one step of a leg, found from the height of the step before it, as the EQ server's FixZ worked from a mover's own
// height: the floor straight down from a little above that height, and the surface straight up from there (a slope or stair steeper than
// the climb), with the one closer to that height winning (the EQ server's FindClosestZ).  The line between the leg's ends plays no part,
// since the ground between two points can be far above or below it.  With no surface at all the step keeps the height.
// A climb also has to be reachable from where the mover stands (reachFrom, when given): a surface that can only be reached through
// another one is a ceiling or the ramp overhead (a steep ramp going down under one gets closer to it than to its own floor), and the step
// takes the floor under it instead
float EQPathBuilder::SnapStepZ(EQZoneMap const* zoneMap, float x, float y, float previousZ, G3D::Vector3 const* reachFrom)
{
    float searchStartZ = previousZ + EQ_PATH_STEP_CLIMB;
    float downZ = zoneMap->GetFloorZ(x, y, searchStartZ, EQ_PATH_STEP_CLIMB + EQ_PATH_STEP_SEARCH_DOWN);
    float upZ = zoneMap->GetCeilingZ(x, y, searchStartZ, EQ_PATH_STEP_SEARCH_UP);
    bool hasDown = downZ > EQ_ZONE_MAP_INVALID_HEIGHT;
    bool hasUp = upZ > EQ_ZONE_MAP_INVALID_HEIGHT;
    float stepZ = previousZ;
    if (hasDown && hasUp)
        stepZ = std::fabs(downZ - previousZ) <= std::fabs(upZ - previousZ) ? downZ : upZ;
    else if (hasDown)
        stepZ = downZ;
    else if (hasUp)
        stepZ = upZ;
    else
    {
        // Nothing in reach: a cliff taller than a climb, where the way on is blocked at the mover's height (it goes up the cliff instead of
        // walking on through the rock, and a direct path's hazard check sees the jump), or a hole in the zone's mesh, where it keeps its height
        if (reachFrom && !zoneMap->IsInLineOfSight(reachFrom->x, reachFrom->y, previousZ + EQ_PATH_STEP_REACH_HEIGHT, x, y, previousZ + EQ_PATH_STEP_REACH_HEIGHT))
        {
            float cliffTopZ = zoneMap->GetCeilingZ(x, y, searchStartZ, EQ_PATH_STEP_SEARCH_DOWN);
            if (cliffTopZ > EQ_ZONE_MAP_INVALID_HEIGHT)
                return cliffTopZ;
        }
        return previousZ;
    }

    if (reachFrom && stepZ > previousZ + EQ_PATH_STEP_REACH_MIN_CLIMB
        && !zoneMap->IsInLineOfSight(reachFrom->x, reachFrom->y, reachFrom->z + EQ_PATH_STEP_REACH_HEIGHT, x, y, stepZ + EQ_PATH_STEP_REACH_HEIGHT))
    {
        float lowerSearchStartZ = stepZ - EQ_PATH_STEP_REACH_UNDER_OFFSET;
        float lowerZ = zoneMap->GetFloorZ(x, y, lowerSearchStartZ, lowerSearchStartZ - (previousZ - EQ_PATH_STEP_SEARCH_DOWN));
        if (lowerZ > EQ_ZONE_MAP_INVALID_HEIGHT)
            return lowerZ;
    }
    return stepZ;
}

// A step's floor, unless it is a big drop with the floor back near the old height a step further on.  That is a crack or a gap in the
// zone's mesh (between a bridge's planks, under a doorway's sill), which the mover walks over instead of dropping into
float EQPathBuilder::SnapStepZOverGaps(EQZoneMap const* zoneMap, G3D::Vector3 const& previousStep, float x, float y, float nextX, float nextY)
{
    float previousZ = previousStep.z;
    float stepZ = SnapStepZ(zoneMap, x, y, previousZ, &previousStep);
    if (previousZ - stepZ <= EQ_PATH_STEP_GAP_DROP)
        return stepZ;
    float nextZ = SnapStepZ(zoneMap, nextX, nextY, previousZ, nullptr);
    if (std::fabs(nextZ - previousZ) <= EQ_PATH_STEP_CLIMB)
        return previousZ;
    return stepZ;
}

// The floor under a route corner.  The navigation mesh only roughly follows the floor (its stairs are ramps), so a corner can be a little
// under the floor as easily as over it, and the closer of the floor under it and the surface over it is the one it meant.  Taking the
// floor under it first would drop a corner that is just under a stair to the floor below the stairs
float EQPathBuilder::GetCornerGroundZ(EQZoneMap const* zoneMap, WorldObject const* source, float x, float y, float z)
{
    float surfaceZ = 0.0f;
    if (CanFly(source) || IsInLiquid(zoneMap, x, y, z, surfaceZ))
        return GetGroundZ(zoneMap, source, x, y, z);
    float searchStartZ = z + EQ_PATH_CORNER_SEARCH_START;
    float downZ = zoneMap->GetFloorZ(x, y, searchStartZ, EQ_PATH_GROUND_SEARCH_DOWN + EQ_PATH_CORNER_SEARCH_START);
    float upZ = zoneMap->GetCeilingZ(x, y, searchStartZ, EQ_PATH_GROUND_SEARCH_UP);
    bool hasDown = downZ > EQ_ZONE_MAP_INVALID_HEIGHT;
    bool hasUp = upZ > EQ_ZONE_MAP_INVALID_HEIGHT;
    if (hasDown && hasUp)
        return std::fabs(downZ - z) <= std::fabs(upZ - z) ? downZ : upZ;
    if (hasDown)
        return downZ;
    if (hasUp)
        return upZ;
    return z;
}

// Adds the step from one floor point to the next, split where the floor bends away from the straight line between them (a dune's crest,
// the bottom of a dip, the edge of a stair) so the mover neither cuts through the ground nor hangs over it between the two
void EQPathBuilder::AddStepWithBends(EQZoneMap const* zoneMap, G3D::Vector3 const& from, G3D::Vector3 const& to, uint32 depth, Movement::PointsArray& legPoints)
{
    if (depth < EQ_PATH_BEND_MAX_DEPTH)
    {
        float lineZ = (from.z + to.z) * 0.5f;
        G3D::Vector3 middle((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f, 0.0f);
        middle.z = SnapStepZ(zoneMap, middle.x, middle.y, lineZ, nullptr);
        if (std::fabs(middle.z - lineZ) > EQ_PATH_BEND_TOLERANCE)
        {
            AddStepWithBends(zoneMap, from, middle, depth + 1, legPoints);
            AddStepWithBends(zoneMap, middle, to, depth + 1, legPoints);
            return;
        }
    }
    legPoints.push_back(to);
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
    G3D::Vector3 previousStep = start;
    for (uint32 step = 1; step <= stepCount; ++step)
    {
        float t = float(step) / float(stepCount);
        float x = start.x + dx * t;
        float y = start.y + dy * t;
        float stepZ = SnapStepZ(zoneMap, x, y, previousStep.z, &previousStep);
        float surfaceZ = 0.0f;
        if (std::fabs(stepZ - previousStep.z) > maxJump && !IsInLiquid(zoneMap, x, y, stepZ + 0.1f, surfaceZ))
            return false;
        previousStep = G3D::Vector3(x, y, stepZ);
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
    G3D::Vector3 previousStep = from;
    bool previousOnFloor = true;
    for (uint32 step = 1; step <= stepCount; ++step)
    {
        float t = float(step) / float(stepCount);
        float x = from.x + dx * t;
        float y = from.y + dy * t;
        float stepZ = to.z; // The leg's end is already on the floor (or is a route corner the caller placed)
        if (step < stepCount)
        {
            float nextT = float(step + 1) / float(stepCount);
            stepZ = SnapStepZOverGaps(zoneMap, previousStep, x, y, from.x + dx * nextT, from.y + dy * nextT);
        }
        bool onFloor = true;
        float surfaceZ = 0.0f;
        float expectedZ = from.z + (to.z - from.z) * t;
        if (swims && (IsInLiquid(zoneMap, x, y, expectedZ, surfaceZ) || IsInLiquid(zoneMap, x, y, stepZ + EQ_PATH_SWIM_FLOOR_CLEARANCE, surfaceZ)))
        {
            // Swimming keeps the height on the line between the leg's ends, kept off the floor and under the surface
            if (step < stepCount)
            {
                stepZ = expectedZ;
                float floorZ = zoneMap->GetFloorZ(x, y, expectedZ + EQ_PATH_SWIM_FLOOR_CLEARANCE, EQ_PATH_STEP_SEARCH_DOWN);
                if (floorZ > EQ_ZONE_MAP_INVALID_HEIGHT)
                    stepZ = std::max(stepZ, floorZ + EQ_PATH_SWIM_FLOOR_CLEARANCE);
                stepZ = std::min(stepZ, surfaceZ - EQ_PATH_SWIM_SURFACE_CLEARANCE);
            }
            onFloor = false;
        }
        G3D::Vector3 stepPoint(x, y, stepZ);
        if (onFloor && previousOnFloor)
            AddStepWithBends(zoneMap, legPoints.back(), stepPoint, 0, legPoints);
        else
            legPoints.push_back(stepPoint);
        previousStep = stepPoint;
        previousOnFloor = onFloor;
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
            corner.z = GetCornerGroundZ(zoneMap, source, corner.x, corner.y, corner.z);
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
