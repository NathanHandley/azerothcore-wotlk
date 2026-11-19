/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ComposedMap.h"
#include "GameObjectModel.h"
#include "GridDefines.h"
#include "Log.h"
#include "MapDefines.h"
#include "Timer.h"
#include "WorldModel.h"
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace
{
    // As mmaps-config.yaml's mesh settings (the global ones every MergedWorlds map is built with), in cells
    constexpr int VERTICES_PER_MAP_EDGE = 2000;
    constexpr int VERTICES_PER_TILE_EDGE = 80;
    constexpr int TILES_PER_MAP_EDGE = VERTICES_PER_MAP_EDGE / VERTICES_PER_TILE_EDGE;
    constexpr float WALKABLE_SLOPE_ANGLE = 60.0f;
    // The head room a way needs: 7 cells.  Recast keeps a span only if the gap to the span above is MORE than this (rcFilterWalkableLowHeightSpans: top - bot
    // <= walkableHeight is cut), floor rounded up and ceiling down, so 7 asks 8 cells of gap, 2.13 to 2.67 yd of room: the fewest over a City of Heroes hero's
    // 6 ft (entworldcoll.c tests from 1.5 to 6 ft above the feet for a ceiling).  The zones' 6 (1.87 to 2.4 yd) let a path run under stairs a hero
    // doesn't fit under (Pocket D's bar stairs, 1.6 to 2.1 yd of room); 8 (2.4 to 2.93 yd, round 12) cut rooms a hero walks under
    constexpr int WALKABLE_HEIGHT = 7;
    // What a step up may be: 2 cells, 0.53 yd, the cell count nearest the half yard a hero steps up on a City of Heroes map (CoH's 1.5 ft: the client's
    // GetStepUpHeight).  The zones' 6 (1.6 yd) let a path go over a prop a hero can't step onto: the P_Tutorial tunnel's barricade, its top plank 1.05 yd up
    // at 17 155.5, was on the way out of the mission, and the client stopped at it (the hero has to jump).  3 cells (0.8 yd) still let one over a 0.83 yd
    // crate in the 5th Column warehouse (36.4 -1.9), whose top the rasterizer can put three cells up.  A composed mission's stairs and curbs are under a foot a step
    constexpr int WALKABLE_CLIMB = 2;
    constexpr int WALKABLE_RADIUS = 2;
    constexpr float MAX_SIMPLIFICATION_ERROR = 1.8f;
    constexpr float BASE_UNIT_DIM = SIZE_OF_GRIDS / float(VERTICES_PER_MAP_EDGE);

    struct RecastContextDeleter
    {
        void operator()(rcContext* context) const { delete context; }
    };

    // One map grid's worth of the navmesh, as TileBuilder::buildMoveMapTile builds it.  Triangles are binned to the grid's sub tiles first, since the
    // generator's way (every triangle into every sub tile's rasterizer) is most of its time on a small, dense composition
    bool BuildGridTile(rcContext* context, dtNavMesh* navMesh, std::vector<float> const& verts, std::vector<int> const& tris, float const* bmin, float const* bmax)
    {
        rcConfig config;
        memset(&config, 0, sizeof(rcConfig));
        rcVcopy(config.bmin, bmin);
        rcVcopy(config.bmax, bmax);
        config.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
        config.cs = BASE_UNIT_DIM;
        config.ch = BASE_UNIT_DIM;
        config.walkableSlopeAngle = WALKABLE_SLOPE_ANGLE;
        config.tileSize = VERTICES_PER_TILE_EDGE;
        config.walkableRadius = WALKABLE_RADIUS;
        config.borderSize = WALKABLE_RADIUS + 3;
        config.maxEdgeLen = VERTICES_PER_TILE_EDGE + 1;
        config.walkableHeight = WALKABLE_HEIGHT;
        config.walkableClimb = WALKABLE_CLIMB;
        config.minRegionArea = rcSqr(60);
        config.mergeRegionArea = rcSqr(50);
        config.maxSimplificationError = MAX_SIMPLIFICATION_ERROR;
        config.detailSampleDist = config.cs * 16;
        config.detailSampleMaxError = config.ch * 1;
        rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);

        rcConfig tileCfg = config;
        tileCfg.width = config.tileSize + config.borderSize * 2;
        tileCfg.height = config.tileSize + config.borderSize * 2;
        float const subTileLength = config.tileSize * config.cs;
        float const border = tileCfg.borderSize * tileCfg.cs;

        // Each triangle into the sub tiles its bounds (with their borders) reach
        int const vertCount = int(verts.size() / 3);
        int const triCount = int(tris.size() / 3);
        std::vector<std::vector<int>> trisBySubTile(TILES_PER_MAP_EDGE * TILES_PER_MAP_EDGE);
        for (int t = 0; t < triCount; ++t)
        {
            float minX = FLT_MAX, maxX = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
            for (int corner = 0; corner < 3; ++corner)
            {
                float const* v = &verts[tris[t * 3 + corner] * 3];
                minX = std::min(minX, v[0]);
                maxX = std::max(maxX, v[0]);
                minZ = std::min(minZ, v[2]);
                maxZ = std::max(maxZ, v[2]);
            }
            int const x0 = std::max(0, int(std::floor((minX - border - config.bmin[0]) / subTileLength)));
            int const x1 = std::min(TILES_PER_MAP_EDGE - 1, int(std::floor((maxX + border - config.bmin[0]) / subTileLength)));
            int const z0 = std::max(0, int(std::floor((minZ - border - config.bmin[2]) / subTileLength)));
            int const z1 = std::min(TILES_PER_MAP_EDGE - 1, int(std::floor((maxZ + border - config.bmin[2]) / subTileLength)));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    trisBySubTile[x + z * TILES_PER_MAP_EDGE].push_back(t);
        }

        std::vector<rcPolyMesh*> polyMeshes;
        std::vector<rcPolyMeshDetail*> detailMeshes;
        for (int y = 0; y < TILES_PER_MAP_EDGE; ++y)
        {
            for (int x = 0; x < TILES_PER_MAP_EDGE; ++x)
            {
                std::vector<int> const& subTileTris = trisBySubTile[x + y * TILES_PER_MAP_EDGE];
                if (subTileTris.empty())
                    continue;
                tileCfg.bmin[0] = config.bmin[0] + x * subTileLength - border;
                tileCfg.bmin[2] = config.bmin[2] + y * subTileLength - border;
                tileCfg.bmax[0] = config.bmin[0] + (x + 1) * subTileLength + border;
                tileCfg.bmax[2] = config.bmin[2] + (y + 1) * subTileLength + border;

                std::vector<int> localTris;
                localTris.reserve(subTileTris.size() * 3);
                for (int t : subTileTris)
                {
                    localTris.push_back(tris[t * 3]);
                    localTris.push_back(tris[t * 3 + 1]);
                    localTris.push_back(tris[t * 3 + 2]);
                }
                int const localTriCount = int(subTileTris.size());

                rcHeightfield* solid = rcAllocHeightfield();
                if (!solid || !rcCreateHeightfield(context, *solid, tileCfg.width, tileCfg.height, tileCfg.bmin, tileCfg.bmax, tileCfg.cs, tileCfg.ch))
                {
                    rcFreeHeightField(solid);
                    continue;
                }
                std::vector<unsigned char> triFlags(localTriCount, NAV_GROUND);
                rcClearUnwalkableTriangles(context, tileCfg.walkableSlopeAngle, verts.data(), vertCount, localTris.data(), localTriCount, triFlags.data());
                rcRasterizeTriangles(context, verts.data(), vertCount, localTris.data(), triFlags.data(), localTriCount, *solid, config.walkableClimb);
                rcFilterLowHangingWalkableObstacles(context, config.walkableClimb, *solid);
                rcFilterLedgeSpans(context, tileCfg.walkableHeight, tileCfg.walkableClimb, *solid);
                rcFilterWalkableLowHeightSpans(context, tileCfg.walkableHeight, *solid);

                rcCompactHeightfield* chf = rcAllocCompactHeightfield();
                rcContourSet* cset = nullptr;
                rcPolyMesh* pmesh = nullptr;
                rcPolyMeshDetail* dmesh = nullptr;
                bool built = chf && rcBuildCompactHeightfield(context, tileCfg.walkableHeight, tileCfg.walkableClimb, *solid, *chf)
                    && rcErodeWalkableArea(context, config.walkableRadius, *chf)
                    && rcBuildDistanceField(context, *chf)
                    && rcBuildRegions(context, *chf, tileCfg.borderSize, tileCfg.minRegionArea, tileCfg.mergeRegionArea);
                if (built)
                {
                    cset = rcAllocContourSet();
                    built = cset && rcBuildContours(context, *chf, tileCfg.maxSimplificationError, tileCfg.maxEdgeLen, *cset);
                }
                if (built)
                {
                    pmesh = rcAllocPolyMesh();
                    built = pmesh && rcBuildPolyMesh(context, *cset, tileCfg.maxVertsPerPoly, *pmesh);
                }
                if (built)
                {
                    dmesh = rcAllocPolyMeshDetail();
                    built = dmesh && rcBuildPolyMeshDetail(context, *pmesh, *chf, tileCfg.detailSampleDist, tileCfg.detailSampleMaxError, *dmesh);
                }
                rcFreeHeightField(solid);
                rcFreeCompactHeightfield(chf);
                rcFreeContourSet(cset);
                if (built)
                {
                    polyMeshes.push_back(pmesh);
                    detailMeshes.push_back(dmesh);
                }
                else
                {
                    rcFreePolyMesh(pmesh);
                    rcFreePolyMeshDetail(dmesh);
                }
            }
        }

        bool added = false;
        rcPolyMesh* polyMesh = rcAllocPolyMesh();
        rcPolyMeshDetail* polyMeshDetail = rcAllocPolyMeshDetail();
        if (polyMesh && polyMeshDetail && !polyMeshes.empty())
        {
            rcMergePolyMeshes(context, polyMeshes.data(), int(polyMeshes.size()), *polyMesh);
            rcMergePolyMeshDetails(context, detailMeshes.data(), int(detailMeshes.size()), *polyMeshDetail);
            for (int i = 0; i < polyMesh->npolys; ++i)
                if (polyMesh->areas[i] & RC_WALKABLE_AREA)
                    polyMesh->flags[i] = polyMesh->areas[i];

            dtNavMeshCreateParams params;
            memset(&params, 0, sizeof(params));
            params.verts = polyMesh->verts;
            params.vertCount = polyMesh->nverts;
            params.polys = polyMesh->polys;
            params.polyAreas = polyMesh->areas;
            params.polyFlags = polyMesh->flags;
            params.polyCount = polyMesh->npolys;
            params.nvp = polyMesh->nvp;
            params.detailMeshes = polyMeshDetail->meshes;
            params.detailVerts = polyMeshDetail->verts;
            params.detailVertsCount = polyMeshDetail->nverts;
            params.detailTris = polyMeshDetail->tris;
            params.detailTriCount = polyMeshDetail->ntris;
            params.walkableHeight = BASE_UNIT_DIM * config.walkableHeight;
            params.walkableRadius = BASE_UNIT_DIM * config.walkableRadius;
            params.walkableClimb = BASE_UNIT_DIM * config.walkableClimb;
            params.tileX = int((((bmin[0] + bmax[0]) / 2) - navMesh->getParams()->orig[0]) / SIZE_OF_GRIDS);
            params.tileY = int((((bmin[2] + bmax[2]) / 2) - navMesh->getParams()->orig[2]) / SIZE_OF_GRIDS);
            // The merged mesh's vertices count from its own corner, which is the first sub tile that had anything (empty ones were never built), not
            // necessarily the grid's
            rcVcopy(params.bmin, bmin);
            rcVcopy(params.bmax, bmax);
            params.bmin[0] = polyMesh->bmin[0];
            params.bmin[2] = polyMesh->bmin[2];
            params.cs = config.cs;
            params.ch = config.ch;
            params.tileLayer = 0;
            params.buildBvTree = true;

            unsigned char* navData = nullptr;
            int navDataSize = 0;
            if (params.nvp <= DT_VERTS_PER_POLYGON && params.vertCount > 0 && params.vertCount < 0xffff && params.polyCount > 0
                && dtCreateNavMeshData(&params, &navData, &navDataSize))
            {
                dtTileRef tileRef = 0;
                if (dtStatusSucceed(navMesh->addTile(navData, navDataSize, DT_TILE_FREE_DATA, 0, &tileRef)) && tileRef)
                    added = true;
                else
                    dtFree(navData);
            }
        }
        for (rcPolyMesh* mesh : polyMeshes)
            rcFreePolyMesh(mesh);
        for (rcPolyMeshDetail* mesh : detailMeshes)
            rcFreePolyMeshDetail(mesh);
        rcFreePolyMesh(polyMesh);
        rcFreePolyMeshDetail(polyMeshDetail);
        return added;
    }
}

std::shared_ptr<dtNavMesh> ComposedMap::BuildNavMesh(uint32 mapId, std::vector<ComposedMapPiece> const& pieces, std::vector<GameObjectModel const*> const& models)
{
    uint32 const startTime = getMSTime();

    // Every piece's collision in recast space: (world y, world z, world x), as PathGenerator asks.  An M2's triangles face the other way
    std::vector<float> verts;
    std::vector<int> tris;
    for (size_t i = 0; i < pieces.size() && i < models.size(); ++i)
    {
        GameObjectModel const* model = models[i];
        if (!model || !model->GetWorldModel())
            continue;
        bool const flip = !model->IsMapObject();
        for (VMAP::GroupModel const& group : model->GetWorldModel()->GetGroupModels())
        {
            std::vector<G3D::Vector3> groupVerts;
            std::vector<VMAP::MeshTriangle> groupTris;
            VMAP::WmoLiquid* liquid = nullptr;
            group.GetMeshData(groupVerts, groupTris, liquid);
            int const offset = int(verts.size() / 3);
            for (G3D::Vector3 const& vertex : groupVerts)
            {
                G3D::Vector3 const world = pieces[i].Rotation * vertex + pieces[i].Position;
                verts.push_back(world.y);
                verts.push_back(world.z);
                verts.push_back(world.x);
            }
            for (VMAP::MeshTriangle const& triangle : groupTris)
            {
                tris.push_back(offset + int(flip ? triangle.idx2 : triangle.idx0));
                tris.push_back(offset + int(triangle.idx1));
                tris.push_back(offset + int(flip ? triangle.idx0 : triangle.idx2));
            }
        }
    }
    if (tris.empty())
        return nullptr;

    float allMin[3], allMax[3];
    rcCalcBounds(verts.data(), int(verts.size() / 3), allMin, allMax);

    // The map grids it covers (getTileBounds: recast x is world y, z is world x)
    int const tileXMin = std::max(0, int(std::floor(32.0f - allMax[0] / SIZE_OF_GRIDS)));
    int const tileXMax = std::min(63, int(std::floor(32.0f - allMin[0] / SIZE_OF_GRIDS)));
    int const tileYMin = std::max(0, int(std::floor(32.0f - allMax[2] / SIZE_OF_GRIDS)));
    int const tileYMax = std::min(63, int(std::floor(32.0f - allMin[2] / SIZE_OF_GRIDS)));
    if (tileXMin > tileXMax || tileYMin > tileYMax)
        return nullptr;

    dtNavMeshParams navMeshParams;
    memset(&navMeshParams, 0, sizeof(dtNavMeshParams));
    navMeshParams.tileWidth = SIZE_OF_GRIDS;
    navMeshParams.tileHeight = SIZE_OF_GRIDS;
    navMeshParams.orig[0] = (32 - tileXMax) * SIZE_OF_GRIDS - SIZE_OF_GRIDS;
    navMeshParams.orig[1] = FLT_MIN;
    navMeshParams.orig[2] = (32 - tileYMax) * SIZE_OF_GRIDS - SIZE_OF_GRIDS;
    navMeshParams.maxTiles = (tileXMax - tileXMin + 1) * (tileYMax - tileYMin + 1);
    int const polyBits = DT_POLY_BITS; // As MapBuilder::buildNavMesh
    navMeshParams.maxPolys = 1 << polyBits;
    std::shared_ptr<dtNavMesh> navMesh(dtAllocNavMesh(), [](dtNavMesh* mesh) { dtFreeNavMesh(mesh); });
    if (!navMesh || dtStatusFailed(navMesh->init(&navMeshParams)))
    {
        LOG_ERROR("maps", "ComposedMap: map {} nav mesh could not be made", mapId);
        return nullptr;
    }

    std::unique_ptr<rcContext, RecastContextDeleter> context(new rcContext(false));
    uint32 tileCount = 0;
    for (int tileX = tileXMin; tileX <= tileXMax; ++tileX)
    {
        for (int tileY = tileYMin; tileY <= tileYMax; ++tileY)
        {
            float bmin[3], bmax[3];
            bmax[0] = (32 - tileX) * SIZE_OF_GRIDS;
            bmax[2] = (32 - tileY) * SIZE_OF_GRIDS;
            bmin[0] = bmax[0] - SIZE_OF_GRIDS;
            bmin[2] = bmax[2] - SIZE_OF_GRIDS;
            bmin[1] = allMin[1];
            bmax[1] = allMax[1];
            if (BuildGridTile(context.get(), navMesh.get(), verts, tris, bmin, bmax))
                ++tileCount;
        }
    }
    int polyCount = 0;
    for (int i = 0; i < navMesh->getMaxTiles(); ++i)
    {
        dtMeshTile const* tile = static_cast<dtNavMesh const*>(navMesh.get())->getTile(i);
        if (tile && tile->header)
            polyCount += tile->header->polyCount;
    }
    LOG_INFO("maps", "ComposedMap: map {} nav mesh of {} pieces ({} triangles, recast bounds {:.1f} {:.1f} {:.1f} to {:.1f} {:.1f} {:.1f}) in {} grid tiles, {} polygons, took {} ms",
        mapId, pieces.size(), tris.size() / 3, allMin[0], allMin[1], allMin[2], allMax[0], allMax[1], allMax[2], tileCount, polyCount, GetMSTimeDiffToNow(startTime));
    if (tileCount == 0)
        return nullptr;
    return navMesh;
}
