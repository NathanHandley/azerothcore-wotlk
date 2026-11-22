-- MergedWorlds world containers (E:\MW\SHARED.md): the items a character keeps in a game world's hidden containers, first its worn equipment.
-- One row per item: the world (SHARED.md's world ids), the set (one per character slot of that game, a number its module picks), and the slot.
-- A kind other than 0 is for later kinds of world container (a game's keyring), so they need no table of their own.  Additive: nothing else
-- changes, and a server from before this never reads it.  Safe to run again.

CREATE TABLE IF NOT EXISTS `character_world_item` (
  `guid` int unsigned NOT NULL COMMENT 'Character',
  `kind` tinyint unsigned NOT NULL DEFAULT '0' COMMENT '0 equipment',
  `world` tinyint unsigned NOT NULL,
  `set_key` int unsigned NOT NULL DEFAULT '0',
  `slot` tinyint unsigned NOT NULL,
  `item` int unsigned NOT NULL COMMENT 'item_instance guid',
  PRIMARY KEY (`item`),
  UNIQUE KEY `guid_kind_world_set_slot` (`guid`,`kind`,`world`,`set_key`,`slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='MergedWorlds world containers: equipment, later kinds';
