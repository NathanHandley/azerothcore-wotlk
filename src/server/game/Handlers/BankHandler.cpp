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

#include "BankPackets.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

bool WorldSession::CanUseBank(ObjectGuid bankerGUID) const
{
    // bankerGUID parameter is optional, set to 0 by default.
    if (!bankerGUID)
        bankerGUID = m_currentBankerGUID;

    bool isUsingBankCommand = (bankerGUID == GetPlayer()->GetGUID() && bankerGUID == m_currentBankerGUID);

    if (!isUsingBankCommand)
    {
        Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(bankerGUID, UNIT_NPC_FLAG_BANKER);
        if (!creature)
            return false;
    }

    return true;
}

bool WorldSession::IsBankPosOutOfReach(uint8 bag, uint8 slot) const
{
    return bag != NULL_BAG && Player::IsBankPos(bag, slot) && !CanUseBank();
}

void WorldSession::HandleBankerActivateOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;

    recvData >> guid;

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER);
    if (!unit)
    {
        LOG_DEBUG("network", "WORLD: HandleBankerActivateOpcode - Unit ({}) not found or you can not interact with him.", guid.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    SendShowBank(guid);
}

void WorldSession::HandleAutoBankItemOpcode(WorldPackets::Bank::AutoBankItem& packet)
{
    LOG_DEBUG("network", "STORAGE: receive bag = {}, slot = {}", packet.Bag, packet.Slot);

    if (!CanUseBank())
    {
        LOG_DEBUG("network", "WORLD: HandleAutoBankItemOpcode - Unit ({}) not found or you can't interact with him.", m_currentBankerGUID.ToString());
        return;
    }

    Item* item = _player->GetItemByPos(packet.Bag, packet.Slot);
    if (!item)
        return;

    // MergedWorlds account bank: with the account bank tab open the item goes there instead
    ItemPosCountVec dest;
    InventoryResult msg = packet.AccountBank ? _player->CanAccountBankItem(NULL_BAG, NULL_SLOT, dest, item, false) : _player->CanBankItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (msg != EQUIP_ERR_OK)
    {
        _player->SendEquipError(msg, item, nullptr);
        return;
    }

    if (dest.size() == 1 && dest[0].pos == item->GetPos())
    {
        _player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);
        return;
    }

    _player->RemoveItem(packet.Bag, packet.Slot, true);
    _player->ItemRemovedQuestCheck(item->GetEntry(), item->GetCount());
    _player->BankItem(dest, item, true);
    _player->UpdateTitansGrip();
}

void WorldSession::HandleAutoStoreBankItemOpcode(WorldPackets::Bank::AutoStoreBankItem& packet)
{
    LOG_DEBUG("network", "STORAGE: receive bag = {}, slot = {}", packet.Bag, packet.Slot);

    if (!CanUseBank())
    {
        LOG_DEBUG("network", "WORLD: HandleAutoStoreBankItemOpcode - Unit ({}) not found or you can't interact with him.", m_currentBankerGUID.ToString());
        return;
    }

    Item* item = _player->GetItemByPos(packet.Bag, packet.Slot);
    if (!item)
        return;

    if (_player->IsBankPos(packet.Bag, packet.Slot))                    // moving from bank to inventory
    {
        ItemPosCountVec dest;
        InventoryResult msg = _player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (msg != EQUIP_ERR_OK)
        {
            _player->SendEquipError(msg, item, nullptr);
            return;
        }

        _player->RemoveItem(packet.Bag, packet.Slot, true);
        if (Item const* storedItem = _player->StoreItem(dest, item, true))
            _player->ItemAddedQuestCheck(storedItem->GetEntry(), storedItem->GetCount());
    }
    else                                                    // moving from inventory to bank
    {
        ItemPosCountVec dest;
        InventoryResult msg = _player->CanBankItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (msg != EQUIP_ERR_OK)
        {
            _player->SendEquipError(msg, item, nullptr);
            return;
        }

        _player->RemoveItem(packet.Bag, packet.Slot, true);
        _player->ItemRemovedQuestCheck(item->GetEntry(), item->GetCount());
        _player->BankItem(dest, item, true);
        _player->UpdateTitansGrip();
    }
}

void WorldSession::HandleBuyBankSlotOpcode(WorldPackets::Bank::BuyBankSlot& buyBankSlot)
{
    WorldPackets::Bank::BuyBankSlotResult packet;
    if (!CanUseBank(buyBankSlot.Banker))
    {
        packet.Result = ERR_BANKSLOT_NOTBANKER;
        SendPacket(packet.Write());
        LOG_DEBUG("network", "WORLD: HandleBuyBankSlotOpcode - {} not found or you can't interact with him.", buyBankSlot.Banker.ToString());
        return;
    }

    // MergedWorlds bank expansion: 24 bag slots, for the character and (bought separately, same prices) for the account bank
    bool accountBank = buyBankSlot.AccountBank != 0;
    if (accountBank && !_player->IsAccountBankLoaded())
    {
        packet.Result = ERR_BANKSLOT_FAILED_TOO_MANY;
        SendPacket(packet.Write());
        return;
    }

    uint32 slot = accountBank ? _player->GetAccountBankBagSlotCount() : _player->GetBankBagSlotCount();

    // next slot
    ++slot;

    BankBagSlotPricesEntry const* slotEntry = sBankBagSlotPricesStore.LookupEntry(slot);

    if (!slotEntry || slot > uint32(accountBank ? MAX_ACCOUNT_BANK_BAG_SLOT_COUNT : MAX_BANK_BAG_SLOT_COUNT))
    {
        packet.Result = ERR_BANKSLOT_FAILED_TOO_MANY;
        SendPacket(packet.Write());
        return;
    }

    uint32 price = slotEntry->price;

    if (!_player->HasEnoughMoney(price))
    {
        packet.Result = ERR_BANKSLOT_INSUFFICIENT_FUNDS;
        SendPacket(packet.Write());
        return;
    }

    if (accountBank)
        _player->SetAccountBankBagSlotCount(slot);
    else
        _player->SetBankBagSlotCount(slot);
    _player->ModifyMoney(-int32(price));

    // MergedWorlds account bank: the slot belongs to every character on the account, so it is written right away
    // together with the money, instead of waiting for the next save of this character
    if (accountBank)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        _player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    packet.Result = ERR_BANKSLOT_OK;
    SendPacket(packet.Write());

    if (!accountBank)
        _player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BUY_BANK_SLOT);
}

void WorldSession::SendShowBank(ObjectGuid guid)
{
    m_currentBankerGUID = guid;
    WorldPackets::Bank::ShowBank packet;
    packet.Banker = guid;
    SendPacket(packet.Write());
}
