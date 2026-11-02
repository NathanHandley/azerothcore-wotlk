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

// MergedWorlds (Nathan Handley + Automation): maps composed at runtime.  An instance can be given pieces, each a model by its game object display ID (its
// collision is a game object model, extracted from GameObjectDisplayInfo.dbc), placed and turned any way.  They collide as game objects do, and the instance
// gets a nav mesh built over them, so its creatures path through what was composed.  The client is told the same pieces and draws them

#ifndef _COMPOSEDMAP_H
#define _COMPOSEDMAP_H

#include "Define.h"
#include <G3D/Matrix3.h>
#include <G3D/Vector3.h>
#include <memory>
#include <vector>

class dtNavMesh;
class GameObjectModel;

struct ComposedMapPiece
{
    uint32 DisplayId = 0;
    G3D::Vector3 Position;
    G3D::Matrix3 Rotation = G3D::Matrix3::identity(); // world = Rotation * model + Position
};

namespace ComposedMap
{
    // A nav mesh over the pieces' models (as mmaps_generator builds one over a map's), or none when there's nothing to walk on.  Takes a while for a big
    // composition (a second or so), and touches no map state, so it can run off the map's thread
    std::shared_ptr<dtNavMesh> BuildNavMesh(uint32 mapId, std::vector<ComposedMapPiece> const& pieces, std::vector<GameObjectModel const*> const& models);
}

#endif
