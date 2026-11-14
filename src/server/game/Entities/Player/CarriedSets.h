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

#ifndef _CARRIED_SETS_H
#define _CARRIED_SETS_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include <string>
#include <vector>

class Item;
class Player;
struct ItemTemplate;

// MergedWorlds carried sets (Nathan 2026-11-13, E:\MW\SHARED.md section 1): a character's held inventory per game world.  The carried set is the
// backpack (INVENTORY_SLOT_ITEM_*), the bag slots (INVENTORY_SLOT_BAG_*) and the bags' items; the worn equipment, keyring, currencies, bank and
// buyback are always the character's own.  World 0 is the character's own set (WoW and EverQuest).  Every other world (a game module: Ragnarok
// Online, GunBound, ...) has its own set, and exactly one set is on the character at any time: the others are stowed, each item kept as it is (its
// item_instance row and GUID) with the place it goes back to.
//
// This is the one owner of the swap.  A module never moves the carried slots between sets itself: it registers its world (RegisterWorld), loads
// and saves its world's state for the player with its own identity rows (LoadWorld, SaveWorld), and asks for its set on or off (PutOn, TakeOff).
// Its own rules (which items are its world's, which bags it starts with, when its set goes on) stay in the module.
//
// Each world other than WoW pairs with WoW: its stowed list (in its module's table) holds whichever of the two sets isn't on.  While its set is
// on, that is the character's own set; otherwise its own.  Its side (CARRIED_SET_SIDE_*, kept in the module's identity row) says which.  So the
// character's own set is in at most one world's table, and only one world's side is CARRIED_SET_SIDE_WORLD at a time: a world puts its set on
// only while the character's own is on, or after the world that has its set on gives it up (IsSetNeeded false).
//
// Saving: the stowed rows, the bags' rows and the side are written by the same Player::SaveToDB transaction (SaveWorld from the module's
// OnPlayerSaveCharacterValues when values.IsInventorySaved), so whenever the server stops the database has each item in exactly one place.  A
// swap marks the world changed (IsChanged): its module saves the character soon after (its OnPlayerUpdate, as mod-rowow does).
//
// Threads: a player's carried sets are touched only from that player's own thread (its session's update, its map's update, or the world thread
// with the maps idle), like its inventory.  The world registry is written at startup only (before any session exists) and only read after.

#define CARRIED_SET_WORLD_WOW           0   // The character's own set.  The other ids are SHARED.md's world ids (1 Ragnarok Online, 4 GunBound, ...)
#define MAX_CARRIED_SET_WORLDS          16

enum CarriedSetSide : uint8
{
    CARRIED_SET_SIDE_NONE   = 0,    // Never set apart: the world's items are mixed in with the character's own (its stowed list is empty or old)
    CARRIED_SET_SIDE_WOW    = 1,    // The world's set is stowed (the character's own is on, or another world's)
    CARRIED_SET_SIDE_WORLD  = 2     // The world's set is on the character, the character's own stowed in this world's list
};

enum CarriedSetResult : uint8
{
    CARRIED_SET_OK          = 0,    // Done (or there was nothing to do)
    CARRIED_SET_BUSY        = 1,    // Another world's set is on and still needed (GetWornWorld says which)
    CARRIED_SET_NOT_LOADED  = 2,    // This world, or another registered one, isn't loaded for the player: nothing was moved
    CARRIED_SET_NO_WORLD    = 3     // Not a registered world
};

// An item of a stowed set: out of the player's slots (and out of the world) until its set comes back
struct CarriedSetItem
{
    Item* StowedItem = nullptr;
    uint8 Bag = 255;    // Where it goes back: INVENTORY_SLOT_BAG_0 (255) for the backpack and bag slots, or the bag slot of its bag
    uint8 Slot = 0;
};

// A world's rules and storage, registered once by its module at startup (RegisterWorld).  The table has the columns
// (<KeyColumn>, item_guid, bag, slot), keyed by what the module passes as the key (its identity's ID)
class CarriedSetWorld
{
public:
    virtual ~CarriedSetWorld() = default;

    uint8 WorldID = 0;
    std::string Name;           // For the logs
    std::string StowedTable;    // e.g. mod_rowow_identity_stowed_item
    std::string KeyColumn;      // e.g. guid
    std::string MailSubject;    // What a swap couldn't place is mailed (a set saved wrong, or a mixed one too full)
    std::string MailBody;

    // The module loads its state for this player (it calls LoadWorld or StartWorld) if it hasn't yet.  A world's set may be on the character from
    // a save (a crash in that world), so every registered world is loaded before any swap
    virtual void LoadForPlayer(Player* player) = 0;
    // Its set must stay on (the character is in the world's form, or on its page).  Asked only while its set is on and another world wants to put
    // its set on: when false, its set is taken off first
    virtual bool IsSetNeeded(Player* player) const = 0;
    // An item of this world, for setting apart a set never set apart (CARRIED_SET_SIDE_NONE): these become the world's set, the rest the character's
    virtual bool IsWorldItem(ItemTemplate const* itemTemplate) const = 0;
};

class CarriedSets
{
public:
    // Startup only (a module's OnAfterConfigLoad or earlier, before any session).  The world object lives for the server's life
    static void RegisterWorld(CarriedSetWorld* world);
    static CarriedSetWorld* GetWorld(uint8 worldID);

    // The world's state for a player, from the module's identity: its side as saved, and its stowed list read from its table by key.  Once per
    // identity: UnloadWorld first to load another (a save slot change)
    static bool LoadWorld(Player* player, uint8 worldID, uint32 key, uint8 side);
    // A world with nothing stowed and never set apart (a new identity, before its first save), loaded without reading the database
    static bool StartWorld(Player* player, uint8 worldID);
    // Forgets the world's state, deleting its stowed items' objects (not their rows).  Refused (false) while its set is on: its stowed list is the
    // character's own set then
    static bool UnloadWorld(Player* player, uint8 worldID);
    static bool IsWorldLoaded(Player* player, uint8 worldID);

    // Which world's set is on the character (CARRIED_SET_WORLD_WOW for the character's own), from the loaded worlds
    static uint8 GetWornWorld(Player* player);
    static bool IsWorn(Player* player, uint8 worldID) { return GetWornWorld(player) == worldID; }
    static uint8 GetSide(Player* player, uint8 worldID);       // CARRIED_SET_SIDE_*, NONE when not loaded
    static uint8 GetSavedSide(Player* player, uint8 worldID);  // ... as last saved with the inventory (a save of the money alone writes this)
    static bool IsChanged(Player* player, uint8 worldID);      // Swapped since its last SaveWorld: the module saves the character soon
    static std::vector<CarriedSetItem> const* GetStowedItems(Player* player, uint8 worldID); // nullptr when not loaded
    static Item* FindStowedItem(Player* player, uint8 worldID, uint32 itemGUIDLow);

    // The world's set onto the character, the character's own stowed in its list.  A set never set apart is set apart first (by IsWorldItem),
    // unless isNewSet (a new identity, whose set starts empty).  Another world's set on is taken off first if that world doesn't need it, else BUSY
    static CarriedSetResult PutOn(Player* player, uint8 worldID, bool isNewSet = false);
    // Whether PutOn would put the world's set on now: no other world has its set on and still needs it (loads every registered world, as PutOn)
    static bool CanPutOn(Player* player, uint8 worldID);
    // The character's own set back on, the world's stowed.  Nothing to do (OK) when the world's set isn't on
    static CarriedSetResult TakeOff(Player* player, uint8 worldID);

    // In the inventory save's transaction (only when the inventory is saved): the stowed rows, if changed, with their item_instance rows, and their
    // bag rows deleted (the items that came back get theirs from the same save's _SaveInventory).  The side saved becomes the current one
    static void SaveWorld(Player* player, uint8 worldID, uint32 key, CharacterDatabaseTransaction trans);

    // A place in a carried set: a bag slot, the backpack, or any slot of a worn bag (NULL_SLOT for anywhere in it)
    static bool IsCarriedSetPosition(uint8 bag, uint8 slot);
    static bool IsCarriedBagSlot(uint8 bag, uint8 slot);
};

#endif
