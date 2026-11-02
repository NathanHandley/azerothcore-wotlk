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

#include "model.h"
#include "adtfile.h"
#include "mpq_libmpq04.h"
#include "vmapexport.h"
#include "wmo.h"
#include <G3D/Quat.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

Model::Model(std::string& filename) : filename(filename), header(), vertices(nullptr), indices(nullptr)
{
}

bool Model::open()
{
    MPQFile f(filename.c_str());

    if (f.isEof())
    {
        f.close();
        // Do not show this error on console to avoid confusion, the extractor can continue working even if some models fail to load
        //printf("Error loading model %s\n", filename.c_str());
        return false;
    }

    _unload();

    memcpy(&header, f.getBuffer(), sizeof(ModelHeader));
    if (header.nBoundingTriangles > 0)
    {
        f.seek(0);
        f.seekRelative(header.ofsBoundingVertices);
        vertices = new Vec3D[header.nBoundingVertices];
        f.read(vertices, header.nBoundingVertices * 12);
        for (uint32 i = 0; i < header.nBoundingVertices; i++)
            vertices[i] = fixCoordSystem(vertices[i]);
        f.seek(0);
        f.seekRelative(header.ofsBoundingTriangles);
        indices = new uint16[header.nBoundingTriangles];
        f.read(indices, header.nBoundingTriangles * 2);
        f.close();
    }
    else
    {
        //printf("not included %s\n", filename.c_str());
        f.close();
        return false;
    }
    return true;
}

bool Model::ConvertToVMAPModel(char const* outfilename)
{
    int N[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    FILE* output = fopen(outfilename, "wb");
    if (!output)
    {
        printf("Can't create the output file '%s'\n", outfilename);
        return false;
    }
    fwrite(VMAP::RAW_VMAP_MAGIC, 8, 1, output);
    uint32 nVertices = header.nBoundingVertices;
    fwrite(&nVertices, sizeof(int), 1, output);
    uint32 nofgroups = 1;
    fwrite(&nofgroups, sizeof(uint32), 1, output);
    fwrite(N, 4 * 3, 1, output); // rootwmoid, flags, groupid
    fwrite(N, sizeof(float), 3 * 2, output); //bbox, only needed for WMO currently
    fwrite(N, 4, 1, output); // liquidflags
    fwrite("GRP ", 4, 1, output);
    uint32 branches = 1;
    int wsize;
    wsize = sizeof(branches) + sizeof(uint32) * branches;
    fwrite(&wsize, sizeof(int), 1, output);
    fwrite(&branches, sizeof(branches), 1, output);
    uint32 nIndexes = header.nBoundingTriangles;
    fwrite(&nIndexes, sizeof(uint32), 1, output);
    fwrite("INDX", 4, 1, output);
    wsize = sizeof(uint32) + sizeof(unsigned short) * nIndexes;
    fwrite(&wsize, sizeof(int), 1, output);
    fwrite(&nIndexes, sizeof(uint32), 1, output);
    if (nIndexes > 0)
    {
        for (uint32 i = 0; i < nIndexes; ++i)
        {
            if ((i % 3) - 1 == 0 && i + 1 < nIndexes)
            {
                uint16 tmp = indices[i];
                indices[i] = indices[i + 1];
                indices[i + 1] = tmp;
            }
        }
        fwrite(indices, sizeof(unsigned short), nIndexes, output);
    }
    fwrite("VERT", 4, 1, output);
    wsize = sizeof(int) + sizeof(float) * 3 * nVertices;
    fwrite(&wsize, sizeof(int), 1, output);
    fwrite(&nVertices, sizeof(int), 1, output);
    if (nVertices > 0)
    {
        for (uint32 vpos = 0; vpos < nVertices; ++vpos)
        {
            float tmp = vertices[vpos].y;
            vertices[vpos].y = -vertices[vpos].z;
            vertices[vpos].z = tmp;
        }

        fwrite(vertices, sizeof(float) * 3, nVertices, output);
    }

    fclose(output);

    return true;
}

Vec3D fixCoordSystem(Vec3D const& v)
{
    return Vec3D(v.x, v.z, -v.y);
}

void Doodad::Extract(ADT::MDDF const& doodadDef, char const* ModelInstName, uint32 mapID, uint32 tileX, uint32 tileY, FILE* pDirfile)
{
    if (!IsModelPlaceable(ModelInstName))
        return;

    // scale factor - divide by 1024. blizzard devs must be on crack, why not just use a float?
    float sc = doodadDef.Scale / 1024.0f;

    Vec3D position = fixCoords(doodadDef.Position);

    uint16 nameSet = 0;// not used for models
    uint32 uniqueId = GenerateUniqueObjectId(doodadDef.UniqueId, 0);
    uint32 tcflags = MOD_M2;
    if (tileX == 65 && tileY == 65)
        tcflags |= MOD_WORLDSPAWN;

    //write mapID, tileX, tileY, Flags, NameSet, UniqueId, Pos, Rot, Scale, name
    fwrite(&mapID, sizeof(uint32), 1, pDirfile);
    fwrite(&tileX, sizeof(uint32), 1, pDirfile);
    fwrite(&tileY, sizeof(uint32), 1, pDirfile);
    fwrite(&tcflags, sizeof(uint32), 1, pDirfile);
    fwrite(&nameSet, sizeof(uint16), 1, pDirfile);
    fwrite(&uniqueId, sizeof(uint32), 1, pDirfile);
    fwrite(&position, sizeof(Vec3D), 1, pDirfile);
    fwrite(&doodadDef.Rotation, sizeof(Vec3D), 1, pDirfile);
    fwrite(&sc, sizeof(float), 1, pDirfile);
    uint32 nlen = strlen(ModelInstName);
    fwrite(&nlen, sizeof(uint32), 1, pDirfile);
    fwrite(ModelInstName, sizeof(char), nlen, pDirfile);
}

// MergedWorlds: one placement of a WMO comes up on every tile it touches, and its doodads' records only differ by tile.  A WMO that is a whole zone has tens of thousands
// of doodads on over a hundred tiles.  They're worked out once for a placement, for as long as no model file has changed, and written with the tile filled in
struct DoodadSetRecords
{
    Vec3D Position;
    Vec3D Rotation;
    uint32 ChangeCount = 0;
    std::vector<char> Bytes;
    std::vector<std::size_t> RecordOffsets;
};

static uint32 DoodadSetRecordsMapID = 0;
static std::map<std::tuple<WMODoodadData const*, uint32, uint16, bool>, DoodadSetRecords> DoodadSetRecordsByPlacement;

template <typename T>
static void AddRecordBytes(std::vector<char>& bytes, T const* value, std::size_t count = 1)
{
    char const* start = reinterpret_cast<char const*>(value);
    bytes.insert(bytes.end(), start, start + sizeof(T) * count);
}

static void WriteDoodadSetRecords(DoodadSetRecords& records, uint32 tileX, uint32 tileY, FILE* pDirfile)
{
    if (records.Bytes.empty())
        return;
    for (std::size_t recordOffset : records.RecordOffsets)
    {
        memcpy(&records.Bytes[recordOffset + sizeof(uint32)], &tileX, sizeof(uint32));
        memcpy(&records.Bytes[recordOffset + sizeof(uint32) * 2], &tileY, sizeof(uint32));
    }
    fwrite(records.Bytes.data(), 1, records.Bytes.size(), pDirfile);
}

void Doodad::ExtractSet(WMODoodadData const& doodadData, ADT::MODF const& wmo, uint32 mapID, uint32 tileX, uint32 tileY, FILE* pDirfile)
{
    if (wmo.DoodadSet >= doodadData.Sets.size())
        return;

    if (DoodadSetRecordsMapID != mapID)
    {
        DoodadSetRecordsMapID = mapID;
        DoodadSetRecordsByPlacement.clear();
    }
    bool isWorldSpawn = tileX == 65 && tileY == 65;
    DoodadSetRecords& records = DoodadSetRecordsByPlacement[std::make_tuple(&doodadData, wmo.UniqueId, uint16(wmo.DoodadSet), isWorldSpawn)];
    if (!records.RecordOffsets.empty() && records.ChangeCount == ModelFileChangeCount && memcmp(&records.Position, &wmo.Position, sizeof(Vec3D)) == 0
        && memcmp(&records.Rotation, &wmo.Rotation, sizeof(Vec3D)) == 0)
    {
        WriteDoodadSetRecords(records, tileX, tileY, pDirfile);
        return;
    }
    records.Position = wmo.Position;
    records.Rotation = wmo.Rotation;
    records.ChangeCount = ModelFileChangeCount;
    records.Bytes.clear();
    records.RecordOffsets.clear();

    G3D::Vector3 wmoPosition(wmo.Position.z, wmo.Position.x, wmo.Position.y);
    G3D::Matrix3 wmoRotation = G3D::Matrix3::fromEulerAnglesZYX(G3D::toRadians(wmo.Rotation.y), G3D::toRadians(wmo.Rotation.x), G3D::toRadians(wmo.Rotation.z));

    uint16 doodadId = 0;
    WMO::MODS const& doodadSetData = doodadData.Sets[wmo.DoodadSet];
    for (uint16 doodadIndex : doodadData.References)
    {
        if (doodadIndex < doodadSetData.StartIndex ||
            doodadIndex >= doodadSetData.StartIndex + doodadSetData.Count)
            continue;

        WMO::MODD const& doodad = doodadData.Spawns[doodadIndex];

        char ModelInstName[1024];
        sprintf(ModelInstName, "%s", GetPlainName(&doodadData.Paths[doodad.NameIndex]));
        uint32 nlen = strlen(ModelInstName);
        fixnamen(ModelInstName, nlen);
        fixname2(ModelInstName, nlen);
        if (nlen > 3)
        {
            char const* extension = &ModelInstName[nlen - 4];
            if (!strcmp(extension, ".mdx") || !strcmp(extension, ".mdl"))
            {
                ModelInstName[nlen - 2] = '2';
                ModelInstName[nlen - 1] = '\0';
            }
        }

        if (!IsModelPlaceable(ModelInstName))
            continue;

        assert(doodadId < std::numeric_limits<uint16>::max());
        ++doodadId;

        G3D::Vector3 position = wmoPosition + (wmoRotation * G3D::Vector3(doodad.Position.x, doodad.Position.y, doodad.Position.z));

        Vec3D rotation;
        (G3D::Quat(doodad.Rotation.X, doodad.Rotation.Y, doodad.Rotation.Z, doodad.Rotation.W)
            .toRotationMatrix() * wmoRotation)
            .toEulerAnglesXYZ(rotation.z, rotation.x, rotation.y);

        rotation.z = G3D::toDegrees(rotation.z);
        rotation.x = G3D::toDegrees(rotation.x);
        rotation.y = G3D::toDegrees(rotation.y);

        uint16 nameSet = 0;     // not used for models
        uint32 uniqueId = GenerateUniqueObjectId(wmo.UniqueId, doodadId);
        uint32 tcflags = MOD_M2;
        if (isWorldSpawn)
            tcflags |= MOD_WORLDSPAWN;

        //write mapID, tileX, tileY, Flags, NameSet, UniqueId, Pos, Rot, Scale, name
        records.RecordOffsets.push_back(records.Bytes.size());
        AddRecordBytes(records.Bytes, &mapID);
        AddRecordBytes(records.Bytes, &tileX);
        AddRecordBytes(records.Bytes, &tileY);
        AddRecordBytes(records.Bytes, &tcflags);
        AddRecordBytes(records.Bytes, &nameSet);
        AddRecordBytes(records.Bytes, &uniqueId);
        AddRecordBytes(records.Bytes, &position);
        AddRecordBytes(records.Bytes, &rotation);
        AddRecordBytes(records.Bytes, &doodad.Scale);
        AddRecordBytes(records.Bytes, &nlen);
        AddRecordBytes(records.Bytes, ModelInstName, nlen);
    }
    WriteDoodadSetRecords(records, tileX, tileY, pDirfile);
}
