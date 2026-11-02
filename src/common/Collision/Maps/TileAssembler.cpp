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

#include "TileAssembler.h"
#include "BoundingIntervalHierarchy.h"
#include "MapDefines.h"
#include "MapTree.h"
#include "VMapDefinitions.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <boost/filesystem.hpp>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using G3D::Vector3;
using G3D::AABox;
using G3D::inf;
using std::pair;

template<> struct BoundsTrait<VMAP::ModelSpawn*>
{
    static void GetBounds(VMAP::ModelSpawn const* const& obj, G3D::AABox& out) { out = obj->GetBounds(); }
};

namespace VMAP
{
    bool readChunk(FILE* rf, char* dest, char const* compare, uint32 len)
    {
        if (fread(dest, sizeof(char), len, rf) != len) { return false; }
        return memcmp(dest, compare, len) == 0;
    }

    Vector3 ModelPosition::transform(Vector3 const& pIn) const
    {
        Vector3 out = pIn * iScale;
        out = iRotation * out;
        return (out);
    }

    //=================================================================

    TileAssembler::TileAssembler(std::string const& pSrcDirName, std::string const& pDestDirName)
        : iDestDir(pDestDirName), iSrcDir(pSrcDirName)
    {
        boost::filesystem::create_directory(iDestDir);
        //init();
    }

    TileAssembler::~TileAssembler()
    {
        //delete iCoordModelMapping;
    }

    // MergedWorlds: how far past a tile's edge a spawn still counts as reaching it, in yards.  It only has to cover what mmaps_generator
    // reads past a tile's edge (its border, walkableRadius + 3 cells, under 3 yards), and a spare spawn in a tile costs next to nothing
    static constexpr float TILE_SPAWN_MARGIN = 16.0f;
    static constexpr float TILE_SIZE = 533.33333f;

    // In vmap space a tile's first ID (its file's first number) runs along y, and its second along x
    static bool DoesSpawnReachTile(ModelSpawn const& spawn, uint32 tileID)
    {
        if (!(spawn.flags & MOD_HAS_BOUND))
        {
            return true;
        }
        uint32 tileX, tileY;
        StaticMapTree::unpackTileID(tileID, tileX, tileY);
        Vector3 const& low = spawn.iBound.low();
        Vector3 const& high = spawn.iBound.high();
        return high.y >= tileX * TILE_SIZE - TILE_SPAWN_MARGIN && low.y <= (tileX + 1) * TILE_SIZE + TILE_SPAWN_MARGIN
            && high.x >= tileY * TILE_SIZE - TILE_SPAWN_MARGIN && low.x <= (tileY + 1) * TILE_SIZE + TILE_SPAWN_MARGIN;
    }

    // MergedWorlds: what's done model by model and map by map runs on this many threads
    static uint32 GetWorkerCount(std::size_t workCount)
    {
        return uint32(std::min<std::size_t>(workCount, std::clamp(std::thread::hardware_concurrency(), 1u, 16u)));
    }

    // Runs work(index) for each index on the threads, until one returns false.  The return is if none did
    template <typename Work>
    static bool RunOnWorkers(std::size_t workCount, Work work)
    {
        std::atomic<std::size_t> nextIndex{0};
        std::atomic<bool> isSuccess{true};
        auto worker = [&]()
        {
            while (isSuccess)
            {
                std::size_t index = nextIndex++;
                if (index >= workCount)
                {
                    return;
                }
                if (!work(index))
                {
                    isSuccess = false;
                }
            }
        };
        std::vector<std::thread> threads;
        uint32 workerCount = GetWorkerCount(workCount);
        for (uint32 i = 1; i < workerCount; ++i)
        {
            threads.emplace_back(worker);
        }
        worker();
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        return isSuccess;
    }

    struct VertexPositionHash
    {
        std::size_t operator()(std::array<uint32, 3> const& position) const
        {
            uint64 hash = position[0];
            hash = hash * 0x9E3779B97F4A7C15ull ^ position[1];
            hash = hash * 0x9E3779B97F4A7C15ull ^ position[2];
            return std::size_t(hash ^ (hash >> 29));
        }
    };

    // MergedWorlds: raw models are triangle soup, so vertices with exactly the same position share one (the collision is unchanged)
    static void WeldVertices(std::vector<Vector3>& vertices, std::vector<MeshTriangle>& triangles)
    {
        std::unordered_map<std::array<uint32, 3>, uint32, VertexPositionHash> indexByPosition;
        indexByPosition.reserve(vertices.size());
        std::vector<Vector3> weldedVertices;
        std::vector<uint32> newIndices(vertices.size());
        weldedVertices.reserve(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            std::array<uint32, 3> position;
            std::memcpy(position.data(), &vertices[i], sizeof(uint32) * 3);
            auto inserted = indexByPosition.try_emplace(position, uint32(weldedVertices.size()));
            if (inserted.second)
            {
                weldedVertices.push_back(vertices[i]);
            }
            newIndices[i] = inserted.first->second;
        }
        for (MeshTriangle& triangle : triangles)
        {
            triangle.idx0 = newIndices[triangle.idx0];
            triangle.idx1 = newIndices[triangle.idx1];
            triangle.idx2 = newIndices[triangle.idx2];
        }
        vertices.swap(weldedVertices);
    }

    bool TileAssembler::convertWorld2()
    {
        bool success = readMapSpawns();
        if (!success)
        {
            return false;
        }

        // export Map data.  MergedWorlds: a map only touches its own spawns and files, so they go side by side
        std::vector<std::pair<uint32, MapSpawns*>> maps(mapData.begin(), mapData.end());
        std::mutex modelFilesLock;
        success = RunOnWorkers(maps.size(), [&](std::size_t mapIndex)
        {
            std::set<std::string> modelFiles;
            bool isConverted = convertMap(maps[mapIndex].first, *maps[mapIndex].second, modelFiles);
            std::lock_guard<std::mutex> guard(modelFilesLock);
            spawnedModelFiles.insert(modelFiles.begin(), modelFiles.end());
            return isConverted;
        });

        // add an object models, listed in temp_gameobject_models file
        exportGameobjectModels();
        // export objects
        std::cout << "\nConverting Model Files" << std::endl;
        std::vector<std::string> modelFiles(spawnedModelFiles.begin(), spawnedModelFiles.end());
        bool isEveryModelConverted = RunOnWorkers(modelFiles.size(), [&](std::size_t modelIndex)
        {
            if (!convertRawFile(modelFiles[modelIndex]))
            {
                std::cout << "error converting " << modelFiles[modelIndex] << std::endl;
                return false;
            }
            return true;
        });
        if (!isEveryModelConverted)
        {
            success = false;
        }

        //cleanup:
        for (MapData::iterator map_iter = mapData.begin(); map_iter != mapData.end(); ++map_iter)
        {
            delete map_iter->second;
        }
        return success;
    }

    // One map's tree and tiles
    bool TileAssembler::convertMap(uint32 mapID, MapSpawns& mapSpawnData, std::set<std::string>& modelFiles)
    {
        bool success = true;
        {
            // Spawns of a tile stay in the order they were read
            std::stable_sort(mapSpawnData.TileEntries.begin(), mapSpawnData.TileEntries.end(), [](std::pair<uint32, uint32> const& left, std::pair<uint32, uint32> const& right)
            {
                return left.first < right.first;
            });
            auto isTileBelow = [](std::pair<uint32, uint32> const& left, std::pair<uint32, uint32> const& right)
            {
                return left.first < right.first;
            };

            // build global map tree
            std::vector<ModelSpawn*> mapSpawns;
            UniqueEntryMap::iterator entry;
            printf("Calculating model bounds for map %u...\n", mapID);
            for (entry = mapSpawnData.UniqueEntries.begin(); entry != mapSpawnData.UniqueEntries.end(); ++entry)
            {
                // M2 models don't have a bound set in WDT/ADT placement data, i still think they're not used for LoS at all on retail
                if (entry->second.flags & MOD_M2)
                {
                    if (!calculateTransformedBound(entry->second))
                    {
                        break;
                    }
                }
                else if (entry->second.flags & MOD_WORLDSPAWN) // WMO maps and terrain maps use different origin, so we need to adapt :/
                {
                    /// @todo remove extractor hack and uncomment below line:
                    //entry->second.iPos += Vector3(533.33333f*32, 533.33333f*32, 0.f);
                    entry->second.iBound = entry->second.iBound + Vector3(533.33333f * 32, 533.33333f * 32, 0.f);
                }
                mapSpawns.push_back(&(entry->second));
                modelFiles.insert(entry->second.name);
            }

            printf("Creating map tree for map %u...\n", mapID);
            BIH pTree;

            try
            {
                pTree.build(mapSpawns, BoundsTrait<ModelSpawn*>::GetBounds);
            }
            catch (std::exception& e)
            {
                printf("Exception ""%s"" when calling pTree.build", e.what());
                return false;
            }

            // ===> possibly move this code to StaticMapTree class
            std::unordered_map<uint32, uint32> modelNodeIdx;
            std::unordered_map<uint32, ModelSpawn const*> spawnsByID;
            modelNodeIdx.reserve(mapSpawns.size());
            spawnsByID.reserve(mapSpawnData.UniqueEntries.size());
            for (uint32 i = 0; i < mapSpawns.size(); ++i)
            {
                modelNodeIdx.insert(pair<uint32, uint32>(mapSpawns[i]->ID, i));
            }
            for (UniqueEntryMap::const_iterator uniqueEntry = mapSpawnData.UniqueEntries.begin(); uniqueEntry != mapSpawnData.UniqueEntries.end(); ++uniqueEntry)
            {
                spawnsByID[uniqueEntry->first] = &uniqueEntry->second;
            }

            // write map tree file
            std::stringstream mapfilename;
            mapfilename << iDestDir << '/' << std::setfill('0') << std::setw(3) << mapID << ".vmtree";
            FILE* mapfile = fopen(mapfilename.str().c_str(), "wb");
            if (!mapfile)
            {
                printf("Cannot open %s\n", mapfilename.str().c_str());
                return false;
            }

            //general info
            if (success && fwrite(VMAP_MAGIC, 1, 8, mapfile) != 8) { success = false; }
            uint32 globalTileID = StaticMapTree::packTileID(65, 65);
            TileMap& tileEntries = mapSpawnData.TileEntries;
            pair<TileMap::iterator, TileMap::iterator> globalRange = std::equal_range(tileEntries.begin(), tileEntries.end(), std::make_pair(globalTileID, uint32(0)), isTileBelow);
            char isTiled = globalRange.first == globalRange.second; // only maps without terrain (tiles) have global WMO
            if (success && fwrite(&isTiled, sizeof(char), 1, mapfile) != 1) { success = false; }
            // Nodes
            if (success && fwrite("NODE", 4, 1, mapfile) != 1) { success = false; }
            if (success) { success = pTree.writeToFile(mapfile); }
            // global map spawns (WDT), if any (most instances)
            if (success && fwrite("GOBJ", 4, 1, mapfile) != 1) { success = false; }

            for (TileMap::iterator glob = globalRange.first; glob != globalRange.second && success; ++glob)
            {
                success = ModelSpawn::writeToFile(mapfile, *spawnsByID[glob->second]);
            }

            fclose(mapfile);

            // <====

            // write map tile files, similar to ADT files, only with extra BSP tree node info
            // MergedWorlds: a WMO's doodads are listed on every tile the WMO touches, so a map that's one big WMO (each City of Heroes, EverQuest
            // and Ragnarok Online zone) lists all of its doodads on all of its tiles.  A tile keeps only the spawns that reach it.  A spawn
            // that reaches none of its tiles (a doodad just past the edge of the only tile listing it) stays on them all
            std::unordered_set<uint32> spawnsReachingListedTile;
            for (TileMap::iterator tileEntry = tileEntries.begin(); tileEntry != tileEntries.end(); ++tileEntry)
            {
                if (DoesSpawnReachTile(*spawnsByID[tileEntry->second], tileEntry->first))
                {
                    spawnsReachingListedTile.insert(tileEntry->second);
                }
            }

            TileMap::iterator tile;
            for (tile = tileEntries.begin(); tile != tileEntries.end(); tile = std::upper_bound(tile, tileEntries.end(), *tile, isTileBelow))
            {
                ModelSpawn const& spawn = *spawnsByID[tile->second];
                if (spawn.flags & MOD_WORLDSPAWN) // WDT spawn, saved as tile 65/65 currently...
                {
                    continue;
                }
                std::vector<uint32> tileSpawnIDs;
                pair<TileMap::iterator, TileMap::iterator> tileRange = std::equal_range(tile, tileEntries.end(), *tile, isTileBelow);
                for (TileMap::iterator tileEntry = tileRange.first; tileEntry != tileRange.second; ++tileEntry)
                {
                    if (DoesSpawnReachTile(*spawnsByID[tileEntry->second], tile->first)
                        || spawnsReachingListedTile.find(tileEntry->second) == spawnsReachingListedTile.end())
                    {
                        tileSpawnIDs.push_back(tileEntry->second);
                    }
                }
                uint32 nSpawns = tileSpawnIDs.size();
                std::stringstream tilefilename;
                tilefilename.fill('0');
                tilefilename << iDestDir << '/' << std::setw(3) << mapID << '_';
                uint32 x, y;
                StaticMapTree::unpackTileID(tile->first, x, y);
                tilefilename << std::setw(2) << x << '_' << std::setw(2) << y << ".vmtile";
                if (FILE* tilefile = fopen(tilefilename.str().c_str(), "wb"))
                {
                    // file header
                    if (success && fwrite(VMAP_MAGIC, 1, 8, tilefile) != 8) { success = false; }
                    // write number of tile spawns
                    if (success && fwrite(&nSpawns, sizeof(uint32), 1, tilefile) != 1) { success = false; }
                    // write tile spawns
                    for (uint32 s = 0; s < nSpawns; ++s)
                    {
                        ModelSpawn const& spawn2 = *spawnsByID[tileSpawnIDs[s]];
                        success = success && ModelSpawn::writeToFile(tilefile, spawn2);
                        // MapTree nodes to update when loading tile:
                        std::unordered_map<uint32, uint32>::iterator nIdx = modelNodeIdx.find(spawn2.ID);
                        if (success && fwrite(&nIdx->second, sizeof(uint32), 1, tilefile) != 1) { success = false; }
                    }
                    fclose(tilefile);
                }
            }
            // break; //test, extract only first map; TODO: remvoe this line
        }
        return success;
    }

    bool TileAssembler::readMapSpawns()
    {
        std::string fname = iSrcDir + "/dir_bin";
        FILE* dirf = fopen(fname.c_str(), "rb");
        if (!dirf)
        {
            printf("Could not read dir_bin file!\n");
            return false;
        }
        printf("Read coordinate mapping...\n");

        // MergedWorlds: a map that's one big WMO lists its doodads on all of its tiles, which makes tens of millions of records, nearly all of them repeats.  They're read
        // from a block of the file at a time, and only a spawn's first record is unpacked: mapID, tileX, tileY, Flags, NameSet, UniqueId, Pos, Rot, Scale, Bound_lo,
        // Bound_hi, name
        constexpr std::size_t READ_BLOCK_SIZE = 1 << 26;
        constexpr std::size_t MAX_NAME_LENGTH = 500;
        constexpr std::size_t FIXED_RECORD_SIZE = sizeof(uint32) * 4 + sizeof(uint16) + sizeof(uint32) + sizeof(float) * 7;
        constexpr std::size_t BOUND_SIZE = sizeof(float) * 6;
        constexpr std::size_t MAX_RECORD_SIZE = FIXED_RECORD_SIZE + BOUND_SIZE + sizeof(uint32) + MAX_NAME_LENGTH;
        std::vector<char> block(READ_BLOCK_SIZE);
        std::size_t blockSize = 0;
        std::size_t position = 0;
        bool isFileRead = false;
        uint32 currentMapID = 0;
        MapSpawns* current = nullptr;
        while (true)
        {
            if (blockSize - position < MAX_RECORD_SIZE && !isFileRead)
            {
                blockSize -= position;
                std::memmove(block.data(), block.data() + position, blockSize);
                position = 0;
                std::size_t readSize = fread(block.data() + blockSize, 1, block.size() - blockSize, dirf);
                blockSize += readSize;
                isFileRead = readSize == 0;
            }
            if (position == blockSize) // EoF...
            {
                break;
            }
            std::size_t available = blockSize - position;
            char const* record = block.data() + position;
            uint32 flags;
            if (available < FIXED_RECORD_SIZE)
            {
                std::cout << "Error reading ModelSpawn!\n";
                break;
            }
            std::memcpy(&flags, record + sizeof(uint32) * 3, sizeof(uint32));
            bool hasBound = (flags & MOD_HAS_BOUND) != 0;
            std::size_t nameOffset = FIXED_RECORD_SIZE + (hasBound ? BOUND_SIZE : 0);
            uint32 nameLength = 0;
            if (available < nameOffset + sizeof(uint32))
            {
                std::cout << "Error reading ModelSpawn!\n";
                break;
            }
            std::memcpy(&nameLength, record + nameOffset, sizeof(uint32));
            if (nameLength > MAX_NAME_LENGTH || available < nameOffset + sizeof(uint32) + nameLength)
            {
                std::cout << "Error reading ModelSpawn!\n";
                break;
            }

            uint32 mapID, tileX, tileY, spawnID;
            std::memcpy(&mapID, record, sizeof(uint32));
            std::memcpy(&tileX, record + sizeof(uint32), sizeof(uint32));
            std::memcpy(&tileY, record + sizeof(uint32) * 2, sizeof(uint32));
            std::memcpy(&spawnID, record + sizeof(uint32) * 4 + sizeof(uint16), sizeof(uint32));
            if (current == nullptr || mapID != currentMapID)
            {
                MapData::iterator map_iter = mapData.find(mapID);
                if (map_iter == mapData.end())
                {
                    printf("spawning Map %d\n", mapID);
                    mapData[mapID] = current = new MapSpawns();
                }
                else
                {
                    current = map_iter->second;
                }
                currentMapID = mapID;
            }
            if (current->ReadSpawnIDs.insert(spawnID).second)
            {
                ModelSpawn spawn;
                char const* field = record + sizeof(uint32) * 3;
                std::memcpy(&spawn.flags, field, sizeof(uint32));
                field += sizeof(uint32);
                std::memcpy(&spawn.adtId, field, sizeof(uint16));
                field += sizeof(uint16);
                std::memcpy(&spawn.ID, field, sizeof(uint32));
                field += sizeof(uint32);
                std::memcpy(&spawn.iPos, field, sizeof(float) * 3);
                field += sizeof(float) * 3;
                std::memcpy(&spawn.iRot, field, sizeof(float) * 3);
                field += sizeof(float) * 3;
                std::memcpy(&spawn.iScale, field, sizeof(float));
                field += sizeof(float);
                if (hasBound)
                {
                    Vector3 boundLow, boundHigh;
                    std::memcpy(&boundLow, field, sizeof(float) * 3);
                    std::memcpy(&boundHigh, field + sizeof(float) * 3, sizeof(float) * 3);
                    spawn.iBound = G3D::AABox(boundLow, boundHigh);
                }
                spawn.name.assign(record + nameOffset + sizeof(uint32), nameLength);
                current->UniqueEntries.emplace(spawn.ID, spawn);
            }
            current->TileEntries.emplace_back(StaticMapTree::packTileID(tileX, tileY), spawnID);
            position += nameOffset + sizeof(uint32) + nameLength;
        }
        bool success = (ferror(dirf) == 0);
        fclose(dirf);
        return success;
    }

    // MergedWorlds: a model's file was read again for each of its spawns, and a run has millions.  Each is read once, by whichever map gets to it first
    std::shared_ptr<ModelVertices const> TileAssembler::getModelVertices(std::string const& modelName)
    {
        {
            std::lock_guard<std::mutex> guard(modelVerticesLock);
            auto modelIter = modelVerticesByName.find(modelName);
            if (modelIter != modelVerticesByName.end())
            {
                return modelIter->second;
            }
        }

        std::string modelFilename(iSrcDir);
        modelFilename.push_back('/');
        modelFilename.append(modelName);
        std::shared_ptr<ModelVertices> vertices;
        WorldModel_Raw raw_model;
        if (raw_model.Read(modelFilename.c_str()))
        {
            vertices = std::make_shared<ModelVertices>();
            for (GroupModel_Raw& group : raw_model.groupsArray)
            {
                vertices->push_back(std::move(group.vertexArray));
            }
            if (vertices->size() != 1)
            {
                printf("Warning: '%s' does not seem to be a M2 model!\n", modelFilename.c_str());
            }
        }
        std::lock_guard<std::mutex> guard(modelVerticesLock);
        return modelVerticesByName.emplace(modelName, vertices).first->second;
    }

    bool TileAssembler::calculateTransformedBound(ModelSpawn& spawn)
    {
        ModelPosition modelPosition;
        modelPosition.iDir = spawn.iRot;
        modelPosition.iScale = spawn.iScale;
        modelPosition.init();

        std::shared_ptr<ModelVertices const> modelVertices = getModelVertices(spawn.name);
        if (!modelVertices)
        {
            return false;
        }

        uint32 groups = modelVertices->size();

        AABox modelBound;
        bool boundEmpty = true;

        for (uint32 g = 0; g < groups; ++g) // should be only one for M2 files...
        {
            std::vector<Vector3> const& vertices = (*modelVertices)[g];

            if (vertices.empty())
            {
                std::cout << "error: model '" << spawn.name << "' has no geometry!" << std::endl;
                continue;
            }

            uint32 nvectors = vertices.size();
            for (uint32 i = 0; i < nvectors; ++i)
            {
                Vector3 v = modelPosition.transform(vertices[i]);

                if (boundEmpty)
                {
                    modelBound = AABox(v, v), boundEmpty = false;
                }
                else
                {
                    modelBound.merge(v);
                }
            }
        }
        spawn.iBound = modelBound + spawn.iPos;
        spawn.flags |= MOD_HAS_BOUND;
        return true;
    }

#pragma pack(push, 1)
    struct WMOLiquidHeader
    {
        int xverts, yverts, xtiles, ytiles;
        float pos_x;
        float pos_y;
        float pos_z;
        short material;
    };
#pragma pack(pop)
    //=================================================================
    bool TileAssembler::convertRawFile(std::string const& pModelFilename)
    {
        bool success = true;
        std::string filename = iSrcDir;
        if (filename.length() > 0)
        {
            filename.push_back('/');
        }
        filename.append(pModelFilename);

        WorldModel_Raw raw_model;
        if (!raw_model.Read(filename.c_str()))
        {
            return false;
        }

        // write WorldModel
        WorldModel model;
        model.setRootWmoID(raw_model.RootWMOID);
        if (!raw_model.groupsArray.empty())
        {
            std::vector<GroupModel> groupsArray;

            uint32 groups = raw_model.groupsArray.size();
            for (uint32 g = 0; g < groups; ++g)
            {
                GroupModel_Raw& raw_group = raw_model.groupsArray[g];
                WeldVertices(raw_group.vertexArray, raw_group.triangles);
                groupsArray.push_back(GroupModel(raw_group.mogpflags, raw_group.GroupWMOID, raw_group.bounds ));
                groupsArray.back().setMeshData(raw_group.vertexArray, raw_group.triangles);
                groupsArray.back().setLiquidData(raw_group.liquid);
            }

            model.setGroupModels(groupsArray);
        }

        success = model.writeFile(iDestDir + "/" + pModelFilename + ".vmo");
        //std::cout << "readRawFile2: '" << pModelFilename << "' tris: " << nElements << " nodes: " << nNodes << std::endl;
        return success;
    }

    void TileAssembler::exportGameobjectModels()
    {
        FILE* model_list = fopen((iSrcDir + "/" + "temp_gameobject_models").c_str(), "rb");
        if (!model_list)
        {
            return;
        }

        char ident[8];
        if (fread(ident, 1, 8, model_list) != 8 || memcmp(ident, VMAP::RAW_VMAP_MAGIC, 8) != 0)
        {
            fclose(model_list);
            return;
        }

        FILE* model_list_copy = fopen((iDestDir + "/" + GAMEOBJECT_MODELS).c_str(), "wb");
        if (!model_list_copy)
        {
            fclose(model_list);
            return;
        }

        fwrite(VMAP::VMAP_MAGIC, 1, 8, model_list_copy);

        uint32 name_length, displayId;
        uint8 isWmo;
        char buff[500];
        while (!feof(model_list))
        {
            if (fread(&displayId, sizeof(uint32), 1, model_list) != 1)
                if (feof(model_list))   // EOF flag is only set after failed reading attempt
                {
                    break;
                }

            if (fread(&isWmo, sizeof(uint8), 1, model_list) != 1
                    || fread(&name_length, sizeof(uint32), 1, model_list) != 1
                    || name_length >= sizeof(buff)
                    || fread(&buff, sizeof(char), name_length, model_list) != name_length)
            {
                std::cout << "\nFile 'temp_gameobject_models' seems to be corrupted" << std::endl;
                break;
            }

            std::string model_name(buff, name_length);

            WorldModel_Raw raw_model;
            if (!raw_model.Read((iSrcDir + "/" + model_name).c_str()))
            {
                continue;
            }

            spawnedModelFiles.insert(model_name);
            AABox bounds;
            bool boundEmpty = true;
            for (uint32 g = 0; g < raw_model.groupsArray.size(); ++g)
            {
                std::vector<Vector3>& vertices = raw_model.groupsArray[g].vertexArray;

                uint32 nvectors = vertices.size();
                for (uint32 i = 0; i < nvectors; ++i)
                {
                    Vector3& v = vertices[i];
                    if (boundEmpty)
                    {
                        bounds = AABox(v, v), boundEmpty = false;
                    }
                    else
                    {
                        bounds.merge(v);
                    }
                }
            }

            fwrite(&displayId, sizeof(uint32), 1, model_list_copy);
            fwrite(&isWmo, sizeof(uint8), 1, model_list_copy);
            fwrite(&name_length, sizeof(uint32), 1, model_list_copy);
            fwrite(&buff, sizeof(char), name_length, model_list_copy);
            fwrite(&bounds.low(), sizeof(Vector3), 1, model_list_copy);
            fwrite(&bounds.high(), sizeof(Vector3), 1, model_list_copy);
        }

        fclose(model_list);
        fclose(model_list_copy);
    }
    // temporary use defines to simplify read/check code (close file and return at fail)
#define READ_OR_RETURN(V, S) if (fread((V), (S), 1, rf) != 1) { \
                                        fclose(rf); printf("readfail, op = %i\n", readOperation); return(false); }
#define READ_OR_RETURN_WITH_DELETE(V, S) if (fread((V), (S), 1, rf) != 1) { \
                                        fclose(rf); printf("readfail, op = %i\n", readOperation); delete[] V; return(false); };
#define CMP_OR_RETURN(V, S)  if (strcmp((V), (S)) != 0)        { \
                                        fclose(rf); printf("cmpfail, %s!=%s\n", V, S);return(false); }

    bool GroupModel_Raw::Read(FILE* rf)
    {
        char blockId[5];
        blockId[4] = 0;
        int blocksize;
        int readOperation = 0;

        READ_OR_RETURN(&mogpflags, sizeof(uint32));
        READ_OR_RETURN(&GroupWMOID, sizeof(uint32));

        Vector3 vec1, vec2;
        READ_OR_RETURN(&vec1, sizeof(Vector3));

        READ_OR_RETURN(&vec2, sizeof(Vector3));
        bounds.set(vec1, vec2);

        READ_OR_RETURN(&liquidflags, sizeof(uint32));

        // will this ever be used? what is it good for anyway??
        uint32 branches;
        READ_OR_RETURN(&blockId, 4);
        CMP_OR_RETURN(blockId, "GRP ");
        READ_OR_RETURN(&blocksize, sizeof(int));
        READ_OR_RETURN(&branches, sizeof(uint32));
        for (uint32 b = 0; b < branches; ++b)
        {
            uint32 indexes;
            // indexes for each branch (not used jet)
            READ_OR_RETURN(&indexes, sizeof(uint32));
        }

        // ---- indexes
        READ_OR_RETURN(&blockId, 4);
        CMP_OR_RETURN(blockId, "INDX");
        READ_OR_RETURN(&blocksize, sizeof(int));
        uint32 nindexes;
        READ_OR_RETURN(&nindexes, sizeof(uint32));
        if (nindexes > 0)
        {
            uint16* indexarray = new uint16[nindexes];
            READ_OR_RETURN_WITH_DELETE(indexarray, nindexes * sizeof(uint16));
            triangles.reserve(nindexes / 3);
            for (uint32 i = 0; i < nindexes; i += 3)
            {
                triangles.push_back(MeshTriangle(indexarray[i], indexarray[i + 1], indexarray[i + 2]));
            }

            delete[] indexarray;
        }

        // ---- vectors
        READ_OR_RETURN(&blockId, 4);
        CMP_OR_RETURN(blockId, "VERT");
        READ_OR_RETURN(&blocksize, sizeof(int));
        uint32 nvectors;
        READ_OR_RETURN(&nvectors, sizeof(uint32));

        if (nvectors > 0)
        {
            float* vectorarray = new float[nvectors * 3];
            READ_OR_RETURN_WITH_DELETE(vectorarray, nvectors * sizeof(float) * 3);
            for (uint32 i = 0; i < nvectors; ++i)
            {
                vertexArray.push_back( Vector3(vectorarray + 3 * i));
            }

            delete[] vectorarray;
        }
        // ----- liquid
        liquid = nullptr;
        if (liquidflags & 3)
        {
            READ_OR_RETURN(&blockId, 4);
            CMP_OR_RETURN(blockId, "LIQU");
            READ_OR_RETURN(&blocksize, sizeof(int));
            uint32 liquidType;
            READ_OR_RETURN(&liquidType, sizeof(uint32));
            if (liquidflags & 1)
            {
                WMOLiquidHeader hlq;
                READ_OR_RETURN(&hlq, sizeof(WMOLiquidHeader));
                liquid = new WmoLiquid(hlq.xtiles, hlq.ytiles, Vector3(hlq.pos_x, hlq.pos_y, hlq.pos_z), liquidType);
                uint32 size = hlq.xverts * hlq.yverts;
                READ_OR_RETURN(liquid->GetHeightStorage(), size * sizeof(float));
                size = hlq.xtiles * hlq.ytiles;
                READ_OR_RETURN(liquid->GetFlagsStorage(), size);
            }
            else
            {
                liquid = new WmoLiquid(0, 0, Vector3::zero(), liquidType);
                liquid->GetHeightStorage()[0] = bounds.high().z;
            }
        }

        return true;
    }

    GroupModel_Raw::~GroupModel_Raw()
    {
        delete liquid;
    }

    bool WorldModel_Raw::Read(char const* path)
    {
        FILE* rf = fopen(path, "rb");
        if (!rf)
        {
            printf("ERROR: Can't open raw model file: %s\n", path);
            return false;
        }

        char ident[9];
        ident[8] = '\0';
        int readOperation = 0;

        READ_OR_RETURN(&ident, 8);
        CMP_OR_RETURN(ident, RAW_VMAP_MAGIC);

        // we have to read one int. This is needed during the export and we have to skip it here
        uint32 tempNVectors;
        READ_OR_RETURN(&tempNVectors, sizeof(tempNVectors));

        uint32 groups;
        READ_OR_RETURN(&groups, sizeof(uint32));
        READ_OR_RETURN(&RootWMOID, sizeof(uint32));

        groupsArray.resize(groups);
        bool succeed = true;
        for (uint32 g = 0; g < groups && succeed; ++g)
        {
            succeed = groupsArray[g].Read(rf);
        }

        if (succeed) /// rf will be freed inside Read if the function had any errors.
        {
            fclose(rf);
        }
        return succeed;
    }

    // drop of temporary use defines
#undef READ_OR_RETURN
#undef CMP_OR_RETURN
}
