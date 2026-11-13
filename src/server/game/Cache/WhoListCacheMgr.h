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

#ifndef _WHO_LISTCACHE_H_
#define _WHO_LISTCACHE_H_

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "Util.h"

// MergedWorlds: the who list's world block.  A MergedWorlds client ends CMSG_WHO with WHO_LIST_MERGEDWORLDS_MAGIC and the text of its world
// filter (w-Midgard, empty for none); only to such a client SMSG_WHO ends with the magic too, then for each player shown, in the list's order,
// the world's name, a byte of WHO_LIST_CLASS_* flags, and the primary and secondary class's names (empty where a flag says it is the
// character's class, which the client names itself).  A stock client sends no magic and is sent no block
#define WHO_LIST_MERGEDWORLDS_MAGIC         0x4D575731  // "MWW1"
#define WHO_LIST_MAX_ZONES                  50          // Zones a MergedWorlds client may send (z-Prontera is one zone and a dozen Ragnarok maps); stock clients send up to 10
#define WHO_LIST_CLASS_PRIMARY_IS_CLASS     0x01        // The primary class is the character's own (its class color in the list)
#define WHO_LIST_CLASS_SECONDARY_IS_CLASS   0x02        // The secondary class is the character's own

// MergedWorlds: the world a player is in and the classes it shows there (OnPlayerWhoListInfo fills it).  The core's are Azeroth, the zone,
// the character's class and no secondary class ("None"); a game module sets its own world's
struct WhoListWorldInfo
{
    std::string World = "Azeroth";
    uint32 ZoneID = 0;                      // The zone shown, and matched by zone searches: the player's zone, or for a Ragnarok map stitched into a bigger one (Midgard) that map's own area
    bool IsPrimaryClassTheClass = true;     // The primary class is the character's class (PrimaryClass is then unused)
    std::string PrimaryClass;
    bool IsSecondaryClassTheClass = false;  // The secondary class is the character's class (SecondaryClass is then unused)
    std::string SecondaryClass = "None";
};

class WhoListPlayerInfo
{
public:
    WhoListPlayerInfo(ObjectGuid guid, TeamId team, AccountTypes security, uint8 level, uint8 clss, uint8 race, uint32 zoneid, uint8 gender, bool visible, std::wstring const& widePlayerName,
        std::wstring const& wideGuildName, std::string const& playerName, std::string const& guildName, WhoListWorldInfo const& worldInfo) :
        _guid(guid),
        _team(team),
        _security(security),
        _level(level),
        _class(clss),
        _race(race),
        _zoneid(zoneid),
        _gender(gender),
        _visible(visible),
        _widePlayerName(widePlayerName),
        _wideGuildName(wideGuildName),
        _playerName(playerName),
        _guildName(guildName),
        _worldInfo(worldInfo)
    {
        Utf8toWStr(_worldInfo.World, _wideWorld);
        wstrToLower(_wideWorld);
        if (_worldInfo.IsPrimaryClassTheClass == false && Utf8toWStr(_worldInfo.PrimaryClass, _widePrimaryClass) == true)
            wstrToLower(_widePrimaryClass);
        if (_worldInfo.IsSecondaryClassTheClass == false && Utf8toWStr(_worldInfo.SecondaryClass, _wideSecondaryClass) == true)
            wstrToLower(_wideSecondaryClass);
    }

    ObjectGuid GetGuid() const { return _guid; }
    TeamId GetTeamId() const { return _team; }
    AccountTypes GetSecurity() const { return _security; }
    uint8 GetLevel() const { return _level; }
    uint8 GetClass() const { return _class; }
    uint8 GetRace() const { return _race; }
    uint32 GetZoneId() const { return _zoneid; }
    uint8 GetGender() const { return _gender; }
    bool IsVisible() const { return _visible; }
    std::wstring const& GetWidePlayerName() const { return _widePlayerName; }
    std::wstring const& GetWideGuildName() const { return _wideGuildName; }
    std::string const& GetPlayerName() const { return _playerName; }
    std::string const& GetGuildName() const { return _guildName; }
    WhoListWorldInfo const& GetWorldInfo() const { return _worldInfo; } // MergedWorlds
    uint32 GetShownZoneId() const { return _worldInfo.ZoneID; } // MergedWorlds: the zone the list shows (GetZoneId is the player's real zone)
    std::wstring const& GetWideWorld() const { return _wideWorld; }
    std::wstring const& GetWidePrimaryClass() const { return _widePrimaryClass; } // Empty when it's the character's class
    std::wstring const& GetWideSecondaryClass() const { return _wideSecondaryClass; }

private:
    ObjectGuid _guid;
    TeamId _team;
    AccountTypes _security;
    uint8 _level;
    uint8 _class;
    uint8 _race;
    uint32 _zoneid;
    uint8 _gender;
    bool _visible;
    std::wstring _widePlayerName;
    std::wstring _wideGuildName;
    std::string _playerName;
    std::string _guildName;
    WhoListWorldInfo _worldInfo;
    std::wstring _wideWorld;
    std::wstring _widePrimaryClass;
    std::wstring _wideSecondaryClass;
};

using WhoListInfoVector = std::vector<WhoListPlayerInfo>;

class AC_GAME_API WhoListCacheMgr
{
    WhoListCacheMgr() = default;
    ~WhoListCacheMgr() = default;

    WhoListCacheMgr(WhoListCacheMgr const&) = delete;
    WhoListCacheMgr(WhoListCacheMgr&&) = delete;

    WhoListCacheMgr& operator= (WhoListCacheMgr const&) = delete;
    WhoListCacheMgr& operator= (WhoListCacheMgr&&) = delete;
public:
    static WhoListCacheMgr* instance();

    void Update();
    WhoListInfoVector const& GetWhoList() const { return _whoListStorage; }

protected:
    WhoListInfoVector _whoListStorage;
};

#define sWhoListCacheMgr WhoListCacheMgr::instance()

#endif
