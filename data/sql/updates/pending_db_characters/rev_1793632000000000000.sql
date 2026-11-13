-- MergedWorlds world bank: `account_bank` keeps one purchased count per bank tab, so its key gains the tab's world.  World 0 is the Account tab as it
-- was, 1 is Ragnarok, 2 is City of Heroes.  A world's tab now has the Account tab's shape, so `account_bank_item` rows of a world work as world 0's do: bag 0 with
-- slot 0-71 the item slots and 72-95 the bag slots, or the guid of the bag that holds the item.  Every existing row stays as it is, and a world that's off keeps its
-- rows unread.  Inventory slots 241-244 are gone, which `character_inventory` never used.
-- Safe to run again: the column is only added while it isn't there.

SET @worldBankColumnCount = (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'account_bank' AND COLUMN_NAME = 'world');

SET @worldBankStatement = IF(@worldBankColumnCount = 0,
    'ALTER TABLE `account_bank` ADD COLUMN `world` tinyint unsigned NOT NULL DEFAULT ''0'' COMMENT ''0 = the account bank, otherwise the world of a bank tab'' AFTER `account`, DROP PRIMARY KEY, ADD PRIMARY KEY (`account`,`world`)',
    'SELECT 1');

PREPARE worldBankPrepared FROM @worldBankStatement;
EXECUTE worldBankPrepared;
DEALLOCATE PREPARE worldBankPrepared;
