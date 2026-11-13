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
    constexpr float EQ_PATH_WALL_CLEARANCE = 0.75f;          // A move stopped by a wall ends this far short of it
    constexpr float EQ_PATH_ON_FLOOR_TOLERANCE = 0.1f;       // A height this close over or under a surface is on that surface
    constexpr uint32 EQ_PATH_COLLISION_MAX_GAP_STEPS = 1;    // Steps with no floor a straight move can cross (a crack); any more is the edge of the zone's mesh
    constexpr float EQ_PATH_BEND_TOLERANCE = 0.25f;          // A step is split where the floor halfway along it is further than this off its line
    constexpr uint32 EQ_PATH_BEND_MAX_DEPTH = 3;             // A step is split at most this many times over (into eighths)
    constexpr float EQ_PATH_CORNER_SEARCH_START = 0.5f;      // A route corner's floor is looked for from this far above it
    constexpr float EQ_PATH_SIMPLIFY_TOLERANCE = 0.15f;      // Steps within this height of a straight line between their neighbours are merged
    constexpr float EQ_PATH_SWIM_FLOOR_CLEARANCE = 0.5f;
    constexpr float EQ_PATH_SWIM_SURFACE_CLEARANCE = 0.5f;
    constexpr float EQ_PATH_GROUND_SEARCH_UP = 3.0f;
    constexpr float EQ_PATH_GROUND_SEARCH_DOWN = 20.0f;
    constexpr uint32 EQ_PATH_MAX_POINTS = 400;
    constexpr float EQ_PATH_PACKET_MAX_ACROSS = 250.0f;      // A move packet's points can be 256 yd across from the path's middle (see FitPathToMovePacket)
    constexpr float EQ_PATH_PACKET_MAX_UP = 120.0f;          // And 128 yd up or down
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

// A swimmer's height at a route corner under water: its swimming height there, kept off the bottom and under the surface.  The bottom is
// looked for from the higher of that height and the corner's own (the mesh's, which is just over the bottom), so a swimming height that
// would be under a rise in the bottom is still caught
float EQPathBuilder::GetSwimCornerZ(EQZoneMap const* zoneMap, G3D::Vector3 const& meshCorner, float swimZ, float surfaceZ)
{
    float floorZ = zoneMap->GetFloorZ(meshCorner.x, meshCorner.y, std::max(swimZ, meshCorner.z) + EQ_PATH_SWIM_FLOOR_CLEARANCE, EQ_PATH_STEP_SEARCH_DOWN);
    if (floorZ > EQ_ZONE_MAP_INVALID_HEIGHT)
        swimZ = std::max(swimZ, floorZ + EQ_PATH_SWIM_FLOOR_CLEARANCE);
    return std::min(swimZ, surfaceZ - EQ_PATH_SWIM_SURFACE_CLEARANCE);
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

bool EQPathBuilder::IsOnFloor(EQZoneMap const* zoneMap, float x, float y, float z)
{
    return zoneMap->GetFloorZ(x, y, z + EQ_PATH_ON_FLOOR_TOLERANCE, EQ_PATH_ON_FLOOR_TOLERANCE * 2.0f) > EQ_ZONE_MAP_INVALID_HEIGHT;
}

// How far along the line a mover stops for a wall hitDistance along it
float EQPathBuilder::GetStopFraction(G3D::Vector3 const& from, G3D::Vector3 const& to, float hitDistance)
{
    return std::max(0.0f, hitDistance - EQ_PATH_WALL_CLEARANCE) / std::max(0.001f, (to - from).length());
}

// Walks the line in the steps a direct path's hazard check uses.  Between two steps a wall is whatever is on the line from a body's height
// over one floor to the same height over the next
G3D::Vector3 EQPathBuilder::GetFirstCollisionPoint(EQZoneMap const* zoneMap, WorldObject const* source, G3D::Vector3 const& start, G3D::Vector3 const& end)
{
    float dx = end.x - start.x;
    float dy = end.y - start.y;
    float distance2D = std::sqrt(dx * dx + dy * dy);
    if (distance2D < 0.001f)
        return end;

    // Flyers and swimmers have no floor to stay on
    float surfaceZ = 0.0f;
    float hitDistance = 0.0f;
    float bodyHeight = EQ_PATH_STEP_REACH_HEIGHT;
    if (CanFly(source) || IsInLiquid(zoneMap, start.x, start.y, start.z, surfaceZ))
    {
        if (!zoneMap->GetFirstHitDistance(start.x, start.y, start.z + bodyHeight, end.x, end.y, end.z + bodyHeight, hitDistance))
            return end;
        return start + (end - start) * GetStopFraction(start, end, hitDistance);
    }

    float scale = zoneMap->GetWorldScale();
    float maxJump = EQ_PATH_HAZARD_MAX_JUMP_EQ * scale;
    uint32 stepCount = std::max(1u, uint32(std::ceil(distance2D / (EQ_PATH_HAZARD_STEP_EQ * scale))));
    G3D::Vector3 previousStep = start;
    G3D::Vector3 lastFloorStep = start;
    uint32 gapSteps = 0;
    for (uint32 step = 1; step <= stepCount; ++step)
    {
        float t = float(step) / float(stepCount);
        float nextT = float(step + 1) / float(stepCount);
        G3D::Vector3 stepPoint(start.x + dx * t, start.y + dy * t, 0.0f);
        stepPoint.z = SnapStepZOverGaps(zoneMap, previousStep, stepPoint.x, stepPoint.y, start.x + dx * nextT, start.y + dy * nextT);

        if (zoneMap->GetFirstHitDistance(previousStep.x, previousStep.y, previousStep.z + bodyHeight, stepPoint.x, stepPoint.y, stepPoint.z + bodyHeight, hitDistance))
        {
            G3D::Vector3 stopPoint = previousStep + (stepPoint - previousStep) * GetStopFraction(previousStep, stepPoint, hitDistance);
            stopPoint.z = SnapStepZ(zoneMap, stopPoint.x, stopPoint.y, previousStep.z, &previousStep);
            return IsOnFloor(zoneMap, stopPoint.x, stopPoint.y, stopPoint.z) ? stopPoint : lastFloorStep;
        }

        // A drop into liquid is a way in, as it is for a direct path
        if (std::fabs(stepPoint.z - previousStep.z) > maxJump && !IsInLiquid(zoneMap, stepPoint.x, stepPoint.y, stepPoint.z + EQ_PATH_ON_FLOOR_TOLERANCE, surfaceZ))
            return lastFloorStep;

        if (IsOnFloor(zoneMap, stepPoint.x, stepPoint.y, stepPoint.z))
        {
            lastFloorStep = stepPoint;
            gapSteps = 0;
        }
        else if (++gapSteps > EQ_PATH_COLLISION_MAX_GAP_STEPS)
            return lastFloorStep;
        previousStep = stepPoint;
    }
    return lastFloorStep;
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

// A monster move packet sends every point of a path but the last as its offset from the middle of the path (halfway between its two ends),
// packed into quarter yards in 11 bits across and 10 bits up.  So a point more than 256 yd across or 128 yd up or down from there wraps
// around, and the client walks the mover toward somewhere else entirely until the path catches up with it (a long roam floating off the
// ground or dropping through it at the start of the trip).  A path that long ends at its last point that still fits, and the mover asks for
// the rest when it gets there
bool EQPathBuilder::FitPathToMovePacket(Movement::PointsArray& pathPoints)
{
    if (pathPoints.size() < 3)
        return false;

    // Every point is within the path's bounds of its middle, so a path that small always fits
    G3D::Vector3 low = pathPoints[0];
    G3D::Vector3 high = pathPoints[0];
    for (G3D::Vector3 const& point : pathPoints)
    {
        low = low.min(point);
        high = high.max(point);
    }
    G3D::Vector3 extent = high - low;
    if (extent.x <= EQ_PATH_PACKET_MAX_ACROSS && extent.y <= EQ_PATH_PACKET_MAX_ACROSS && extent.z <= EQ_PATH_PACKET_MAX_UP)
        return false;

    for (uint32 lastIndex = 2; lastIndex < pathPoints.size(); ++lastIndex)
    {
        G3D::Vector3 middle = (pathPoints[0] + pathPoints[lastIndex]) * 0.5f;
        for (uint32 i = 1; i < lastIndex; ++i)
        {
            G3D::Vector3 offset = middle - pathPoints[i];
            if (std::fabs(offset.x) > EQ_PATH_PACKET_MAX_ACROSS || std::fabs(offset.y) > EQ_PATH_PACKET_MAX_ACROSS || std::fabs(offset.z) > EQ_PATH_PACKET_MAX_UP)
            {
                pathPoints.resize(lastIndex);
                return true;
            }
        }
    }
    return false;
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
    // they go back on the floor too.  Under water the mesh is the bottom, so a swimmer's corner keeps its depth instead, on the line between
    // the trip's ends as it is between corners (otherwise it would dive to the bottom at the start of every long swim and come back up at
    // the end)
    Movement::PointsArray corners;
    corners.push_back(start);
    if (routeCorners.size() > 2)
    {
        float routeLength = 0.0f;
        for (uint32 i = 1; i < routeCorners.size(); ++i)
            routeLength += (routeCorners[i] - routeCorners[i - 1]).xy().length();
        float lengthSoFar = 0.0f;
        for (uint32 i = 1; i + 1 < routeCorners.size(); ++i)
        {
            G3D::Vector3 corner = routeCorners[i];
            lengthSoFar += (routeCorners[i] - routeCorners[i - 1]).xy().length();
            float surfaceZ = 0.0f;
            if (swims && IsInLiquid(zoneMap, corner.x, corner.y, corner.z, surfaceZ))
                corner.z = GetSwimCornerZ(zoneMap, corner, start.z + (end.z - start.z) * (routeLength > 0.001f ? lengthSoFar / routeLength : 0.0f), surfaceZ);
            else
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
    if (FitPathToMovePacket(pathPoints))
        cutShort = true;
    return cutShort;
}
