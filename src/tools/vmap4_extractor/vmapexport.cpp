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

#define _CRT_SECURE_NO_DEPRECATE
#include <cerrno>
#include <cstdio>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#if defined(WIN32) || defined(_WIN32)
#include <Windows.h>
#include <direct.h>
#define mkdir _mkdir
#endif

#undef min
#undef max

//From Extractor
#include "vmapexport.h"
#include "adtfile.h"
#include "dbcfile.h"
#include "mpq_libmpq04.h"
#include "wdtfile.h"
#include "wmo.h"
#include "model.h"
#include <G3D/Matrix3.h>
#include <G3D/Quat.h>
#include <G3D/Vector3.h>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

//------------------------------------------------------------------------------
// Defines

#define MPQ_BLOCK_SIZE 0x1000

//-----------------------------------------------------------------------------

extern ArchiveSet gOpenArchives;

typedef struct
{
    char name[64];
    unsigned int id;
} map_id;

std::vector<map_id> map_ids;
uint32 map_count;
char output_path[128] = ".";
char input_path[1024] = ".";
bool hasInputPathParam = false;
bool preciseVectorData = false;
std::unordered_map<std::string, WMODoodadData> WmoDoodads;

// Constants

char const* szWorkDirWmo = "./Buildings";

// MergedWorlds: with any here, only these maps are extracted (and the gameobject models are left out)
static std::set<uint32> MapIDFilter;
static bool ExtractGameobjectModelsWithMapFilter = false; // MergedWorlds: -g

// MergedWorlds: a placement only goes in if its model has vertices, and a WMO's doodads are placed again on every tile the WMO touches, so asking the file each time
// opened files 80 million times over a run with the City of Heroes zones, nearly all of the run.  Each model is read once.  An answer of no holds until any model
// file changes, as one with this name can still arrive
struct ModelPlaceableEntry
{
    bool IsPlaceable = false;
    uint32 ChangeCount = 0;
};
static std::unordered_map<std::string, ModelPlaceableEntry> ModelPlaceableEntries;
uint32 ModelFileChangeCount = 0;

bool IsModelPlaceable(char const* modelName)
{
    auto entryIter = ModelPlaceableEntries.find(modelName);
    if (entryIter != ModelPlaceableEntries.end() && (entryIter->second.IsPlaceable || entryIter->second.ChangeCount == ModelFileChangeCount))
        return entryIter->second.IsPlaceable;

    char tempname[1036];
    sprintf(tempname, "%s/%s", szWorkDirWmo, modelName);
    ModelPlaceableEntry entry;
    entry.ChangeCount = ModelFileChangeCount;
    if (FILE* input = fopen(tempname, "rb"))
    {
        fseek(input, 8, SEEK_SET); // get the correct no of vertices
        int nVertices;
        int count = fread(&nVertices, sizeof(int), 1, input);
        fclose(input);
        entry.IsPlaceable = count == 1 && nVertices != 0;
    }
    ModelPlaceableEntries[modelName] = entry;
    return entry.IsPlaceable;
}

void OnModelFileChanged(char const* modelName)
{
    ModelPlaceableEntries.erase(modelName);
    ++ModelFileChangeCount;
}

std::map<std::pair<uint32, uint16>, uint32> uniqueObjectIds;

uint32 GenerateUniqueObjectId(uint32 clientId, uint16 clientDoodadId)
{
    return uniqueObjectIds.emplace(std::make_pair(clientId, clientDoodadId), uint32(uniqueObjectIds.size() + 1)).first->second;
}

// Local testing functions

bool FileExists(char const* file)
{
    if (FILE* n = fopen(file, "rb"))
    {
        fclose(n);
        return true;
    }
    return false;
}

void strToLower(char* str)
{
    while (*str)
    {
        *str = tolower(*str);
        ++str;
    }
}

// MergedWorlds: a City of Heroes mission piece is placed as a game object, and a game object's collision is its WMO's groups alone (a WMO's doodads only collide where
// a map places the WMO).  A mission map's piece has its props as doodads of its WMO, so their collision goes in with its groups: each doodad of its first set, its
// model's collision moved where the WMO puts it, in groups of at most 65535 vertices.  Only those pieces, so a stock game object's collision stays as it was
static bool IsCoHMissionPieceWmo(std::string const& name)
{
    std::string lowerName = name;
    for (char& c : lowerName)
        c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lowerName.rfind("world\\wmo\\cityofheroes\\coh_p_", 0) == 0;
}

struct DoodadCollision
{
    std::vector<G3D::Vector3> Vertices; // The model's own axes
    std::vector<uint16> Indices; // Wound as a WMO group's are
};
static std::unordered_map<std::string, DoodadCollision> DoodadCollisionsByPath; // The extractor runs on one thread

static DoodadCollision const& GetDoodadCollision(std::string path)
{
    std::size_t length = path.length();
    if (length > 3 && (path.compare(length - 4, 4, ".mdx") == 0 || path.compare(length - 4, 4, ".MDX") == 0 || path.compare(length - 4, 4, ".mdl") == 0
        || path.compare(length - 4, 4, ".MDL") == 0))
        path.replace(length - 2, 2, "2");
    auto collisionIter = DoodadCollisionsByPath.find(path);
    if (collisionIter != DoodadCollisionsByPath.end())
        return collisionIter->second;
    DoodadCollision& collision = DoodadCollisionsByPath[path];
    Model model(path);
    if (model.open())
    {
        // open() turns the vertices to (x, z, -y), and a model's triangles face the other way from a WMO group's only once ConvertToVMAPModel swaps them
        collision.Vertices.reserve(model.header.nBoundingVertices);
        for (uint32 i = 0; i < model.header.nBoundingVertices; ++i)
            collision.Vertices.emplace_back(model.vertices[i].x, -model.vertices[i].z, model.vertices[i].y);
        collision.Indices.assign(model.indices, model.indices + model.header.nBoundingTriangles - model.header.nBoundingTriangles % 3);
    }
    return collision;
}

static void WriteDoodadCollisionGroup(std::vector<G3D::Vector3> const& vertices, std::vector<uint16> const& indices, FILE* output)
{
    G3D::Vector3 boundsMin = vertices[0];
    G3D::Vector3 boundsMax = vertices[0];
    for (G3D::Vector3 const& vertex : vertices)
    {
        boundsMin = boundsMin.min(vertex);
        boundsMax = boundsMax.max(vertex);
    }
    uint32 zero = 0;
    fwrite(&zero, sizeof(uint32), 1, output); // mogpflags
    fwrite(&zero, sizeof(uint32), 1, output); // group WMO ID
    fwrite(&boundsMin, sizeof(float), 3, output);
    fwrite(&boundsMax, sizeof(float), 3, output);
    fwrite(&zero, sizeof(uint32), 1, output); // liquid flags
    uint32 indexCount = uint32(indices.size());
    uint32 vertexCount = uint32(vertices.size());
    uint32 branchCount = 1;
    int blockSize = sizeof(uint32) * 2;
    fwrite("GRP ", 4, 1, output);
    fwrite(&blockSize, sizeof(int), 1, output);
    fwrite(&branchCount, sizeof(uint32), 1, output);
    fwrite(&indexCount, sizeof(uint32), 1, output);
    blockSize = int(sizeof(uint32) + sizeof(uint16) * indexCount);
    fwrite("INDX", 4, 1, output);
    fwrite(&blockSize, sizeof(int), 1, output);
    fwrite(&indexCount, sizeof(uint32), 1, output);
    fwrite(indices.data(), sizeof(uint16), indexCount, output);
    blockSize = int(sizeof(uint32) + sizeof(float) * 3 * vertexCount);
    fwrite("VERT", 4, 1, output);
    fwrite(&blockSize, sizeof(int), 1, output);
    fwrite(&vertexCount, sizeof(uint32), 1, output);
    for (G3D::Vector3 const& vertex : vertices)
        fwrite(&vertex, sizeof(float), 3, output);
}

// Gives back how many groups were written, and adds their triangles to triangleCount
static uint32 WriteDoodadCollisionGroups(WMODoodadData const& doodads, std::unordered_set<uint32> const& validDoodadNames, FILE* output, int& triangleCount)
{
    if (doodads.Sets.empty() || !doodads.Paths)
        return 0;
    WMO::MODS const& doodadSet = doodads.Sets[0];
    std::vector<G3D::Vector3> vertices;
    std::vector<uint16> indices;
    uint32 groupCount = 0;
    for (std::size_t spawnIndex = doodadSet.StartIndex; spawnIndex < std::size_t(doodadSet.StartIndex) + doodadSet.Count && spawnIndex < doodads.Spawns.size(); ++spawnIndex)
    {
        WMO::MODD const& spawn = doodads.Spawns[spawnIndex];
        if (validDoodadNames.find(spawn.NameIndex) == validDoodadNames.end())
            continue;
        DoodadCollision const& collision = GetDoodadCollision(&doodads.Paths[spawn.NameIndex]);
        if (collision.Indices.empty() || collision.Vertices.size() > 65535)
            continue;
        if (vertices.size() + collision.Vertices.size() > 65535)
        {
            WriteDoodadCollisionGroup(vertices, indices, output);
            triangleCount += int(indices.size() / 3);
            ++groupCount;
            vertices.clear();
            indices.clear();
        }
        G3D::Matrix3 rotation = G3D::Quat(spawn.Rotation.X, spawn.Rotation.Y, spawn.Rotation.Z, spawn.Rotation.W).toRotationMatrix();
        G3D::Vector3 position(spawn.Position.x, spawn.Position.y, spawn.Position.z);
        uint16 firstVertex = uint16(vertices.size());
        for (G3D::Vector3 const& vertex : collision.Vertices)
            vertices.push_back(position + rotation * (vertex * spawn.Scale));
        for (uint16 index : collision.Indices)
            indices.push_back(uint16(firstVertex + index));
    }
    if (!indices.empty())
    {
        WriteDoodadCollisionGroup(vertices, indices, output);
        triangleCount += int(indices.size() / 3);
        ++groupCount;
    }
    return groupCount;
}

static bool ExtractWmoFile(std::string& fname, std::string const& originalName, char* plain_name, char const* szLocalFile)
{
    if (FileExists(szLocalFile))
        return true;

    int p = 0;
    // Select root wmo files
    char const* rchr = strrchr(plain_name, '_');
    if (rchr != nullptr)
    {
        char cpy[4];
        memcpy(cpy, rchr, 4);
        for (int m : cpy)
        {
            if (isdigit(m))
                p++;
        }
    }

    // MergedWorlds: a group file's name ends in "_" and exactly three digits ("_000.wmo"); a root's can have more ("COH_mz_v_winter_2010.wmo")
    if (p == 3 && rchr[4] == '.')
        return true;

    bool file_ok = true;
    printf("Extracting %s\n", originalName.c_str());
    WMORoot froot(originalName);
    if (!froot.open())
    {
        printf("Couldn't open RootWmo!!!\n");
        return false;
    }
    FILE* output = fopen(szLocalFile, "wb");
    if (!output)
    {
        printf("couldn't open %s for writing!\n", szLocalFile);
        return false;
    }
    froot.ConvertToVMAPRootWmo(output);
    WMODoodadData& doodads = WmoDoodads[plain_name];
    std::swap(doodads, froot.DoodadData);
    int Wmo_nVertices = 0;
    uint32 groupCount = 0;
    //printf("root has %d groups\n", froot->nGroups);
    if (froot.nGroups != 0)
    {
        for (uint32 i = 0; i < froot.nGroups; ++i)
        {
            char temp[1024];
            strncpy(temp, fname.c_str(), 1024);
            temp[fname.length() - 4] = 0;
            char groupFileName[1024];
            int ret = snprintf(groupFileName, 1024, "%s_%03u.wmo", temp, i);
            if (ret < 0)
            {
                printf("Error when formatting string");
                return false;
            }
            //printf("Trying to open groupfile %s\n",groupFileName);

            string s = groupFileName;
            WMOGroup fgroup(s);
            if (!fgroup.open(&froot))
            {
                printf("Could not open all Group file for: %s\n", plain_name);
                file_ok = false;
                break;
            }

            if (fgroup.ShouldSkip(&froot))
                continue;

            Wmo_nVertices += fgroup.ConvertToVMAPGroupWmo(output, preciseVectorData);
            ++groupCount;
            for (uint16 groupReference : fgroup.DoodadReferences)
            {
                if (groupReference >= doodads.Spawns.size())
                    continue;

                uint32 doodadNameIndex = doodads.Spawns[groupReference].NameIndex;
                if (froot.ValidDoodadNames.find(doodadNameIndex) == froot.ValidDoodadNames.end())
                    continue;

                doodads.References.insert(groupReference);
            }
        }
    }
    if (file_ok && IsCoHMissionPieceWmo(originalName))
        groupCount += WriteDoodadCollisionGroups(doodads, froot.ValidDoodadNames, output, Wmo_nVertices);

    fseek(output, 8, SEEK_SET); // store the correct no of vertices
    fwrite(&Wmo_nVertices, sizeof(int), 1, output);
    // store the correct no of groups
    fwrite(&groupCount, sizeof(uint32), 1, output);
    fclose(output);

    // Delete the extracted file in the case of an error
    if (!file_ok)
        remove(szLocalFile);
    OnModelFileChanged(plain_name);
    return true;
}

// MergedWorlds: every tile names the WMOs on it, so each path is only looked into once
static std::unordered_map<std::string, bool> WmoExtractResults;

bool ExtractSingleWmo(std::string& fname)
{
    // Copy files from archive
    std::string originalName = fname;

    char szLocalFile[1024];
    char* plain_name = GetPlainName(&fname[0]);
    fixnamen(plain_name, strlen(plain_name));
    fixname2(plain_name, strlen(plain_name));
    sprintf(szLocalFile, "%s/%s", szWorkDirWmo, plain_name);

    auto resultIter = WmoExtractResults.find(originalName);
    if (resultIter != WmoExtractResults.end())
        return resultIter->second;
    bool result = ExtractWmoFile(fname, originalName, plain_name, szLocalFile);
    WmoExtractResults[originalName] = result;
    return result;
}

void ParsMapFiles()
{
    char fn[512];
    //char id_filename[64];
    for (unsigned int i = 0; i < map_count; ++i)
    {
        if (!MapIDFilter.empty() && MapIDFilter.find(map_ids[i].id) == MapIDFilter.end())
            continue;
        sprintf(fn,"World\\Maps\\%s\\%s.wdt", map_ids[i].name, map_ids[i].name);
        WDTFile WDT(fn,map_ids[i].name);
        if (WDT.init(map_ids[i].id))
        {
            printf("Processing Map %u\n[", map_ids[i].id);
            for (int x = 0; x < 64; ++x)
            {
                for (int y = 0; y < 64; ++y)
                {
                    if (ADTFile* ADT = WDT.GetMap(x, y))
                    {
                        //sprintf(id_filename,"%02u %02u %03u",x,y,map_ids[i].id);//!!!!!!!!!
                        ADT->init(map_ids[i].id, x, y);
                        delete ADT;
                    }
                }
                printf("#");
                fflush(stdout);
            }
            printf("]\n");
        }
    }
}

void getGamePath()
{
#ifdef _WIN32
    strcpy(input_path, "Data\\");
#else
    strcpy(input_path, "Data/");
#endif
}

bool scan_patches(char* scanmatch, std::vector<std::string>& pArchiveNames)
{
    int i;
    char path[512];

    for (i = 1; i <= 99; i++)
    {
        if (i != 1)
        {
            sprintf(path, "%s-%d.MPQ", scanmatch, i);
        }
        else
        {
            sprintf(path, "%s.MPQ", scanmatch);
        }
#ifdef __linux__
        if (FILE* h = fopen64(path, "rb"))
#else
        if (FILE* h = fopen(path, "rb"))
#endif
        {
            fclose(h);
            //matches.push_back(path);
            pArchiveNames.emplace_back(path);
        }
    }

    // MergedWorlds: the lettered patches the client loads after the numbered ones (patch-enUS-A holds the merged DBCs)
    for (char letter = 'A'; letter <= 'Z'; letter++)
    {
        sprintf(path, "%s-%c.MPQ", scanmatch, letter);
#ifdef __linux__
        if (FILE* h = fopen64(path, "rb"))
#else
        if (FILE* h = fopen(path, "rb"))
#endif
        {
            fclose(h);
            pArchiveNames.emplace_back(path);
        }
    }

    return (true);
}

bool fillArchiveNameVector(std::vector<std::string>& pArchiveNames)
{
    if (!hasInputPathParam)
        getGamePath();

    printf("\nGame path: %s\n", input_path);

    char path[512];
    string in_path(input_path);
    std::vector<std::string> locales, searchLocales;

    searchLocales.emplace_back("enGB");
    searchLocales.emplace_back("enUS");
    searchLocales.emplace_back("deDE");
    searchLocales.emplace_back("esES");
    searchLocales.emplace_back("frFR");
    searchLocales.emplace_back("koKR");
    searchLocales.emplace_back("zhCN");
    searchLocales.emplace_back("zhTW");
    searchLocales.emplace_back("enCN");
    searchLocales.emplace_back("enTW");
    searchLocales.emplace_back("esMX");
    searchLocales.emplace_back("ruRU");

    for (auto & searchLocale : searchLocales)
    {
        std::string localePath = in_path + searchLocale;
        // check if locale exists:
        struct stat status;
        if (stat(localePath.c_str(), &status))
            continue;
        if ((status.st_mode & S_IFDIR) == 0)
            continue;
        printf("Found locale '%s'\n", searchLocale.c_str());
        locales.push_back(searchLocale);
    }
    printf("\n");

    // open locale expansion and common files
    printf("Adding data files from locale directories.\n");
    for (auto & locale : locales)
    {
        pArchiveNames.push_back(in_path + locale + "/locale-" + locale + ".MPQ");
        pArchiveNames.push_back(in_path + locale + "/expansion-locale-" + locale + ".MPQ");
        pArchiveNames.push_back(in_path + locale + "/lichking-locale-" + locale + ".MPQ");
    }

    // open expansion and common files
    pArchiveNames.push_back(input_path + string("common.MPQ"));
    pArchiveNames.push_back(input_path + string("common-2.MPQ"));
    pArchiveNames.push_back(input_path + string("expansion.MPQ"));
    pArchiveNames.push_back(input_path + string("lichking.MPQ"));

    // now, scan for the patch levels in the core dir
    printf("Scanning patch levels from data directory.\n");
    int ret = snprintf(path, 512, "%spatch", input_path);
    if (ret < 0)
    {
        printf("Error when formatting string");
        return false;
    }
    if (!scan_patches(path, pArchiveNames))
        return (false);

    // now, scan for the patch levels in locale dirs
    printf("Scanning patch levels from locale directories.\n");
    bool foundOne = false;
    for (auto & locale : locales)
    {
        printf("Locale: %s\n", locale.c_str());
        int ret2 = snprintf(path, 512, "%s%s/patch-%s", input_path, locale.c_str(), locale.c_str());
        if (ret2 < 0)
        {
            printf("Error when formatting string");
            return false;
        }
        if (scan_patches(path, pArchiveNames))
            foundOne = true;
    }

    printf("\n");

    if (!foundOne)
    {
        printf("no locale found\n");
        return false;
    }

    return true;
}

bool processArgv(int argc, char** argv, char const* versionString)
{
    bool result = true;
    hasInputPathParam = false;
    preciseVectorData = false;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp("-s", argv[i]) == 0)
        {
            preciseVectorData = false;
        }
        else if (strcmp("-d", argv[i]) == 0)
        {
            if ((i + 1) < argc)
            {
                hasInputPathParam = true;
                strcpy(input_path, argv[i + 1]);
                if (input_path[strlen(input_path) - 1] != '\\' && input_path[strlen(input_path) - 1] != '/')
                    strcat(input_path, "/");
                ++i;
            }
            else
            {
                result = false;
            }
        }
        else if (strcmp("-?", argv[1]) == 0)
        {
            result = false;
        }
        else if (strcmp("-l", argv[i]) == 0)
        {
            preciseVectorData = true;
        }
        else if (strcmp("-g", argv[i]) == 0)
        {
            // MergedWorlds: the gameobject models too when only some maps are (composed maps' pieces are gameobject models)
            ExtractGameobjectModelsWithMapFilter = true;
        }
        else if (strcmp("-m", argv[i]) == 0)
        {
            if ((i + 1) < argc)
            {
                std::stringstream mapIDs(argv[i + 1]);
                std::string mapID;
                while (std::getline(mapIDs, mapID, ','))
                    MapIDFilter.insert(uint32(atoi(mapID.c_str())));
                ++i;
            }
            else
            {
                result = false;
            }
        }
        else
        {
            result = false;
            break;
        }
    }
    if (!result)
    {
        printf("Extract %s.\n", versionString);
        printf("%s [-?][-s][-l][-d <path>][-m <map id,map id...>][-g]\n", argv[0]);
        printf("   -s : (default) small size (data size optimization), ~500MB less vmap data.\n");
        printf("   -l : large size, ~500MB more vmap data. (might contain more details)\n");
        printf("   -d <path>: Path to the vector data source folder.\n");
        printf("   -m <ids>: Only these maps, and no gameobject models.\n");
        printf("   -g : With -m, the gameobject models as well.\n");
        printf("   -? : This message.\n");
    }
    return result;
}

//xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
// Main
//
// The program must be run with two command line arguments
//
// Arg1 - The source MPQ name (for testing reading and file find)
// Arg2 - Listfile name
//

int main(int argc, char** argv)
{
    bool success = true;
    char const* versionString = "V4.00 2012_02";

    // Use command line arguments, when some
    if (!processArgv(argc, argv, versionString))
        return 1;

    // some simple check if working dir is dirty
    else
    {
        std::string sdir = std::string(szWorkDirWmo) + "/dir";
        std::string sdir_bin = std::string(szWorkDirWmo) + "/dir_bin";
        struct stat status;
        if (!stat(sdir.c_str(), &status) || !stat(sdir_bin.c_str(), &status))
        {
            printf("Your output directory seems to be polluted, please use an empty directory!\n");
            printf("<press return to exit>");
            char garbage[2];
            return scanf("%c", garbage);
        }
    }

    printf("Extract %s. Beginning work ....\n", versionString);
    //xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
    // Create the working directory
    if (mkdir(szWorkDirWmo
#if defined(__linux__) || defined(__APPLE__)
              , 0711
#endif
             ))
        success = (errno == EEXIST);

    // prepare archive name list
    std::vector<std::string> archiveNames;
    fillArchiveNameVector(archiveNames);
    for (auto & archiveName : archiveNames)
    {
        MPQArchive* archive = new MPQArchive(archiveName.c_str());
        if (gOpenArchives.empty() || gOpenArchives.front() != archive)
            delete archive;
    }

    if (gOpenArchives.empty())
    {
        printf("FATAL ERROR: None MPQ archive found by path '%s'. Use -d option with proper path.\n", input_path);
        return 1;
    }

    //xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
    //map.dbc
    if (success)
    {
        DBCFile* dbc = new DBCFile("DBFilesClient\\Map.dbc");
        if (!dbc->open())
        {
            delete dbc;
            printf("FATAL ERROR: Map.dbc not found in data file.\n");
            return 1;
        }
        map_count = dbc->getRecordCount();
        map_ids.resize(map_count);
        for (unsigned int x = 0; x < map_count; ++x)
        {
            map_ids[x].id = dbc->getRecord(x).getUInt(0);

            char const* map_name = dbc->getRecord(x).getString(1);
            std::size_t max_map_name_length = sizeof(map_ids[x].name);
            if (strlen(map_name) >= max_map_name_length)
            {
                delete dbc;
                printf("FATAL ERROR: Map name too long.\n");
                return 1;
            }

            strncpy(map_ids[x].name, map_name, max_map_name_length);
            map_ids[x].name[max_map_name_length - 1] = '\0';
            printf("Map - %s\n", map_ids[x].name);
        }

        delete dbc;
        ParsMapFiles();
        //nError = ERROR_SUCCESS;
        // Extract models, listed in DameObjectDisplayInfo.dbc
        if (MapIDFilter.empty() || ExtractGameobjectModelsWithMapFilter)
            ExtractGameobjectModels();
    }

    printf("\n");
    if (!success)
    {
        printf("ERROR: Extract %s. Work NOT complete.\n   Precise vector data=%d.\nPress any key.\n", versionString, preciseVectorData);
        getchar();
    }

    printf("Extract %s. Work complete. No errors.\n", versionString);
    return 0;
}
