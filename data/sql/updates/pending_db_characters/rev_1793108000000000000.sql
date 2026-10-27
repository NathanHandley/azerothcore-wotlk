-- MergedWorlds bank expansion: the bank grows to 72 item slots (12x6) and 24 bag slots (2x12), and every inventory slot range
-- after it moves up (bank bags 67-73 -> 111-117, keyring 86-117 -> 147-178, currency tokens 118-149 -> 179-210).  Adds the
-- account bank: `account_bank` (purchased bag slots) and `account_bank_item` (bag 0 = the account bank itself, slot 0-71 item
-- slots and 72-95 bag slots; otherwise the item guid of the account bank bag holding it).
-- Safe to run again: the slot move only happens while `character_inventory_slot_layout` doesn't say version 2, and that marker is
-- written in the same transaction as the move.  worldserver refuses to start until the marker is there.

CREATE TABLE IF NOT EXISTS `account_bank` (
  `account` int unsigned NOT NULL COMMENT 'Account Identifier',
  `bagSlots` tinyint unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='MergedWorlds account bank';

CREATE TABLE IF NOT EXISTS `account_bank_item` (
  `account` int unsigned NOT NULL COMMENT 'Account Identifier',
  `bag` int unsigned NOT NULL DEFAULT '0',
  `slot` tinyint unsigned NOT NULL DEFAULT '0',
  `item` int unsigned NOT NULL DEFAULT '0' COMMENT 'Item Global Unique Identifier',
  PRIMARY KEY (`item`),
  UNIQUE KEY `account_bag_slot` (`account`,`bag`,`slot`),
  KEY `idx_account` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='MergedWorlds account bank';

CREATE TABLE IF NOT EXISTS `character_inventory_slot_layout` (
  `id` tinyint unsigned NOT NULL DEFAULT '0',
  `version` int unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='MergedWorlds inventory slot layout version (2 = bank expansion)';

START TRANSACTION;

SELECT IFNULL(MAX(`version`), 1) INTO @inventoryLayoutVersion FROM `character_inventory_slot_layout` WHERE `id` = 0 FOR UPDATE;

-- Highest slots first so no row lands on a slot another row has not left yet
UPDATE `character_inventory`
SET `slot` = CASE
    WHEN `slot` BETWEEN 67 AND 73 THEN `slot` + 44
    ELSE `slot` + 61
END
WHERE @inventoryLayoutVersion < 2 AND `bag` = 0 AND (`slot` BETWEEN 67 AND 73 OR `slot` BETWEEN 86 AND 149)
ORDER BY `slot` DESC;

INSERT INTO `character_inventory_slot_layout` (`id`, `version`) VALUES (0, 2) ON DUPLICATE KEY UPDATE `version` = GREATEST(`version`, 2);

COMMIT;
