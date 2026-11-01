-- MergedWorlds world bank: `account_bank_item` gains the tab a row is on.  World 0 is the account bank as it was, 1 is Ragnarok, 2 is City of Heroes.
-- A world's row always has bag 0 and slot 0-143, the item slots of its tab (Player::_LoadAccountBank).  The rows of a world that's off stay in the
-- table, and no load or save looks at them.  Inventory slots 237-244 are appended, so `character_inventory` doesn't move.
-- Safe to run again: the column is only added while it isn't there.

SET @worldBankColumnCount = (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'account_bank_item' AND COLUMN_NAME = 'world');

SET @worldBankStatement = IF(@worldBankColumnCount = 0,
    'ALTER TABLE `account_bank_item` ADD COLUMN `world` tinyint unsigned NOT NULL DEFAULT ''0'' COMMENT ''0 = the account bank, otherwise the world of a bank tab'' AFTER `account`, DROP INDEX `account_bag_slot`, ADD UNIQUE KEY `account_world_bag_slot` (`account`,`world`,`bag`,`slot`)',
    'SELECT 1');

PREPARE worldBankPrepared FROM @worldBankStatement;
EXECUTE worldBankPrepared;
DEALLOCATE PREPARE worldBankPrepared;
