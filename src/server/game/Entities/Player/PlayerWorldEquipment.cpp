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

// MergedWorlds world equipment (E:\MW\SHARED.md, "World containers").  Each game world can keep its worn items in hidden 36 slot containers
// on the character, built like the account bank's vaults but in no player slot: a container is found by its bag address (Player.h) and the
// client finds it by its fixed GUID, so no player slot, update field or layout changes.  A world keeps one set of worn items per character
// slot of that game (its "sets", keyed by a number the module picks); the set in use is in the world's equipment container, one other set can
// be shown in its staging container, and the rest are held here, put away, as detached items.  The core stores, loads and saves it all; what
// a slot means and what may go in it is the world's module's (WorldEquipmentHandler)

#include "Bag.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Language.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"

namespace
{
    struct WorldEquipmentWorld
    {
        uint8 SlotCount = 0;
        WorldEquipmentHandler* Handler = nullptr;
    };

    // Written by the modules at startup, before any session exists, and only read after that (as Player::SetWorldBankEnabled)
    WorldEquipmentWorld WorldEquipmentWorlds[WORLD_CONTAINER_WORLD_COUNT + 1];

    // Each block of world containers: what kind it is and its first bag address (world W is first + W - 1).  Equipment has two blocks; a later
    // kind, such as a keyring, takes the next block numbers and one of the reserved address blocks of SHARED.md
    struct WorldContainerBlockInfo
    {
        uint8 Kind;
        uint8 AddressFirst;
    };

    constexpr WorldContainerBlockInfo WorldContainerBlocks[MAX_WORLD_CONTAINER_BLOCK] =
    {
        { WORLD_CONTAINER_KIND_EQUIPMENT, WORLD_CONTAINER_ADDRESS_EQUIPMENT },          // WORLD_CONTAINER_EQUIPMENT
        { WORLD_CONTAINER_KIND_EQUIPMENT, WORLD_CONTAINER_ADDRESS_EQUIPMENT_STAGING },  // WORLD_CONTAINER_EQUIPMENT_STAGING
    };

    // No block may sit on a bag byte that a player slot holding a container has, or a packet naming that bag would mean both
    constexpr bool IsBagHolderSlotNumber(uint32 slot)
    {
        return (slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END) || (slot >= BANK_SLOT_BAG_START && slot < BANK_SLOT_BAG_END)
            || (slot >= ACCOUNT_BANK_BAG_SLOT_START && slot < WORLD_BANK_VAULT_SLOT_END && slot != LIGHT_SOURCE_SLOT);
    }

    constexpr bool AreWorldContainerAddressesFree()
    {
        for (WorldContainerBlockInfo const& block : WorldContainerBlocks)
            for (uint32 address = block.AddressFirst; address < uint32(block.AddressFirst) + WORLD_CONTAINER_WORLD_COUNT; ++address)
                if (address == INVENTORY_SLOT_BAG_0 || address == NULL_BAG || IsBagHolderSlotNumber(address))
                    return false;
        return true;
    }

    static_assert(AreWorldContainerAddressesFree(), "A world container address block overlaps a player slot that holds a container: move the block (E:\\MW\\SHARED.md)");
    static_assert(PLAYER_SLOT_END <= WORLD_CONTAINER_OWN_SLOT, "WORLD_CONTAINER_OWN_SLOT must stay a slot number no player slot uses");
    static_assert(WORLD_CONTAINER_SIZE <= MAX_BAG_SIZE, "A world container is a Bag");

    // A place in the bags the character carries: the backpack, or inside one of the four bags
    bool IsCarriedPos(uint8 bag, uint8 slot)
    {
        if (bag == INVENTORY_SLOT_BAG_0)
            return slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END;
        return bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END;
    }
}

uint8 Player::GetWorldContainerAddress(uint8 block, uint8 world)
{
    if (block >= MAX_WORLD_CONTAINER_BLOCK || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT)
        return NULL_BAG;
    return uint8(WorldContainerBlocks[block].AddressFirst + world - 1);
}

bool Player::GetWorldContainerOfAddress(uint8 bag, uint8& block, uint8& world)
{
    for (uint8 b = 0; b < MAX_WORLD_CONTAINER_BLOCK; ++b)
    {
        if (bag >= WorldContainerBlocks[b].AddressFirst && bag < WorldContainerBlocks[b].AddressFirst + WORLD_CONTAINER_WORLD_COUNT)
        {
            block = b;
            world = uint8(bag - WorldContainerBlocks[b].AddressFirst + 1);
            return true;
        }
    }
    return false;
}

bool Player::IsWorldContainerAddress(uint8 bag)
{
    uint8 block;
    uint8 world;
    return GetWorldContainerOfAddress(bag, block, world);
}

bool Player::IsWorldEquipmentPos(uint8 bag, uint8 /*slot*/)
{
    uint8 block;
    uint8 world;
    return GetWorldContainerOfAddress(bag, block, world) && WorldContainerBlocks[block].Kind == WORLD_CONTAINER_KIND_EQUIPMENT;
}

ObjectGuid::LowType Player::GetWorldContainerGuidCounter(uint8 block, uint8 world)
{
    // Fixed guids at the very top of the item range, below the bank vaults' (which reach at most 20 below the top): the containers are rebuilt
    // at every login and never saved, so they never meet a real item.  The client computes the same (MergedWorldClient Player.h)
    return ObjectGuid::GetMaxCounter(HighGuid::Item) - WORLD_CONTAINER_GUID_OFFSET - WORLD_CONTAINER_WORLD_COUNT * block - world;
}

void Player::RegisterWorldEquipment(uint8 world, uint8 slotCount, WorldEquipmentHandler* handler)
{
    if (world == 0 || world > WORLD_CONTAINER_WORLD_COUNT || slotCount > WORLD_CONTAINER_SIZE || !handler)
    {
        LOG_ERROR("entities.player", "Player::RegisterWorldEquipment: world {} with {} slots can't be registered", world, slotCount);
        return;
    }
    if (WorldEquipmentWorlds[world].Handler)
        LOG_ERROR("entities.player", "Player::RegisterWorldEquipment: world {} was already registered; the later registration replaces it", world);
    WorldEquipmentWorlds[world].SlotCount = slotCount;
    WorldEquipmentWorlds[world].Handler = handler;
}

uint8 Player::GetWorldEquipmentSlotCount(uint8 world)
{
    return (world == 0 || world > WORLD_CONTAINER_WORLD_COUNT) ? 0 : WorldEquipmentWorlds[world].SlotCount;
}

WorldEquipmentHandler* Player::GetWorldEquipmentHandler(uint8 world)
{
    return (world == 0 || world > WORLD_CONTAINER_WORLD_COUNT) ? nullptr : WorldEquipmentWorlds[world].Handler;
}

Bag* Player::_GetWorldContainer(uint8 block, uint8 world) const
{
    if (block >= MAX_WORLD_CONTAINER_BLOCK || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT)
        return nullptr;
    return m_worldContainers[block][world];
}

Bag* Player::_CreateWorldContainer(uint8 block, uint8 world)
{
    if (Bag* existing = _GetWorldContainer(block, world))
        return existing;
    if (block >= MAX_WORLD_CONTAINER_BLOCK || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT)
        return nullptr;

    // As Player::_CreateAccountBankVaults builds a vault: the vault template, which keeps the bag out of every save (Item::IsAccountBankVault)
    Bag* container = new Bag();
    if (!container->Create(GetWorldContainerGuidCounter(block, world), ACCOUNT_BANK_VAULT_ITEM_ENTRY, this))
    {
        LOG_ERROR("entities.player", "Player::_CreateWorldContainer: could not create the world container (item template {} missing?) for player {}", ACCOUNT_BANK_VAULT_ITEM_ENTRY, GetGUID().ToString());
        delete container;
        return nullptr;
    }

    container->SetBinding(true);
    container->SetContainer(nullptr);
    // Its own slot is no position any classifier accepts.  What it holds names it by its address (Bag::GetBagAddress, Item::GetBagSlot)
    container->SetSlot(WORLD_CONTAINER_OWN_SLOT);
    container->SetBagAddress(GetWorldContainerAddress(block, world));
    container->SetState(ITEM_UNCHANGED, this);
    m_worldContainers[block][world] = container;

    // What m_items gets from the player's world and client (Player::AddToWorld, BuildCreateUpdateBlockForPlayer) when it's built later
    if (IsInWorld())
    {
        container->AddToWorld();
        container->SendUpdateToPlayer(this);
    }
    return container;
}

std::vector<Bag*> Player::_GetWorldContainers() const
{
    std::vector<Bag*> containers;
    for (uint8 block = 0; block < MAX_WORLD_CONTAINER_BLOCK; ++block)
        for (uint8 world = 1; world <= WORLD_CONTAINER_WORLD_COUNT; ++world)
            if (m_worldContainers[block][world])
                containers.push_back(m_worldContainers[block][world]);
    return containers;
}

void Player::_DeleteWorldEquipment()
{
    for (uint8 block = 0; block < MAX_WORLD_CONTAINER_BLOCK; ++block)
    {
        for (uint8 world = 1; world <= WORLD_CONTAINER_WORLD_COUNT; ++world)
        {
            delete m_worldContainers[block][world];     // a Bag deletes what it holds
            m_worldContainers[block][world] = nullptr;
        }
    }
    for (uint8 world = 1; world <= WORLD_CONTAINER_WORLD_COUNT; ++world)
    {
        for (auto& setPair : m_worldEquipmentSets[world])
            for (Item* item : setPair.second.Items)
                delete item;
        m_worldEquipmentSets[world].clear();
    }
    for (Item* item : m_worldEquipmentRemovedItems)
        delete item;
    m_worldEquipmentRemovedItems.clear();
}

int8 Player::_GetWorldEquipmentSetBlock(uint8 world, uint32 key) const
{
    for (uint8 block = 0; block < MAX_WORLD_CONTAINER_BLOCK; ++block)
        if (WorldContainerBlocks[block].Kind == WORLD_CONTAINER_KIND_EQUIPMENT && m_worldContainerHasSet[block][world] && m_worldContainerSetKeys[block][world] == key)
            return int8(block);
    return -1;
}

bool Player::_IsWorldEquipmentSetUsable(uint8 world, uint8 slot) const
{
    return m_worldEquipmentLoaded && world != 0 && world <= WORLD_CONTAINER_WORLD_COUNT && slot < GetWorldEquipmentSlotCount(world);
}

Item* Player::GetWorldEquipmentSetItem(uint8 world, uint32 key, uint8 slot) const
{
    if (!_IsWorldEquipmentSetUsable(world, slot))
        return nullptr;
    int8 block = _GetWorldEquipmentSetBlock(world, key);
    if (block >= 0)
        return m_worldContainers[block][world]->GetItemByPos(slot);
    auto setItr = m_worldEquipmentSets[world].find(key);
    return setItr == m_worldEquipmentSets[world].end() ? nullptr : setItr->second.Items[slot];
}

Item* Player::GetWorldEquipmentItem(uint8 world, uint8 slot) const
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? GetWorldEquipmentSetItem(world, key, slot) : nullptr;
}

Item* Player::GetWorldEquipmentItemByGuid(uint8 world, ObjectGuid guid) const
{
    uint32 key;
    if (!GetWorldEquipmentSet(world, key))
        return nullptr;
    for (uint8 slot = 0; slot < GetWorldEquipmentSlotCount(world); ++slot)
        if (Item* item = GetWorldEquipmentSetItem(world, key, slot))
            if (item->GetGUID() == guid)
                return item;
    return nullptr;
}

bool Player::GetWorldEquipmentSet(uint8 world, uint32& key) const
{
    if (!m_worldEquipmentLoaded || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT || !m_worldContainerHasSet[WORLD_CONTAINER_EQUIPMENT][world])
        return false;
    key = m_worldContainerSetKeys[WORLD_CONTAINER_EQUIPMENT][world];
    return true;
}

bool Player::GetStagedWorldEquipmentSet(uint8 world, uint32& key) const
{
    if (!m_worldEquipmentLoaded || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT || !m_worldContainerHasSet[WORLD_CONTAINER_EQUIPMENT_STAGING][world])
        return false;
    key = m_worldContainerSetKeys[WORLD_CONTAINER_EQUIPMENT_STAGING][world];
    return true;
}

std::vector<uint32> Player::GetWorldEquipmentSetKeys(uint8 world) const
{
    std::vector<uint32> keys;
    if (!m_worldEquipmentLoaded || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT)
        return keys;
    // a set in a container is the one in use or the staged one, not put away
    for (auto const& setPair : m_worldEquipmentSets[world])
        if (_GetWorldEquipmentSetBlock(world, setPair.first) < 0)
            keys.push_back(setPair.first);
    return keys;
}

// Takes the item out of a set's slot.  It comes back detached: in no slot, not in the world, not in the save queue, its state kept (a changed
// item is written with the put away sets, or when it's stored again)
Item* Player::_DetachWorldEquipmentItem(uint8 world, uint32 key, uint8 slot)
{
    int8 block = _GetWorldEquipmentSetBlock(world, key);
    if (block < 0)
    {
        auto setItr = m_worldEquipmentSets[world].find(key);
        if (setItr == m_worldEquipmentSets[world].end())
            return nullptr;
        Item* item = setItr->second.Items[slot];
        setItr->second.Items[slot] = nullptr;
        return item;
    }

    uint8 address = GetWorldContainerAddress(uint8(block), world);
    Item* item = GetItemByPos(address, slot);
    if (!item)
        return nullptr;
    RemoveItem(address, slot, true);
    item->RemoveFromUpdateQueueOf(this);
    if (item->IsInWorld())
    {
        item->RemoveFromWorld();
        item->DestroyForPlayer(this);
    }
    return item;
}

// Puts a detached item in a set's slot, which is empty.  In a container it's stored like any item; put away it waits for the next save
void Player::_AttachWorldEquipmentItem(uint8 world, uint32 key, uint8 slot, Item* item, bool isRowChanged)
{
    int8 block = _GetWorldEquipmentSetBlock(world, key);
    if (block < 0)
    {
        m_worldEquipmentSets[world][key].Items[slot] = item;
        if (isRowChanged && item->GetState() == ITEM_UNCHANGED)
            item->FSetState(ITEM_CHANGED);
        return;
    }

    // Kept out of the save queue while it's stored, then queued only when something about it needs writing
    ItemUpdateState state = item->GetState();
    ItemPosCountVec dest;
    dest.push_back(ItemPosCount((uint16(GetWorldContainerAddress(uint8(block), world)) << 8) | slot, item->GetCount()));
    bool wasQueueBlocked = m_itemUpdateQueueBlocked;
    m_itemUpdateQueueBlocked = true;
    Item* stored = StoreItem(dest, item, true);
    m_itemUpdateQueueBlocked = wasQueueBlocked;
    if (!stored)
        return;
    stored->FSetState(state);
    if (isRowChanged || state != ITEM_UNCHANGED)
        stored->SetState(ITEM_CHANGED, this);
}

// What the core checks before an item goes into a world's equipment, from wherever it is
InventoryResult Player::_CanPutInWorldEquipment(Item* item) const
{
    if (!item || item->GetOwnerGUID() != GetGUID())
        return EQUIP_ERR_ITEM_NOT_FOUND;
    if (item->IsBag())
        return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    // Loot release and the trade find items by GUID, which can't find world equipment (CanPlaceItemInAccountBank's reasons)
    if (GetLootGUID() == item->GetGUID() || item->IsInTrade())
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    return EQUIP_ERR_OK;
}

// A worn item leaving for the bags, or anywhere else a module sends it
InventoryResult Player::_CanTakeFromWorldEquipment(Item* item) const
{
    if (!item)
        return EQUIP_ERR_ITEM_NOT_FOUND;
    if (GetLootGUID() == item->GetGUID() || item->IsInTrade())
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    return EQUIP_ERR_OK;
}

// The set is created on first use: a set is a number the module picks
WorldEquipmentSet& Player::_GetOrAddWorldEquipmentSet(uint8 world, uint32 key)
{
    return m_worldEquipmentSets[world][key];
}

InventoryResult Player::_PutWorldContainerSet(uint8 block, uint8 world, uint32 key)
{
    if (m_worldContainerHasSet[block][world] && m_worldContainerSetKeys[block][world] == key)
        return EQUIP_ERR_OK;

    Bag* container = _CreateWorldContainer(block, world);
    if (!container)
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;

    _TakeWorldContainerSet(block, world);

    WorldEquipmentSet& set = _GetOrAddWorldEquipmentSet(world, key);
    Item* items[WORLD_CONTAINER_SIZE];
    for (uint8 slot = 0; slot < WORLD_CONTAINER_SIZE; ++slot)
    {
        items[slot] = set.Items[slot];
        set.Items[slot] = nullptr;
    }
    m_worldContainerSetKeys[block][world] = key;
    m_worldContainerHasSet[block][world] = true;
    for (uint8 slot = 0; slot < WORLD_CONTAINER_SIZE; ++slot)
        if (items[slot])
            _AttachWorldEquipmentItem(world, key, slot, items[slot], false);
    return EQUIP_ERR_OK;
}

void Player::_TakeWorldContainerSet(uint8 block, uint8 world)
{
    if (!m_worldContainerHasSet[block][world])
        return;

    uint32 key = m_worldContainerSetKeys[block][world];
    Item* items[WORLD_CONTAINER_SIZE];
    for (uint8 slot = 0; slot < WORLD_CONTAINER_SIZE; ++slot)
        items[slot] = _DetachWorldEquipmentItem(world, key, slot);
    m_worldContainerHasSet[block][world] = false;
    WorldEquipmentSet& set = _GetOrAddWorldEquipmentSet(world, key);
    for (uint8 slot = 0; slot < WORLD_CONTAINER_SIZE; ++slot)
        if (items[slot])
            set.Items[slot] = items[slot];  // same set, same slot: its row is unchanged
}

InventoryResult Player::ActivateWorldEquipmentSet(uint8 world, uint32 key)
{
    if (!_IsWorldEquipmentSetUsable(world, 0))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    uint32 stagedKey;
    if (GetStagedWorldEquipmentSet(world, stagedKey) && stagedKey == key)
        UnstageWorldEquipmentSet(world);
    return _PutWorldContainerSet(WORLD_CONTAINER_EQUIPMENT, world, key);
}

void Player::DeactivateWorldEquipmentSet(uint8 world)
{
    if (_IsWorldEquipmentSetUsable(world, 0))
        _TakeWorldContainerSet(WORLD_CONTAINER_EQUIPMENT, world);
}

InventoryResult Player::StageWorldEquipmentSet(uint8 world, uint32 key)
{
    if (!_IsWorldEquipmentSetUsable(world, 0))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    uint32 activeKey;
    if (GetWorldEquipmentSet(world, activeKey) && activeKey == key)
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;     // a set is in one container at most
    return _PutWorldContainerSet(WORLD_CONTAINER_EQUIPMENT_STAGING, world, key);
}

void Player::UnstageWorldEquipmentSet(uint8 world)
{
    if (_IsWorldEquipmentSetUsable(world, 0))
        _TakeWorldContainerSet(WORLD_CONTAINER_EQUIPMENT_STAGING, world);
}

void Player::DestroyWorldEquipmentSet(uint8 world, uint32 key)
{
    if (!_IsWorldEquipmentSetUsable(world, 0))
        return;

    int8 block = _GetWorldEquipmentSetBlock(world, key);
    for (uint8 slot = 0; slot < WORLD_CONTAINER_SIZE; ++slot)
    {
        Item* item = _DetachWorldEquipmentItem(world, key, slot);
        if (!item)
            continue;
        LOG_INFO("entities.player", "Player::DestroyWorldEquipmentSet: {} world {} set {} slot {}: item {} (entry {}) destroyed", GetName(), world, key, slot, item->GetGUID().GetCounter(), item->GetEntry());
        // Its rows go in the next save (_SaveWorldEquipment)
        m_worldEquipmentRemovedItems.push_back(item);
    }
    if (block >= 0)
        m_worldContainerHasSet[block][world] = false;
    m_worldEquipmentSets[world].erase(key);
}

InventoryResult Player::WearWorldEquipmentSetItem(uint8 world, uint32 key, uint8 slot, Item* item)
{
    if (!_IsWorldEquipmentSetUsable(world, slot))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (InventoryResult result = _CanPutInWorldEquipment(item); result != EQUIP_ERR_OK)
        return result;
    if (!IsCarriedPos(item->GetBagSlot(), item->GetSlot()) || GetItemByPos(item->GetBagSlot(), item->GetSlot()) != item)
        return EQUIP_ERR_ITEM_NOT_FOUND;
    if (GetWorldEquipmentSetItem(world, key, slot))
        return EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;

    _GetOrAddWorldEquipmentSet(world, key);
    _TakeFromBagsForWorldEquipment(item);
    _AttachWorldEquipmentItem(world, key, slot, item, true);
    return EQUIP_ERR_OK;
}

// An item leaves the carried bags for world equipment: from now on it's not inventory (counts, quest items, refunds, trades of soulbound loot)
void Player::_TakeFromBagsForWorldEquipment(Item* item)
{
    ItemRemovedQuestCheck(item->GetEntry(), item->GetCount());
    item->SetNotRefundable(this, false);
    item->ClearSoulboundTradeable(this);
    RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
    UpdateTitansGrip();
    item->RemoveFromUpdateQueueOf(this);
    if (item->IsInWorld())
    {
        item->RemoveFromWorld();
        item->DestroyForPlayer(this);
    }
    if (item->GetState() == ITEM_UNCHANGED)
        item->FSetState(ITEM_CHANGED);
}

// A detached item goes to the carried bags at dest (from CanStoreItem)
Item* Player::_StoreFromWorldEquipment(ItemPosCountVec const& dest, Item* item)
{
    uint32 entry = item->GetEntry();
    uint32 count = item->GetCount();
    Item* stored = StoreItem(dest, item, true);     // stores, sends, queues for the save (which deletes its world equipment row)
    ItemAddedQuestCheck(entry, count);
    if (stored && stored == item && stored->IsBOPTradable())
        AddTradeableItem(stored);
    return stored;
}

// Where in the carried bags an item fits: one place, anywhere in one bag (bagSlot NULL_SLOT), or anywhere at all (bag NULL_BAG)
InventoryResult Player::_CanStoreInCarriedBags(uint8 bag, uint8 bagSlot, ItemPosCountVec& dest, Item* item, bool swap) const
{
    dest.clear();
    if (bag != NULL_BAG)
    {
        if (bagSlot != NULL_SLOT ? !IsCarriedPos(bag, bagSlot) : (bag != INVENTORY_SLOT_BAG_0 && !IsCarriedPos(bag, 0)))
            return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
        return CanStoreItem(bag, bagSlot, dest, item, swap);
    }

    // The backpack, then each bag.  Not CanStoreItem's own search, which also reaches the keyring and the currency slots
    InventoryResult result = CanStoreItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, item, false);
    for (uint8 bagSlotNumber = INVENTORY_SLOT_BAG_START; result != EQUIP_ERR_OK && bagSlotNumber < INVENTORY_SLOT_BAG_END; ++bagSlotNumber)
    {
        if (!GetBagByPos(bagSlotNumber))
            continue;
        dest.clear();
        result = CanStoreItem(bagSlotNumber, NULL_SLOT, dest, item, false);
    }
    return result == EQUIP_ERR_OK ? result : EQUIP_ERR_INVENTORY_FULL;
}

InventoryResult Player::TakeOffWorldEquipmentSetItem(uint8 world, uint32 key, uint8 slot, uint8 bag, uint8 bagSlot)
{
    if (!_IsWorldEquipmentSetUsable(world, slot))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    Item* item = GetWorldEquipmentSetItem(world, key, slot);
    if (InventoryResult result = _CanTakeFromWorldEquipment(item); result != EQUIP_ERR_OK)
        return result;

    ItemPosCountVec dest;
    InventoryResult result = _CanStoreInCarriedBags(bag, bagSlot, dest, item, false);
    if (result != EQUIP_ERR_OK)
        return result;

    item = _DetachWorldEquipmentItem(world, key, slot);
    _StoreFromWorldEquipment(dest, item);
    return EQUIP_ERR_OK;
}

InventoryResult Player::SwapWorldEquipmentSetItem(uint8 world, uint32 key, uint8 slot, Item* item)
{
    if (!_IsWorldEquipmentSetUsable(world, slot))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (InventoryResult result = _CanPutInWorldEquipment(item); result != EQUIP_ERR_OK)
        return result;
    uint8 bag = item->GetBagSlot();
    uint8 bagSlot = item->GetSlot();
    if (!IsCarriedPos(bag, bagSlot) || GetItemByPos(bag, bagSlot) != item)
        return EQUIP_ERR_ITEM_NOT_FOUND;
    Item* worn = GetWorldEquipmentSetItem(world, key, slot);
    if (!worn)
        return WearWorldEquipmentSetItem(world, key, slot, item);
    if (InventoryResult result = _CanTakeFromWorldEquipment(worn); result != EQUIP_ERR_OK)
        return result;

    // The worn item goes where the bag item is, so it never needs free room.  Checked before anything moves (a quiver takes only ammo)
    ItemPosCountVec dest;
    InventoryResult result = CanStoreItem(bag, bagSlot, dest, worn, true);
    if (result != EQUIP_ERR_OK)
        return result;

    _TakeFromBagsForWorldEquipment(item);
    worn = _DetachWorldEquipmentItem(world, key, slot);
    _AttachWorldEquipmentItem(world, key, slot, item, true);
    _StoreFromWorldEquipment(dest, worn);
    return EQUIP_ERR_OK;
}

InventoryResult Player::MoveWorldEquipmentSetItem(uint8 world, uint32 fromKey, uint8 fromSlot, uint32 toKey, uint8 toSlot)
{
    if (!_IsWorldEquipmentSetUsable(world, fromSlot) || !_IsWorldEquipmentSetUsable(world, toSlot))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (fromKey == toKey && fromSlot == toSlot)
        return EQUIP_ERR_OK;
    Item* item = GetWorldEquipmentSetItem(world, fromKey, fromSlot);
    if (InventoryResult result = _CanTakeFromWorldEquipment(item); result != EQUIP_ERR_OK)
        return result;
    Item* other = GetWorldEquipmentSetItem(world, toKey, toSlot);
    if (other)
        if (InventoryResult result = _CanTakeFromWorldEquipment(other); result != EQUIP_ERR_OK)
            return result;

    _GetOrAddWorldEquipmentSet(world, toKey);
    item = _DetachWorldEquipmentItem(world, fromKey, fromSlot);
    other = other ? _DetachWorldEquipmentItem(world, toKey, toSlot) : nullptr;
    _AttachWorldEquipmentItem(world, toKey, toSlot, item, true);
    if (other)
        _AttachWorldEquipmentItem(world, fromKey, fromSlot, other, true);
    return EQUIP_ERR_OK;
}

Item* Player::RemoveWorldEquipmentSetItem(uint8 world, uint32 key, uint8 slot, CharacterDatabaseTransaction trans)
{
    if (!_IsWorldEquipmentSetUsable(world, slot))
        return nullptr;
    Item* item = GetWorldEquipmentSetItem(world, key, slot);
    if (_CanTakeFromWorldEquipment(item) != EQUIP_ERR_OK)
        return nullptr;

    item = _DetachWorldEquipmentItem(world, key, slot);
    // In the caller's transaction, so whenever the server stops the item is in one place: here, or wherever the caller puts it (a mail)
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_WORLD_ITEM_BY_ITEM);
    stmt->SetData(0, item->GetGUID().GetCounter());
    trans->Append(stmt);
    Item::DeleteFromInventoryDB(trans, item->GetGUID().GetCounter());
    return item;
}

InventoryResult Player::PutWorldEquipmentSetItem(uint8 world, uint32 key, uint8 slot, Item* item)
{
    if (!_IsWorldEquipmentSetUsable(world, slot))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (!item || item->IsInWorld() || item->GetContainer() || (item->GetSlot() != NULL_SLOT && GetItemByPos(item->GetBagSlot(), item->GetSlot()) == item))
        return EQUIP_ERR_ITEM_NOT_FOUND;    // only an item in no slot
    if (item->GetOwnerGUID() != GetGUID())
        item->SetOwnerGUID(GetGUID());
    if (InventoryResult result = _CanPutInWorldEquipment(item); result != EQUIP_ERR_OK)
        return result;
    if (GetWorldEquipmentSetItem(world, key, slot))
        return EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;

    item->SetSlot(NULL_SLOT);
    item->SetContainer(nullptr);
    _GetOrAddWorldEquipmentSet(world, key);
    _AttachWorldEquipmentItem(world, key, slot, item, true);
    return EQUIP_ERR_OK;
}

// The set in use, for the calls a module makes most
InventoryResult Player::WearWorldEquipmentItem(uint8 world, uint8 slot, Item* item)
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? WearWorldEquipmentSetItem(world, key, slot, item) : EQUIP_ERR_CANT_DO_RIGHT_NOW;
}

InventoryResult Player::SwapWorldEquipmentItem(uint8 world, uint8 slot, Item* item)
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? SwapWorldEquipmentSetItem(world, key, slot, item) : EQUIP_ERR_CANT_DO_RIGHT_NOW;
}

InventoryResult Player::TakeOffWorldEquipmentItem(uint8 world, uint8 slot, uint8 bag, uint8 bagSlot)
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? TakeOffWorldEquipmentSetItem(world, key, slot, bag, bagSlot) : EQUIP_ERR_CANT_DO_RIGHT_NOW;
}

InventoryResult Player::MoveWorldEquipmentItem(uint8 world, uint8 fromSlot, uint8 toSlot)
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? MoveWorldEquipmentSetItem(world, key, fromSlot, key, toSlot) : EQUIP_ERR_CANT_DO_RIGHT_NOW;
}

Item* Player::RemoveWorldEquipmentItem(uint8 world, uint8 slot, CharacterDatabaseTransaction trans)
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? RemoveWorldEquipmentSetItem(world, key, slot, trans) : nullptr;
}

InventoryResult Player::PutWorldEquipmentItem(uint8 world, uint8 slot, Item* item)
{
    uint32 key;
    return GetWorldEquipmentSet(world, key) ? PutWorldEquipmentSetItem(world, key, slot, item) : EQUIP_ERR_CANT_DO_RIGHT_NOW;
}

// The set a world container holds, as its block and key, for a drag the client made on it
bool Player::_GetWorldContainerDragSet(uint8 bag, uint8 slot, uint8& block, uint8& world, uint32& key) const
{
    if (!GetWorldContainerOfAddress(bag, block, world) || !_IsWorldEquipmentSetUsable(world, slot) || !m_worldContainers[block][world]
        || !m_worldContainerHasSet[block][world] || !GetWorldEquipmentHandler(world))
        return false;
    key = m_worldContainerSetKeys[block][world];
    return true;
}

// CMSG_SWAP_ITEM with a world container on either side.  The core checks only what it owns; the world's module decides and moves (4.7)
void Player::HandleClientWorldEquipmentSwap(uint8 srcBag, uint8 srcSlot, uint8 dstBag, uint8 dstSlot)
{
    LOG_DEBUG("entities.player.items", "Player::HandleClientWorldEquipmentSwap: {} from ({}, {}) to ({}, {})", GetName(), srcBag, srcSlot, dstBag, dstSlot);
    Item* srcItem = GetItemByPos(srcBag, srcSlot);
    Item* dstItem = GetItemByPos(dstBag, dstSlot);
    InventoryResult result = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    uint8 srcBlock = 0, srcWorld = 0, dstBlock = 0, dstWorld = 0;
    uint32 srcKey = 0, dstKey = 0;
    bool isSrcWorld = IsWorldContainerAddress(srcBag);
    bool isDstWorld = IsWorldContainerAddress(dstBag);

    if (!srcItem)
        result = EQUIP_ERR_ITEM_NOT_FOUND;
    else if (isSrcWorld && !_GetWorldContainerDragSet(srcBag, srcSlot, srcBlock, srcWorld, srcKey))
        result = EQUIP_ERR_ITEM_NOT_FOUND;
    else if (isDstWorld && !_GetWorldContainerDragSet(dstBag, dstSlot, dstBlock, dstWorld, dstKey))
        result = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    else if (isSrcWorld && isDstWorld)
    {
        if (srcWorld != dstWorld)
            result = EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;
        else if (_CanTakeFromWorldEquipment(srcItem) == EQUIP_ERR_OK && (!dstItem || _CanTakeFromWorldEquipment(dstItem) == EQUIP_ERR_OK))
            result = GetWorldEquipmentHandler(srcWorld)->OnClientMove(this, srcWorld, srcBlock, srcSlot, dstBlock, dstSlot);
        else
            result = EQUIP_ERR_CANT_DO_RIGHT_NOW;
    }
    else if (isDstWorld)
    {
        // A bag item dropped on an equipment slot: only from the carried bags
        if (!IsCarriedPos(srcBag, srcSlot))
            result = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
        else if ((result = _CanPutInWorldEquipment(srcItem)) == EQUIP_ERR_OK)
            result = GetWorldEquipmentHandler(dstWorld)->OnClientWear(this, dstWorld, dstBlock, dstSlot, srcItem);
    }
    else
    {
        // A worn item dropped on a place in the carried bags
        if (!IsCarriedPos(dstBag, dstSlot) || (dstBag != INVENTORY_SLOT_BAG_0 && (!GetBagByPos(dstBag) || dstSlot >= GetBagByPos(dstBag)->GetBagSize())))
            result = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
        else if ((result = _CanTakeFromWorldEquipment(srcItem)) == EQUIP_ERR_OK && (!dstItem || (result = _CanPutInWorldEquipment(dstItem)) == EQUIP_ERR_OK))
            result = GetWorldEquipmentHandler(srcWorld)->OnClientTakeOff(this, srcWorld, srcBlock, srcSlot, dstBag, dstSlot);
    }

    if (result != EQUIP_ERR_OK)
        SendEquipError(result, srcItem, dstItem);
}

// CMSG_AUTOSTORE_BAG_ITEM with a world container as the source: a worn item dropped on a bag's button or the backpack's
void Player::HandleClientWorldEquipmentAutoStore(uint8 srcBag, uint8 srcSlot, uint8 dstBag)
{
    LOG_DEBUG("entities.player.items", "Player::HandleClientWorldEquipmentAutoStore: {} from ({}, {}) to bag {}", GetName(), srcBag, srcSlot, dstBag);
    Item* srcItem = GetItemByPos(srcBag, srcSlot);
    InventoryResult result = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    uint8 block = 0, world = 0;
    uint32 key = 0;

    if (!srcItem || !IsWorldContainerAddress(srcBag) || !_GetWorldContainerDragSet(srcBag, srcSlot, block, world, key))
        result = EQUIP_ERR_ITEM_NOT_FOUND;
    else if (dstBag != INVENTORY_SLOT_BAG_0 && !(dstBag >= INVENTORY_SLOT_BAG_START && dstBag < INVENTORY_SLOT_BAG_END && GetBagByPos(dstBag)))
        result = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    else if ((result = _CanTakeFromWorldEquipment(srcItem)) == EQUIP_ERR_OK)
        result = GetWorldEquipmentHandler(world)->OnClientTakeOff(this, world, block, srcSlot, dstBag, NULL_SLOT);

    if (result != EQUIP_ERR_OK)
        SendEquipError(result, srcItem, nullptr);
}

void Player::_LoadWorldEquipment(PreparedQueryResult result, uint32 timeDiff)
{
    m_worldEquipmentLoaded = false;
    if (result)
    {
        uint32 zoneId = GetZoneId();
        std::list<Item*> problematicItems;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        m_itemUpdateQueueBlocked = true;
        do
        {
            Field* fields = result->Fetch();
            uint8 world = fields[11].Get<uint8>();
            uint8 slot = fields[12].Get<uint8>();
            ObjectGuid::LowType itemGuidLow = fields[13].Get<uint32>();
            uint8 kind = fields[15].Get<uint8>();
            uint32 key = fields[16].Get<uint32>();

            // A kind this server doesn't know, or a world that's off, keeps its rows as they are: its item templates may not exist, and every
            // path below can delete a row or an item
            if (kind != WORLD_CONTAINER_KIND_EQUIPMENT || world == 0 || world > WORLD_CONTAINER_WORLD_COUNT || !GetWorldEquipmentHandler(world))
                continue;

            // An item is in one place only: already loaded from this character's inventory, mail or account bank, the row is stale
            ObjectGuid itemGuid = ObjectGuid::Create<HighGuid::Item>(itemGuidLow);
            if (GetItemByGuid(itemGuid) || GetMItem(itemGuidLow))
            {
                LOG_ERROR("entities.player", "Player::_LoadWorldEquipment: player {} has item {} in world {} equipment that is also elsewhere. Removing the world equipment row.",
                          GetName(), itemGuid.ToString(), world);
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_WORLD_ITEM_BY_ITEM);
                stmt->SetData(0, itemGuidLow);
                trans->Append(stmt);
                continue;
            }

            Item* item = _LoadItem(trans, zoneId, timeDiff, fields);
            if (!item)
            {
                // gone for good (broken, holiday over, zone limited...): _LoadItem deleted the item, and its row goes with it
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_WORLD_ITEM_BY_ITEM);
                stmt->SetData(0, itemGuidLow);
                trans->Append(stmt);
                continue;
            }

            WorldEquipmentSet& set = m_worldEquipmentSets[world][key];
            if (slot >= GetWorldEquipmentSlotCount(world) || set.Items[slot] || item->IsBag())
            {
                LOG_ERROR("entities.player", "Player::_LoadWorldEquipment: player {} has item ({}, entry: {}) at an unusable world {} equipment place (set {}, slot {}). Item will be sent by mail.",
                          GetName(), item->GetGUID().ToString(), item->GetEntry(), world, key, slot);
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_WORLD_ITEM_BY_ITEM);
                stmt->SetData(0, itemGuidLow);
                trans->Append(stmt);
                item->DeleteFromInventoryDB(trans);
                problematicItems.push_back(item);
                continue;
            }

            // put away as loaded: nothing to save.  Worn items are never refundable or trade-back loot (_TakeFromBagsForWorldEquipment);
            // _LoadItem may have listed one that somehow is, and the clear is saved
            item->SetContainer(nullptr);
            item->SetSlot(NULL_SLOT);
            item->FSetState(ITEM_UNCHANGED);
            if (item->IsRefundable() || item->IsBOPTradable())
            {
                item->SetNotRefundable(this, false);
                item->ClearSoulboundTradeable(this);
                item->FSetState(ITEM_CHANGED);
            }
            set.Items[slot] = item;
        } while (result->NextRow());
        m_itemUpdateQueueBlocked = false;

        while (!problematicItems.empty())
        {
            std::string subject = GetSession()->GetAcoreString(LANG_NOT_EQUIPPED_ITEM);

            MailDraft draft(subject, "There were problems with worn item(s).");
            for (uint8 i = 0; !problematicItems.empty() && i < MAX_MAIL_ITEMS; ++i)
            {
                draft.AddItem(problematicItems.front());
                problematicItems.pop_front();
            }
            draft.SendMailTo(trans, this, MailSender(this, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
        }
        CharacterDatabase.CommitTransaction(trans);
    }
    m_worldEquipmentLoaded = true;
}

// Where an item in a world container is saved: its world, the set the container holds, and the slot
bool Player::_GetWorldEquipmentDBPosition(Item const* item, uint8& world, uint32& key, uint8& slot) const
{
    uint8 block;
    if (!GetWorldContainerOfAddress(item->GetBagSlot(), block, world) || !m_worldContainerHasSet[block][world])
        return false;
    key = m_worldContainerSetKeys[block][world];
    slot = item->GetSlot();
    return true;
}

// The sets put away, and what was destroyed while put away.  Part of _SaveInventory's transaction
void Player::_SaveWorldEquipment(CharacterDatabaseTransaction trans)
{
    CharacterDatabasePreparedStatement* stmt = nullptr;
    for (uint8 world = 1; world <= WORLD_CONTAINER_WORLD_COUNT; ++world)
    {
        for (auto& setPair : m_worldEquipmentSets[world])
        {
            for (uint8 slot = 0; slot < WORLD_CONTAINER_SIZE; ++slot)
            {
                Item* item = setPair.second.Items[slot];
                if (!item || item->GetState() == ITEM_UNCHANGED)
                    continue;

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_CHAR_WORLD_ITEM);
                stmt->SetData(0, GetGUID().GetCounter());
                stmt->SetData(1, uint8(WORLD_CONTAINER_KIND_EQUIPMENT));
                stmt->SetData(2, world);
                stmt->SetData(3, setPair.first);
                stmt->SetData(4, slot);
                stmt->SetData(5, item->GetGUID().GetCounter());
                trans->Append(stmt);

                Item::DeleteFromInventoryDB(trans, item->GetGUID().GetCounter());

                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM_BY_ITEM);
                stmt->SetData(0, item->GetGUID().GetCounter());
                trans->Append(stmt);

                item->SaveToDB(trans);
            }
        }
    }

    for (Item* item : m_worldEquipmentRemovedItems)
    {
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_WORLD_ITEM_BY_ITEM);
        stmt->SetData(0, item->GetGUID().GetCounter());
        trans->Append(stmt);
        Item::DeleteFromInventoryDB(trans, item->GetGUID().GetCounter());
        item->FSetState(ITEM_REMOVED);
        item->SaveToDB(trans);                              // deletes the item_instance row and the item
    }
    m_worldEquipmentRemovedItems.clear();
}
