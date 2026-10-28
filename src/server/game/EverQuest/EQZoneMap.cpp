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

#include "EQZoneMap.h"
#include "EQAreaTriggerHooks.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <zlib.h>

EverQuestAreaTriggerHooks::GameMasterZoneLineHandlerFunction EverQuestAreaTriggerHooks::GameMasterZoneLineHandler = nullptr;

namespace
{
    constexpr uint32 EQ_ZONE_MAP_FILE_MAGIC = 0x504D5145; // "EQMP"
    constexpr uint32 EQ_ZONE_MAP_FILE_VERSION = 1;
    constexpr int32 EQ_ZONE_MAP_CHILD_LEAF = -1;
    constexpr int32 EQ_ZONE_MAP_CHILD_OUTSIDE = -2;
    constexpr uint32 EQ_ZONE_MAP_BVH_LEAF_TRIANGLES = 4;
    constexpr uint32 EQ_ZONE_MAP_BVH_MAX_DEPTH = 40;
    constexpr float EQ_ZONE_MAP_BEST_Z_START_ADJUST = 0.5f; // Yards above the given height a best Z search starts down from
    constexpr float EQ_ZONE_MAP_LIQUID_JOIN_GAP = 0.01f;
    constexpr float EQ_ZONE_MAP_LIQUID_FLOOR_TOLERANCE = 0.1f; // EQ starts dungeon liquid exactly at the floor, so liquid starting this little over a point holds it
    constexpr float EQ_ZONE_MAP_LIQUID_SEARCH_ABOVE = 2000.0f;

    // Orders triangles by their center on one axis
    struct TriangleCenterAxisLess
    {
        TriangleCenterAxisLess(std::vector<EQZoneMap::Vec3> const& centers, uint32 axis) : Centers(centers), Axis(axis) { }

        bool operator()(uint32 a, uint32 b) const
        {
            return GetValue(Centers[a]) < GetValue(Centers[b]);
        }

        float GetValue(EQZoneMap::Vec3 const& center) const
        {
            return Axis == 0 ? center.X : (Axis == 1 ? center.Y : center.Z);
        }

        std::vector<EQZoneMap::Vec3> const& Centers;
        uint32 Axis;
    };

    template<typename T>
    bool ReadValue(std::vector<uint8> const& buffer, size_t& offset, T& value)
    {
        if (offset + sizeof(T) > buffer.size())
            return false;
        std::memcpy(&value, buffer.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }
}

bool EQZoneMap::LoadFromFile(std::string const& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return false;
    std::vector<uint8> fileBytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Header
    size_t offset = 0;
    uint32 magic = 0, version = 0, mapIDCount = 0;
    if (!ReadValue(fileBytes, offset, magic) || !ReadValue(fileBytes, offset, version) || !ReadValue(fileBytes, offset, mapIDCount))
        return false;
    if (magic != EQ_ZONE_MAP_FILE_MAGIC || version != EQ_ZONE_MAP_FILE_VERSION)
    {
        LOG_ERROR("maps", "EQZoneMap: '{}' is not a version {} EQ zone map", filePath, EQ_ZONE_MAP_FILE_VERSION);
        return false;
    }
    _mapIDs.clear();
    for (uint32 i = 0; i < mapIDCount; ++i)
    {
        uint32 mapID = 0;
        if (!ReadValue(fileBytes, offset, mapID))
            return false;
        _mapIDs.push_back(mapID);
    }
    uint32 bodySize = 0, compressedBodySize = 0;
    if (!ReadValue(fileBytes, offset, _worldScale) || !ReadValue(fileBytes, offset, bodySize) || !ReadValue(fileBytes, offset, compressedBodySize))
        return false;
    if (offset + compressedBodySize > fileBytes.size())
        return false;

    // Body
    std::vector<uint8> body(bodySize);
    uLongf uncompressedSize = bodySize;
    if (uncompress(body.data(), &uncompressedSize, fileBytes.data() + offset, compressedBodySize) != Z_OK || uncompressedSize != bodySize)
    {
        LOG_ERROR("maps", "EQZoneMap: '{}' could not be decompressed", filePath);
        return false;
    }
    offset = 0;
    uint32 vertexCount = 0;
    if (!ReadValue(body, offset, vertexCount))
        return false;
    _vertices.resize(vertexCount);
    for (uint32 i = 0; i < vertexCount; ++i)
        if (!ReadValue(body, offset, _vertices[i].X) || !ReadValue(body, offset, _vertices[i].Y) || !ReadValue(body, offset, _vertices[i].Z))
            return false;
    uint32 triangleCount = 0;
    if (!ReadValue(body, offset, triangleCount))
        return false;
    _triangleIndices.resize(size_t(triangleCount) * 3);
    for (size_t i = 0; i < _triangleIndices.size(); ++i)
    {
        if (!ReadValue(body, offset, _triangleIndices[i]))
            return false;
        if (_triangleIndices[i] >= vertexCount)
            return false;
    }
    uint32 regionNodeCount = 0;
    if (!ReadValue(body, offset, regionNodeCount))
        return false;
    _regionNodes.resize(regionNodeCount);
    for (uint32 i = 0; i < regionNodeCount; ++i)
    {
        RegionNode& node = _regionNodes[i];
        if (!ReadValue(body, offset, node.Normal[0]) || !ReadValue(body, offset, node.Normal[1]) || !ReadValue(body, offset, node.Normal[2])
            || !ReadValue(body, offset, node.Distance) || !ReadValue(body, offset, node.Front) || !ReadValue(body, offset, node.Back)
            || !ReadValue(body, offset, node.RegionFlags) || !ReadValue(body, offset, node.LiquidTypeID))
            return false;
        if (node.Front >= int32(regionNodeCount) || node.Back >= int32(regionNodeCount))
            return false;
    }

    BuildBVH();
    return true;
}

void EQZoneMap::BuildBVH()
{
    uint32 triangleCount = uint32(_triangleIndices.size() / 3);
    _bvhTriangles.resize(triangleCount);
    std::vector<Vec3> centers(triangleCount);
    for (uint32 i = 0; i < triangleCount; ++i)
    {
        _bvhTriangles[i] = i;
        Vec3 const& a = _vertices[_triangleIndices[i * 3]];
        Vec3 const& b = _vertices[_triangleIndices[i * 3 + 1]];
        Vec3 const& c = _vertices[_triangleIndices[i * 3 + 2]];
        centers[i] = { (a.X + b.X + c.X) / 3.0f, (a.Y + b.Y + c.Y) / 3.0f, (a.Z + b.Z + c.Z) / 3.0f };
    }
    _bvhNodes.clear();
    _bvhNodes.reserve(triangleCount / 2 + 1);
    if (triangleCount > 0)
        BuildBVHNode(0, triangleCount, centers, 0);
}

int32 EQZoneMap::BuildBVHNode(uint32 firstTriangle, uint32 triangleCount, std::vector<Vec3> const& centers, uint32 depth)
{
    int32 nodeIndex = int32(_bvhNodes.size());
    _bvhNodes.emplace_back();
    BVHNode node;
    node.Min[0] = node.Min[1] = node.Min[2] = FLT_MAX;
    node.Max[0] = node.Max[1] = node.Max[2] = -FLT_MAX;
    float centerMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float centerMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint32 i = firstTriangle; i < firstTriangle + triangleCount; ++i)
    {
        uint32 triangle = _bvhTriangles[i];
        for (uint32 corner = 0; corner < 3; ++corner)
        {
            Vec3 const& vertex = _vertices[_triangleIndices[triangle * 3 + corner]];
            float values[3] = { vertex.X, vertex.Y, vertex.Z };
            for (uint32 axis = 0; axis < 3; ++axis)
            {
                node.Min[axis] = std::min(node.Min[axis], values[axis]);
                node.Max[axis] = std::max(node.Max[axis], values[axis]);
            }
        }
        float centerValues[3] = { centers[triangle].X, centers[triangle].Y, centers[triangle].Z };
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            centerMin[axis] = std::min(centerMin[axis], centerValues[axis]);
            centerMax[axis] = std::max(centerMax[axis], centerValues[axis]);
        }
    }

    if (triangleCount <= EQ_ZONE_MAP_BVH_LEAF_TRIANGLES || depth >= EQ_ZONE_MAP_BVH_MAX_DEPTH)
    {
        node.Left = -1;
        node.Right = -1;
        node.FirstTriangle = firstTriangle;
        node.TriangleCount = triangleCount;
        _bvhNodes[nodeIndex] = node;
        return nodeIndex;
    }

    // Split on the middle of the longest axis of the triangle centers
    uint32 splitAxis = 0;
    float longest = centerMax[0] - centerMin[0];
    for (uint32 axis = 1; axis < 3; ++axis)
    {
        if (centerMax[axis] - centerMin[axis] > longest)
        {
            longest = centerMax[axis] - centerMin[axis];
            splitAxis = axis;
        }
    }
    uint32 middle = firstTriangle + triangleCount / 2;
    std::nth_element(_bvhTriangles.begin() + firstTriangle, _bvhTriangles.begin() + middle, _bvhTriangles.begin() + firstTriangle + triangleCount,
        TriangleCenterAxisLess(centers, splitAxis));

    node.FirstTriangle = 0;
    node.TriangleCount = 0;
    node.Left = BuildBVHNode(firstTriangle, middle - firstTriangle, centers, depth + 1);
    node.Right = BuildBVHNode(middle, firstTriangle + triangleCount - middle, centers, depth + 1);
    _bvhNodes[nodeIndex] = node;
    return nodeIndex;
}

bool EQZoneMap::RayIntersectsBox(BVHNode const& node, Vec3 const& origin, Vec3 const& inverseDirection, float maxDistance) const
{
    float originValues[3] = { origin.X, origin.Y, origin.Z };
    float inverseValues[3] = { inverseDirection.X, inverseDirection.Y, inverseDirection.Z };
    float tMin = 0.0f;
    float tMax = maxDistance;
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        float t1 = (node.Min[axis] - originValues[axis]) * inverseValues[axis];
        float t2 = (node.Max[axis] - originValues[axis]) * inverseValues[axis];
        if (t1 > t2)
            std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax)
            return false;
    }
    return true;
}

// Both sides of a triangle count, as they did for the EQ server's ray casts
bool EQZoneMap::RayIntersectsTriangle(uint32 triangleIndex, Vec3 const& origin, Vec3 const& direction, float& distance) const
{
    Vec3 const& a = _vertices[_triangleIndices[triangleIndex * 3]];
    Vec3 const& b = _vertices[_triangleIndices[triangleIndex * 3 + 1]];
    Vec3 const& c = _vertices[_triangleIndices[triangleIndex * 3 + 2]];
    float edge1[3] = { b.X - a.X, b.Y - a.Y, b.Z - a.Z };
    float edge2[3] = { c.X - a.X, c.Y - a.Y, c.Z - a.Z };
    float p[3] = { direction.Y * edge2[2] - direction.Z * edge2[1], direction.Z * edge2[0] - direction.X * edge2[2], direction.X * edge2[1] - direction.Y * edge2[0] };
    float determinant = edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
    if (std::fabs(determinant) < 1e-9f)
        return false;
    float inverseDeterminant = 1.0f / determinant;
    float t[3] = { origin.X - a.X, origin.Y - a.Y, origin.Z - a.Z };
    float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f)
        return false;
    float q[3] = { t[1] * edge1[2] - t[2] * edge1[1], t[2] * edge1[0] - t[0] * edge1[2], t[0] * edge1[1] - t[1] * edge1[0] };
    float v = (direction.X * q[0] + direction.Y * q[1] + direction.Z * q[2]) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    distance = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * inverseDeterminant;
    return distance >= 0.0f;
}

// Nearest hit along a ray with a unit length direction
bool EQZoneMap::Raycast(Vec3 const& origin, Vec3 const& direction, float maxDistance, float& hitDistance) const
{
    if (_bvhNodes.empty())
        return false;
    Vec3 inverseDirection = { direction.X != 0.0f ? 1.0f / direction.X : FLT_MAX, direction.Y != 0.0f ? 1.0f / direction.Y : FLT_MAX,
        direction.Z != 0.0f ? 1.0f / direction.Z : FLT_MAX };
    bool hit = false;
    hitDistance = maxDistance;
    int32 stack[128];
    int32 stackSize = 0;
    stack[stackSize++] = 0;
    while (stackSize > 0)
    {
        BVHNode const& node = _bvhNodes[stack[--stackSize]];
        if (!RayIntersectsBox(node, origin, inverseDirection, hitDistance))
            continue;
        if (node.Left < 0)
        {
            for (uint32 i = node.FirstTriangle; i < node.FirstTriangle + node.TriangleCount; ++i)
            {
                float distance = 0.0f;
                if (RayIntersectsTriangle(_bvhTriangles[i], origin, direction, distance) && distance <= hitDistance)
                {
                    hitDistance = distance;
                    hit = true;
                }
            }
            continue;
        }
        if (stackSize + 2 > 128)
            continue;
        stack[stackSize++] = node.Left;
        stack[stackSize++] = node.Right;
    }
    return hit;
}

float EQZoneMap::GetFloorZ(float x, float y, float z, float maxDistance) const
{
    float hitDistance = 0.0f;
    if (Raycast({ x, y, z }, { 0.0f, 0.0f, -1.0f }, maxDistance, hitDistance))
        return z - hitDistance;
    return EQ_ZONE_MAP_INVALID_HEIGHT;
}

float EQZoneMap::GetCeilingZ(float x, float y, float z, float maxDistance) const
{
    float hitDistance = 0.0f;
    if (Raycast({ x, y, z }, { 0.0f, 0.0f, 1.0f }, maxDistance, hitDistance))
        return z + hitDistance;
    return EQ_ZONE_MAP_INVALID_HEIGHT;
}

float EQZoneMap::FindBestZ(float x, float y, float z, float maxDistanceUp, float maxDistanceDown) const
{
    float floorZ = GetFloorZ(x, y, z + EQ_ZONE_MAP_BEST_Z_START_ADJUST, maxDistanceDown + EQ_ZONE_MAP_BEST_Z_START_ADJUST);
    if (floorZ > EQ_ZONE_MAP_INVALID_HEIGHT)
        return floorZ;
    return GetCeilingZ(x, y, z, maxDistanceUp);
}

bool EQZoneMap::IsInLineOfSight(float x1, float y1, float z1, float x2, float y2, float z2) const
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float dz = z2 - z1;
    float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 0.001f)
        return true;
    float hitDistance = 0.0f;
    return !Raycast({ x1, y1, z1 }, { dx / length, dy / length, dz / length }, length, hitDistance);
}

uint32 EQZoneMap::GetRegionFlags(float x, float y, float z) const
{
    int32 nodeIndex = _regionNodes.empty() ? EQ_ZONE_MAP_CHILD_OUTSIDE : 0;
    while (nodeIndex >= 0)
    {
        RegionNode const& node = _regionNodes[nodeIndex];
        if (node.Front == EQ_ZONE_MAP_CHILD_LEAF)
            return node.RegionFlags;
        float side = node.Normal[0] * x + node.Normal[1] * y + node.Normal[2] * z + node.Distance;
        nodeIndex = side >= 0.0f ? node.Front : node.Back;
    }
    return EQ_REGION_NONE;
}

// Every region a vertical line passes through between two heights (each split plane cuts the line once at most)
void EQZoneMap::GetVerticalRegionIntervals(int32 nodeIndex, float x, float y, float bottomZ, float topZ,
    std::vector<std::pair<std::pair<float, float>, int32>>& intervals) const
{
    if (nodeIndex < 0 || topZ <= bottomZ)
        return;
    RegionNode const& node = _regionNodes[nodeIndex];
    if (node.Front == EQ_ZONE_MAP_CHILD_LEAF)
    {
        intervals.push_back({ { bottomZ, topZ }, nodeIndex });
        return;
    }
    float rest = node.Normal[0] * x + node.Normal[1] * y + node.Distance;
    float sideBottom = node.Normal[2] * bottomZ + rest;
    float sideTop = node.Normal[2] * topZ + rest;
    if (sideBottom >= 0.0f && sideTop >= 0.0f)
        GetVerticalRegionIntervals(node.Front, x, y, bottomZ, topZ, intervals);
    else if (sideBottom < 0.0f && sideTop < 0.0f)
        GetVerticalRegionIntervals(node.Back, x, y, bottomZ, topZ, intervals);
    else
    {
        float crossZ = -rest / node.Normal[2];
        if (sideBottom >= 0.0f)
        {
            GetVerticalRegionIntervals(node.Front, x, y, bottomZ, crossZ, intervals);
            GetVerticalRegionIntervals(node.Back, x, y, crossZ, topZ, intervals);
        }
        else
        {
            GetVerticalRegionIntervals(node.Back, x, y, bottomZ, crossZ, intervals);
            GetVerticalRegionIntervals(node.Front, x, y, crossZ, topZ, intervals);
        }
    }
}

bool EQZoneMap::GetLiquidInfo(float x, float y, float z, float maxDistanceBelow, EQZoneLiquidInfo& liquidInfo) const
{
    if (_regionNodes.empty())
        return false;
    std::vector<std::pair<std::pair<float, float>, int32>> intervals;
    GetVerticalRegionIntervals(0, x, y, z - maxDistanceBelow, z + EQ_ZONE_MAP_LIQUID_SEARCH_ABOVE, intervals);
    std::sort(intervals.begin(), intervals.end());

    // Touching liquid intervals are one body of liquid, and the one holding the point (or the highest one under it) is the answer
    bool found = false;
    EQZoneLiquidInfo current;
    bool inRun = false;
    for (auto const& interval : intervals)
    {
        RegionNode const& leaf = _regionNodes[interval.second];
        if ((leaf.RegionFlags & EQ_REGION_ANY_LIQUID) == 0)
        {
            if (inRun && current.BottomZ <= z + EQ_ZONE_MAP_LIQUID_FLOOR_TOLERANCE)
            {
                liquidInfo = current;
                found = true;
            }
            inRun = false;
            continue;
        }
        if (inRun && interval.first.first - current.SurfaceZ <= EQ_ZONE_MAP_LIQUID_JOIN_GAP)
        {
            current.SurfaceZ = interval.first.second;
            if (leaf.LiquidTypeID != 0)
                current.LiquidTypeID = leaf.LiquidTypeID;
            current.RegionFlags |= leaf.RegionFlags;
        }
        else
        {
            if (inRun && current.BottomZ <= z + EQ_ZONE_MAP_LIQUID_FLOOR_TOLERANCE)
            {
                liquidInfo = current;
                found = true;
            }
            current.BottomZ = interval.first.first;
            current.SurfaceZ = interval.first.second;
            current.RegionFlags = leaf.RegionFlags;
            current.LiquidTypeID = leaf.LiquidTypeID;
            inRun = true;
        }
        if (current.BottomZ > z + EQ_ZONE_MAP_LIQUID_FLOOR_TOLERANCE)
            break;
    }
    if (inRun && current.BottomZ <= z + EQ_ZONE_MAP_LIQUID_FLOOR_TOLERANCE)
    {
        liquidInfo = current;
        found = true;
    }
    return found;
}

EQZoneMapMgr* EQZoneMapMgr::instance()
{
    static EQZoneMapMgr instance;
    return &instance;
}

void EQZoneMapMgr::Initialize(std::string const& dataPath)
{
    _filePathsByMapID.clear();
    std::filesystem::path folder = std::filesystem::path(dataPath) / "eqmaps";
    std::error_code errorCode;
    if (!std::filesystem::is_directory(folder, errorCode))
    {
        LOG_INFO("server.loading", "No EverQuest zone maps found (no {} folder)", folder.string());
        return;
    }
    for (auto const& entry : std::filesystem::directory_iterator(folder, errorCode))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".eqmap")
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        uint32 header[3] = { 0, 0, 0 };
        if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != EQ_ZONE_MAP_FILE_MAGIC || header[1] != EQ_ZONE_MAP_FILE_VERSION)
        {
            LOG_ERROR("server.loading", "EQ zone map '{}' has a bad header, skipped", entry.path().string());
            continue;
        }
        for (uint32 i = 0; i < header[2]; ++i)
        {
            uint32 mapID = 0;
            if (!file.read(reinterpret_cast<char*>(&mapID), sizeof(mapID)))
                break;
            _filePathsByMapID[mapID] = entry.path().string();
        }
    }
    LOG_INFO("server.loading", ">> Found EverQuest zone maps for {} maps", _filePathsByMapID.size());
}

EQZoneMap const* EQZoneMapMgr::GetZoneMap(uint32 mapID)
{
    auto filePathIter = _filePathsByMapID.find(mapID);
    if (filePathIter == _filePathsByMapID.end())
        return nullptr;

    std::lock_guard<std::mutex> guard(_loadLock);
    auto zoneMapIter = _zoneMapsByFilePath.find(filePathIter->second);
    if (zoneMapIter != _zoneMapsByFilePath.end())
        return zoneMapIter->second.get();

    std::shared_ptr<EQZoneMap> zoneMap = std::make_shared<EQZoneMap>();
    if (!zoneMap->LoadFromFile(filePathIter->second))
    {
        LOG_ERROR("maps", "EQ zone map '{}' for map {} could not be loaded", filePathIter->second, mapID);
        zoneMap = nullptr;
    }
    _zoneMapsByFilePath[filePathIter->second] = zoneMap;
    return zoneMap ? zoneMap.get() : nullptr;
}
