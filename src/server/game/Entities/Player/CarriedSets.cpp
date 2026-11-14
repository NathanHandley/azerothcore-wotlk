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

// MergedWorlds carried sets: see CarriedSets.h.  The mechanics were mod-rowow's (UpdateCarriedSet, TakeCarriedSet, PutCarriedSet,
// SaveIdentityStowedItems, 2026-11-02 to 11-13), moved here unchanged so every game module shares one owner of the swap

#include "CarriedSets.h"
#include "Bag.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include <array>

namespace
{
    // Written at startup only (RegisterWorld), read-only once sessions exist
    std::array<CarriedSetWorld*, MAX_CARRIED_SET_WORLDS> RegisteredWorlds = { };

    struct CarriedSetWorldState
    {
        bool IsLoaded = false;
        uint8 Side = CARRIED_SET_SIDE_NONE;
        uint8 SavedSide = CARRIED_SET_SIDE_NONE; // As last saved with the inventory's rows (a save of the money alone keeps it)
        bool IsChanged = false; // Swapped since the last save with the inventory
        uint32 Key = 0; // The module's key of its rows (its identity's ID), for the logs
        std::vector<CarriedSetItem> StowedItems; // The set of the pair (this world and WoW) that isn't on
    };

    // A player's carried sets, on the player (Object::CustomData) while it's logged in.  Only the player's own thread touches it
    struct PlayerCarriedSets : public DataMap::Base
    {
        ~PlayerCarriedSets() override
        {
            // The stowed sets' items go with the player (a logout): its slots don't have them
            for (CarriedSetWorldState& state : States)
                for (CarriedSetItem const& stowedItem : state.StowedItems)
                    delete stowedItem.StowedItem;
        }

        std::array<CarriedSetWorldState, MAX_CARRIED_SET_WORLDS> States;
    };

    char const* const PlayerCarriedSetsKey = "MWCarriedSets";

    PlayerCarriedSets* GetPlayerSets(Player* player)
    {
        return player->CustomData.GetDefault<PlayerCarriedSets>(PlayerCarriedSetsKey);
    }

    CarriedSetWorldState* FindState(Player* player, uint8 worldID)
    {
        if (worldID == CARRIED_SET_WORLD_WOW || worldID >= MAX_CARRIED_SET_WORLDS)
            return nullptr;
        PlayerCarriedSets* sets = player->CustomData.Get<PlayerCarriedSets>(PlayerCarriedSetsKey);
        return sets != nullptr ? &sets->States[worldID] : nullptr;
    }

    CarriedSetWorldState* FindLoadedState(Player* player, uint8 worldID)
    {
        CarriedSetWorldState* state = FindState(player, worldID);
        return state != nullptr && state->IsLoaded == true ? state : nullptr;
    }

    // Takes the carried set off the character: the bags' items first (so each bag comes off empty), then the bags, then the backpack, each with
    // the place it had.  The items stay as they are, their rows and GUIDs too (a module may keep its equipment by GUID); only their places change,
    // which the next save with the inventory writes (SaveWorld)
    std::vector<CarriedSetItem> TakeCarriedSet(Player* player)
    {
        std::vector<CarriedSetItem> takenItems;
        auto takeItem = [player, &takenItems](uint8 bag, uint8 slot)
        {
            Item* item = player->GetItemByPos(bag, slot);
            if (item == nullptr)
                return;
            player->MoveItemFromInventory(bag, slot, true);
            if (item->GetState() == ITEM_UNCHANGED)
                item->FSetState(ITEM_CHANGED); // Its item_instance row is written with the set, as it is now (the refund the move took off)
            CarriedSetItem takenItem;
            takenItem.StowedItem = item;
            takenItem.Bag = bag;
            takenItem.Slot = slot;
            takenItems.push_back(takenItem);
        };
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; bagSlot++)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); slot++)
                    takeItem(bagSlot, uint8(slot));
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; slot++)
            takeItem(INVENTORY_SLOT_BAG_0, slot);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
            takeItem(INVENTORY_SLOT_BAG_0, slot);
        return takenItems;
    }

    // Puts an item of a carried set in a free place of the carried slots.  Only the place is checked (a bag slot takes a bag, a bag takes what its kind
    // takes), not what a new item is: the item was there before, and a unique item's twin got while it was stowed mustn't keep it out
    bool PutCarriedItem(Player* player, Item* item, uint8 bag, uint8 slot)
    {
        if (player->GetItemByPos(bag, slot) != nullptr)
            return false;
        if (bag == INVENTORY_SLOT_BAG_0)
        {
            if (CarriedSets::IsCarriedBagSlot(bag, slot) == true ? item->ToBag() == nullptr : (slot < INVENTORY_SLOT_ITEM_START || slot >= INVENTORY_SLOT_ITEM_END))
                return false;
        }
        else
        {
            Bag* container = (bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END) ? player->GetBagByPos(bag) : nullptr;
            if (container == nullptr || slot >= container->GetBagSize() || item->IsNotEmptyBag() == true ||
                ItemCanGoIntoBag(item->GetTemplate(), container->GetTemplate()) == false)
                return false;
        }
        // Player::MoveItemToInventory's, without the epic item achievement: the item comes back to the bags, it isn't new
        player->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
        ItemPosCountVec destination;
        destination.push_back(ItemPosCount((uint16(bag) << 8) | slot, item->GetCount()));
        Item* storedItem = player->StoreItem(destination, item, true);
        storedItem->SetState(ITEM_CHANGED, player);
        if (storedItem->IsBOPTradable() == true)
            player->AddTradeableItem(storedItem);
        return true;
    }

    // Each in the first free carried slot that takes it, the bags first into the free bag slots when they're to be worn
    void PutCarriedItemsAnywhere(Player* player, std::vector<Item*> const& items, bool areBagsWorn, std::vector<Item*>& unplacedItemsOut)
    {
        std::vector<Item*> remainingItems;
        for (Item* item : items)
        {
            bool isPlaced = false;
            if (areBagsWorn == true && item->ToBag() != nullptr)
                for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END && isPlaced == false; slot++)
                    isPlaced = PutCarriedItem(player, item, INVENTORY_SLOT_BAG_0, slot);
            if (isPlaced == false)
                remainingItems.push_back(item);
        }
        for (Item* item : remainingItems)
        {
            bool isPlaced = false;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && isPlaced == false; slot++)
                isPlaced = PutCarriedItem(player, item, INVENTORY_SLOT_BAG_0, slot);
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END && isPlaced == false; bagSlot++)
                if (Bag* bag = player->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize() && isPlaced == false; slot++)
                        isPlaced = PutCarriedItem(player, item, bagSlot, uint8(slot));
            if (isPlaced == false)
                unplacedItemsOut.push_back(item);
        }
    }

    // Puts a carried set on the character, each item back in its place: the bags first (they hold the rest), then the backpack, then the bags' items.
    // One whose place is gone goes in any free slot; what has no room at all is returned
    std::vector<Item*> PutCarriedSet(Player* player, std::vector<CarriedSetItem> const& items, bool areBagsWorn)
    {
        std::vector<Item*> homelessItems;
        for (uint8 pass = 0; pass < 3; pass++)
        {
            for (CarriedSetItem const& stowedItem : items)
            {
                uint8 itemPass = CarriedSets::IsCarriedBagSlot(stowedItem.Bag, stowedItem.Slot) == true ? 0 : (stowedItem.Bag == INVENTORY_SLOT_BAG_0 ? 1 : 2);
                if (itemPass != pass)
                    continue;
                if (PutCarriedItem(player, stowedItem.StowedItem, stowedItem.Bag, stowedItem.Slot) == false)
                    homelessItems.push_back(stowedItem.StowedItem);
            }
        }
        std::vector<Item*> unplacedItems;
        PutCarriedItemsAnywhere(player, homelessItems, areBagsWorn, unplacedItems);
        return unplacedItems;
    }

    // What no carried slot has room for goes by mail.  Each mail's transaction also takes its items out of the bags' rows and the stowed set's, so
    // whenever the server stops, each item is in one place: in the mail, or where it was before
    void MailCarriedItems(Player* player, CarriedSetWorld const* world, std::vector<Item*> const& items)
    {
        for (size_t firstIndex = 0; firstIndex < items.size(); firstIndex += MAX_MAIL_ITEMS)
        {
            CharacterDatabaseTransaction mailTrans = CharacterDatabase.BeginTransaction();
            MailDraft draft(world->MailSubject, world->MailBody);
            for (size_t itemIndex = firstIndex; itemIndex < items.size() && itemIndex < firstIndex + MAX_MAIL_ITEMS; itemIndex++)
            {
                Item* item = items[itemIndex];
                LOG_INFO("entities.player", "Carried set of {} ({}): item {} (entry {}) had no room, sent by mail", player->GetName(), world->Name,
                    item->GetGUID().GetCounter(), item->GetEntry());
                Item::DeleteFromInventoryDB(mailTrans, item->GetGUID().GetCounter());
                mailTrans->Append("DELETE FROM `{}` WHERE `item_guid` = {}", world->StowedTable, item->GetGUID().GetCounter());
                item->SaveToDB(mailTrans);
                draft.AddItem(item);
            }
            draft.SendMailTo(mailTrans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
            CharacterDatabase.CommitTransaction(mailTrans);
        }
    }

    // Nothing in the bags may be in use as they change (the module stopped casts): a trade offering them, the loot of one being opened
    void PrepareForSwap(Player* player)
    {
        if (player->GetTradeData() != nullptr)
            player->TradeCancel(true);
        if (ObjectGuid lootGUID = player->GetLootGUID())
            player->GetSession()->DoLootRelease(lootGUID);
    }

    // Has every registered world's module load its state for the player (a world's set may be on from a save, the character's own in its list)
    bool LoadAllWorlds(Player* player, CarriedSetWorld const* forWorld)
    {
        for (uint8 worldID = 1; worldID < MAX_CARRIED_SET_WORLDS; worldID++)
        {
            CarriedSetWorld* world = RegisteredWorlds[worldID];
            if (world == nullptr || CarriedSets::IsWorldLoaded(player, worldID) == true)
                continue;
            world->LoadForPlayer(player);
            if (CarriedSets::IsWorldLoaded(player, worldID) == false)
            {
                LOG_ERROR("entities.player", "CarriedSets: {}'s {} set wasn't loaded, so the {} set isn't put on", player->GetName(), world->Name, forWorld->Name);
                return false;
            }
        }
        return true;
    }

    // The world's set off and the character's own back on (its side is CARRIED_SET_SIDE_WORLD)
    void SwapWorldSetOff(Player* player, CarriedSetWorld const* world, CarriedSetWorldState& state)
    {
        PrepareForSwap(player);
        std::vector<CarriedSetItem> takenItems = TakeCarriedSet(player);
        std::vector<Item*> unplacedItems = PutCarriedSet(player, state.StowedItems, false);
        state.StowedItems = takenItems;
        // Only a set saved wrong leaves anything with no room
        MailCarriedItems(player, world, unplacedItems);
        state.Side = CARRIED_SET_SIDE_WOW;
        state.IsChanged = true;
    }
}

void CarriedSets::RegisterWorld(CarriedSetWorld* world)
{
    if (world == nullptr || world->WorldID == CARRIED_SET_WORLD_WOW || world->WorldID >= MAX_CARRIED_SET_WORLDS)
    {
        LOG_ERROR("entities.player", "CarriedSets::RegisterWorld: world {} isn't a world id (1 to {})", world != nullptr ? world->WorldID : 0, MAX_CARRIED_SET_WORLDS - 1);
        return;
    }
    if (RegisteredWorlds[world->WorldID] != nullptr && RegisteredWorlds[world->WorldID] != world)
    {
        LOG_ERROR("entities.player", "CarriedSets::RegisterWorld: world {} ({}) is {}'s already", world->WorldID, world->Name, RegisteredWorlds[world->WorldID]->Name);
        return;
    }
    RegisteredWorlds[world->WorldID] = world;
}

CarriedSetWorld* CarriedSets::GetWorld(uint8 worldID)
{
    return worldID < MAX_CARRIED_SET_WORLDS ? RegisteredWorlds[worldID] : nullptr;
}

bool CarriedSets::LoadWorld(Player* player, uint8 worldID, uint32 key, uint8 side)
{
    // The stowed set's items, loaded as Player::_LoadInventory loads the bags' (its columns, for Item::LoadFromDB), but kept out of the player's
    // slots.  One that can't be loaded (no such item template now) or that is in the player's slots already is left out, its item_instance row as
    // it was; the next save of the set drops its row from the set
    CarriedSetWorld* world = GetWorld(worldID);
    if (world == nullptr || player == nullptr)
        return false;
    CarriedSetWorldState& state = GetPlayerSets(player)->States[worldID];
    if (state.IsLoaded == true)
    {
        LOG_ERROR("entities.player", "CarriedSets::LoadWorld: {}'s {} set is loaded already (key {}), not loaded again for key {}", player->GetName(), world->Name, state.Key, key);
        return false;
    }
    state = CarriedSetWorldState();
    state.IsLoaded = true;
    state.Key = key;
    state.Side = side <= CARRIED_SET_SIDE_WORLD ? side : uint8(CARRIED_SET_SIDE_NONE);
    state.SavedSide = state.Side;
    QueryResult result = CharacterDatabase.Query("SELECT `instance`.`creatorGuid`, `instance`.`giftCreatorGuid`, `instance`.`count`, `instance`.`duration`, `instance`.`charges`, "
        "`instance`.`flags`, `instance`.`enchantments`, `instance`.`randomPropertyId`, `instance`.`durability`, `instance`.`playedTime`, `instance`.`text`, `stowed`.`bag`, "
        "`stowed`.`slot`, `stowed`.`item_guid`, `instance`.`itemEntry` FROM `{}` `stowed` JOIN `item_instance` `instance` ON `instance`.`guid` = "
        "`stowed`.`item_guid` WHERE `stowed`.`{}` = {}", world->StowedTable, world->KeyColumn, key);
    if (!result)
        return true;
    do
    {
        Field* fields = result->Fetch();
        ObjectGuid::LowType itemGUID = fields[13].Get<uint32>();
        uint32 itemTemplateID = fields[14].Get<uint32>();
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemTemplateID);
        if (itemTemplate == nullptr || player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUID)) != nullptr)
        {
            LOG_ERROR("entities.player", "CarriedSets::LoadWorld: {}'s stowed {} item {} (entry {}) {}, left out", player->GetName(), world->Name, itemGUID, itemTemplateID,
                itemTemplate == nullptr ? "has no item template" : "is in the bags too");
            continue;
        }
        Item* item = NewItemOrBag(itemTemplate);
        if (item->LoadFromDB(itemGUID, player->GetGUID(), fields, itemTemplateID) == false)
        {
            LOG_ERROR("entities.player", "CarriedSets::LoadWorld: {}'s stowed {} item {} (entry {}) didn't load, left out", player->GetName(), world->Name, itemGUID, itemTemplateID);
            delete item;
            continue;
        }
        item->FSetState(ITEM_UNCHANGED);
        CarriedSetItem stowedItem;
        stowedItem.StowedItem = item;
        stowedItem.Bag = fields[11].Get<uint8>();
        stowedItem.Slot = fields[12].Get<uint8>();
        state.StowedItems.push_back(stowedItem);
    } while (result->NextRow());
    return true;
}

bool CarriedSets::StartWorld(Player* player, uint8 worldID)
{
    CarriedSetWorld* world = GetWorld(worldID);
    if (world == nullptr || player == nullptr)
        return false;
    CarriedSetWorldState& state = GetPlayerSets(player)->States[worldID];
    if (state.IsLoaded == true)
    {
        LOG_ERROR("entities.player", "CarriedSets::StartWorld: {}'s {} set is loaded already (key {})", player->GetName(), world->Name, state.Key);
        return false;
    }
    state = CarriedSetWorldState();
    state.IsLoaded = true;
    return true;
}

bool CarriedSets::UnloadWorld(Player* player, uint8 worldID)
{
    CarriedSetWorldState* state = FindState(player, worldID);
    if (state == nullptr || state->IsLoaded == false)
        return true;
    if (state->Side == CARRIED_SET_SIDE_WORLD)
    {
        LOG_ERROR("entities.player", "CarriedSets::UnloadWorld: {}'s {} set is on the character: its stowed list is the character's own set, kept", player->GetName(),
            GetWorld(worldID) != nullptr ? GetWorld(worldID)->Name : "?");
        return false;
    }
    for (CarriedSetItem const& stowedItem : state->StowedItems)
        delete stowedItem.StowedItem;
    *state = CarriedSetWorldState();
    return true;
}

bool CarriedSets::IsWorldLoaded(Player* player, uint8 worldID)
{
    return player != nullptr && FindLoadedState(player, worldID) != nullptr;
}

uint8 CarriedSets::GetWornWorld(Player* player)
{
    PlayerCarriedSets* sets = player != nullptr ? player->CustomData.Get<PlayerCarriedSets>(PlayerCarriedSetsKey) : nullptr;
    if (sets == nullptr)
        return CARRIED_SET_WORLD_WOW;
    for (uint8 worldID = 1; worldID < MAX_CARRIED_SET_WORLDS; worldID++)
        if (sets->States[worldID].IsLoaded == true && sets->States[worldID].Side == CARRIED_SET_SIDE_WORLD)
            return worldID;
    return CARRIED_SET_WORLD_WOW;
}

uint8 CarriedSets::GetSide(Player* player, uint8 worldID)
{
    CarriedSetWorldState* state = player != nullptr ? FindLoadedState(player, worldID) : nullptr;
    return state != nullptr ? state->Side : uint8(CARRIED_SET_SIDE_NONE);
}

uint8 CarriedSets::GetSavedSide(Player* player, uint8 worldID)
{
    CarriedSetWorldState* state = player != nullptr ? FindLoadedState(player, worldID) : nullptr;
    return state != nullptr ? state->SavedSide : uint8(CARRIED_SET_SIDE_NONE);
}

bool CarriedSets::IsChanged(Player* player, uint8 worldID)
{
    CarriedSetWorldState* state = player != nullptr ? FindLoadedState(player, worldID) : nullptr;
    return state != nullptr && state->IsChanged == true;
}

std::vector<CarriedSetItem> const* CarriedSets::GetStowedItems(Player* player, uint8 worldID)
{
    CarriedSetWorldState* state = player != nullptr ? FindLoadedState(player, worldID) : nullptr;
    return state != nullptr ? &state->StowedItems : nullptr;
}

Item* CarriedSets::FindStowedItem(Player* player, uint8 worldID, uint32 itemGUIDLow)
{
    if (std::vector<CarriedSetItem> const* stowedItems = GetStowedItems(player, worldID))
        for (CarriedSetItem const& stowedItem : *stowedItems)
            if (stowedItem.StowedItem != nullptr && stowedItem.StowedItem->GetGUID().GetCounter() == itemGUIDLow)
                return stowedItem.StowedItem;
    return nullptr;
}

CarriedSetResult CarriedSets::PutOn(Player* player, uint8 worldID, bool isNewSet)
{
    CarriedSetWorld* world = GetWorld(worldID);
    if (world == nullptr || player == nullptr)
        return CARRIED_SET_NO_WORLD;

    // Every registered world is loaded first: one of them may have its set on from a save, with the character's own in its list
    if (LoadAllWorlds(player, world) == false)
        return CARRIED_SET_NOT_LOADED;
    CarriedSetWorldState& state = GetPlayerSets(player)->States[worldID];

    // Another world's set on is taken off first, unless that world still needs it (its form, its page)
    uint8 wornWorldID = GetWornWorld(player);
    if (wornWorldID == worldID)
        return CARRIED_SET_OK;
    if (wornWorldID != CARRIED_SET_WORLD_WOW)
    {
        CarriedSetWorld* wornWorld = RegisteredWorlds[wornWorldID];
        if (wornWorld->IsSetNeeded(player) == true)
            return CARRIED_SET_BUSY;
        SwapWorldSetOff(player, wornWorld, GetPlayerSets(player)->States[wornWorldID]);
    }

    PrepareForSwap(player);
    std::vector<CarriedSetItem> takenItems = TakeCarriedSet(player);
    std::vector<Item*> unplacedItems;
    if (state.Side == CARRIED_SET_SIDE_NONE && isNewSet == false)
    {
        // A set from before the sets were apart, the world's items (and its bags with them) mixed in with the character's.  The character's things
        // become its set, each in its place (one that was in the world's bag where there's room), and are stowed; the world's things become the
        // world's set, each in its place, its bags in the bag slots.  None is stowed yet, but any that were go in too, so none is lost
        takenItems.insert(takenItems.end(), state.StowedItems.begin(), state.StowedItems.end());
        state.StowedItems.clear();
        std::vector<CarriedSetItem> characterItems;
        std::vector<CarriedSetItem> worldItems;
        for (CarriedSetItem takenItem : takenItems)
        {
            if (world->IsWorldItem(takenItem.StowedItem->GetTemplate()) == false)
            {
                characterItems.push_back(takenItem);
                continue;
            }
            if (takenItem.StowedItem->ToBag() != nullptr && IsCarriedBagSlot(takenItem.Bag, takenItem.Slot) == false)
                takenItem.Slot = NULL_SLOT; // A world's bag given while the bag slots were full goes in one now
            worldItems.push_back(takenItem);
        }
        std::vector<Item*> unplacedCharacterItems = PutCarriedSet(player, characterItems, false);
        state.StowedItems = TakeCarriedSet(player);
        unplacedItems = PutCarriedSet(player, worldItems, true);
        // The character's things its own set has no room for stay with the world's rather than leave the bags
        PutCarriedItemsAnywhere(player, unplacedCharacterItems, false, unplacedItems);
        LOG_INFO("entities.player", "Carried set of {} ({}): set apart, {} of the character's things stowed and {} of the world's in the bags ({} of the character's stay with them)",
            player->GetName(), world->Name, state.StowedItems.size(), worldItems.size(), unplacedCharacterItems.size());
    }
    else
    {
        unplacedItems = PutCarriedSet(player, state.StowedItems, false);
        state.StowedItems = takenItems;
    }
    // Only a set saved wrong (or a mixed one too full) leaves anything with no room
    MailCarriedItems(player, world, unplacedItems);
    state.Side = CARRIED_SET_SIDE_WORLD;
    state.IsChanged = true;
    return CARRIED_SET_OK;
}

bool CarriedSets::CanPutOn(Player* player, uint8 worldID)
{
    CarriedSetWorld* world = GetWorld(worldID);
    if (world == nullptr || player == nullptr || LoadAllWorlds(player, world) == false)
        return false;
    uint8 wornWorldID = GetWornWorld(player);
    return wornWorldID == CARRIED_SET_WORLD_WOW || wornWorldID == worldID || RegisteredWorlds[wornWorldID]->IsSetNeeded(player) == false;
}

CarriedSetResult CarriedSets::TakeOff(Player* player, uint8 worldID)
{
    CarriedSetWorld* world = GetWorld(worldID);
    if (world == nullptr || player == nullptr)
        return CARRIED_SET_NO_WORLD;
    CarriedSetWorldState* state = FindLoadedState(player, worldID);
    if (state == nullptr)
        return CARRIED_SET_NOT_LOADED;
    if (state->Side != CARRIED_SET_SIDE_WORLD)
        return CARRIED_SET_OK;
    SwapWorldSetOff(player, world, *state);
    return CARRIED_SET_OK;
}

void CarriedSets::SaveWorld(Player* player, uint8 worldID, uint32 key, CharacterDatabaseTransaction trans)
{
    // In the inventory save's transaction: the stowed items leave the bags' rows (and the account bank's) as they come into the set's, with their
    // item_instance rows as they are now, and the items that came back have their bags' rows from the same save (Player::_SaveInventory)
    CarriedSetWorld* world = GetWorld(worldID);
    CarriedSetWorldState* state = player != nullptr ? FindLoadedState(player, worldID) : nullptr;
    if (world == nullptr || state == nullptr)
        return;
    state->SavedSide = state->Side;
    if (state->IsChanged == false)
        return;
    trans->Append("DELETE FROM `{}` WHERE `{}` = {}", world->StowedTable, world->KeyColumn, key);
    for (CarriedSetItem const& stowedItem : state->StowedItems)
    {
        Item* item = stowedItem.StowedItem;
        Item::DeleteFromInventoryDB(trans, item->GetGUID().GetCounter());
        item->SaveToDB(trans);
        trans->Append("INSERT INTO `{}` (`{}`, `item_guid`, `bag`, `slot`) VALUES ({}, {}, {}, {})", world->StowedTable, world->KeyColumn, key,
            item->GetGUID().GetCounter(), stowedItem.Bag, stowedItem.Slot);
    }
    state->Key = key;
    state->IsChanged = false;
}

bool CarriedSets::IsCarriedSetPosition(uint8 bag, uint8 slot)
{
    if (bag == INVENTORY_SLOT_BAG_0)
        return slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_ITEM_END;
    return bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END;
}

bool CarriedSets::IsCarriedBagSlot(uint8 bag, uint8 slot)
{
    return bag == INVENTORY_SLOT_BAG_0 && slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END;
}
