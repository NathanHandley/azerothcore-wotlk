-- Add some Jeeves drops
DELETE FROM `creature_loot_template` WHERE `Item` = 49050;
INSERT INTO `creature_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (29724, 49050, 0, 0.2, 0, 1, 0, 1, 1, 'Library Guardian - Schematic: Jeeves');
INSERT INTO `creature_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (33432, 49050, 0, 0.9, 0, 1, 0, 1, 1, 'Leviathan Mk II - Schematic: Jeeves');
INSERT INTO `creature_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (33113, 49050, 0, 0.4, 0, 1, 0, 1, 1, 'Flame Leviathan - Schematic: Jeeves');