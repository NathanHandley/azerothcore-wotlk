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

#ifndef _EQ_ZONE_MAP_H
#define _EQ_ZONE_MAP_H

#include "Define.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// MergedWorlds: the server side map of a converted EverQuest zone (data/eqmaps/<map id>.eqmap, written by the MergedWorlds converter).
// It is what the EQ server used its zone .map and .wtr files for: every triangle the player collides with (floor heights, ceilings and
// line of sight), and EQ's own region BSP tree (water, lava, slime, freezing water and the other region types).  Everything is in WoW
// world coordinates.  A map that has one of these is an EverQuest map, and only those maps use any EverQuest movement behaviour.

enum EQRegionFlags : uint32
{
    EQ_REGION_NONE              = 0x00,
    EQ_REGION_WATER             = 0x01,
    EQ_REGION_LAVA              = 0x02,
    EQ_REGION_SLIME             = 0x04,
    EQ_REGION_FREEZING_WATER    = 0x08,
    EQ_REGION_PVP               = 0x10,
    EQ_REGION_ZONE_LINE         = 0x20,
    EQ_REGION_SLIPPERY          = 0x40,
    EQ_REGION_ANY_LIQUID        = EQ_REGION_WATER | EQ_REGION_LAVA | EQ_REGION_SLIME | EQ_REGION_FREEZING_WATER
};

constexpr float EQ_ZONE_MAP_INVALID_HEIGHT = -200000.0f;
constexpr float EQ_ZONE_MAP_LIQUID_ABOVE_SEARCH_DISTANCE = 50.0f; // How far under a point liquid is looked for when the point isn't in any

// The liquid in a column of an EQ zone (one liquid region, or touching ones, from its bottom to its surface)
struct EQZoneLiquidInfo
{
    float SurfaceZ = EQ_ZONE_MAP_INVALID_HEIGHT;
    float BottomZ = EQ_ZONE_MAP_INVALID_HEIGHT;
    uint32 RegionFlags = EQ_REGION_NONE;
    uint32 LiquidTypeID = 0; // LiquidType.dbc row
};

class EQZoneMap
{
public:
    bool LoadFromFile(std::string const& filePath);

    std::vector<uint32> const& GetMapIDs() const { return _mapIDs; }
    float GetWorldScale() const { return _worldScale; }

    // The first surface straight down from the point, no further than maxDistance below it, or EQ_ZONE_MAP_INVALID_HEIGHT
    float GetFloorZ(float x, float y, float z, float maxDistance) const;

    // The first surface straight up from the point, no further than maxDistance above it, or EQ_ZONE_MAP_INVALID_HEIGHT
    float GetCeilingZ(float x, float y, float z, float maxDistance) const;

    // EQ's best Z: the floor under a point a little above the given height, or failing that the nearest surface above it (the EQ server's
    // FindBestZ).  Surfaces more than maxDistanceDown below or maxDistanceUp above the given height don't count
    float FindBestZ(float x, float y, float z, float maxDistanceUp, float maxDistanceDown) const;

    // True when nothing in the zone is between the two points
    bool IsInLineOfSight(float x1, float y1, float z1, float x2, float y2, float z2) const;

    // The region flags at a point
    uint32 GetRegionFlags(float x, float y, float z) const;

    // The liquid the point is in, or failing that the nearest liquid under it (no more than maxDistanceBelow down).  False when neither
    bool GetLiquidInfo(float x, float y, float z, float maxDistanceBelow, EQZoneLiquidInfo& liquidInfo) const;

    struct Vec3
    {
        float X;
        float Y;
        float Z;
    };

private:
    struct BVHNode
    {
        float Min[3];
        float Max[3];
        int32 Left;           // Child node index, or -1 for a leaf
        int32 Right;
        uint32 FirstTriangle; // Leaves only, into _bvhTriangles
        uint32 TriangleCount;
    };

    struct RegionNode
    {
        float Normal[3];
        float Distance;
        int32 Front;
        int32 Back;
        uint32 RegionFlags;
        uint32 LiquidTypeID;
    };

    void BuildBVH();
    int32 BuildBVHNode(uint32 firstTriangle, uint32 triangleCount, std::vector<Vec3> const& centers, uint32 depth);
    bool Raycast(Vec3 const& origin, Vec3 const& direction, float maxDistance, float& hitDistance) const;
    bool RayIntersectsBox(BVHNode const& node, Vec3 const& origin, Vec3 const& inverseDirection, float maxDistance) const;
    bool RayIntersectsTriangle(uint32 triangleIndex, Vec3 const& origin, Vec3 const& direction, float& distance) const;
    void GetVerticalRegionIntervals(int32 nodeIndex, float x, float y, float bottomZ, float topZ,
        std::vector<std::pair<std::pair<float, float>, int32>>& intervals) const;

    std::vector<uint32> _mapIDs;
    float _worldScale = 0.0f;
    std::vector<Vec3> _vertices;
    std::vector<uint32> _triangleIndices;  // Three per triangle
    std::vector<uint32> _bvhTriangles;     // Triangle indexes ordered by BVH leaf
    std::vector<BVHNode> _bvhNodes;
    std::vector<RegionNode> _regionNodes;
};

class EQZoneMapMgr
{
public:
    static EQZoneMapMgr* instance();

    // Finds which maps have an EQ zone map (reads the headers of every file in data/eqmaps)
    void Initialize(std::string const& dataPath);

    // The EQ zone map of a map, loaded the first time it is asked for, or nullptr for maps that aren't EverQuest zones
    EQZoneMap const* GetZoneMap(uint32 mapID);

    bool IsEverQuestMap(uint32 mapID) const { return _filePathsByMapID.find(mapID) != _filePathsByMapID.end(); }

private:
    std::unordered_map<uint32, std::string> _filePathsByMapID;
    std::unordered_map<std::string, std::shared_ptr<EQZoneMap>> _zoneMapsByFilePath;
    std::mutex _loadLock;
};

#define sEQZoneMapMgr EQZoneMapMgr::instance()

#endif
