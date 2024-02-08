-- Add red winter hat to the 3 npcs that had green winter hats
DELETE FROM creature_loot_template WHERE Entry = 2764 AND Item = 21524;
DELETE FROM creature_loot_template WHERE Entry = 2765 AND Item = 21524;
DELETE FROM creature_loot_template WHERE Entry = 4063 AND Item = 21524;
INSERT INTO `creature_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (2764, 21524, 0, 0.5, 0, 1, 0, 1, 1, 'Sleeby - Red Winter Hat');
INSERT INTO `creature_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (2765, 21524, 0, 0.5, 0, 1, 0, 1, 1, 'Znort - Red Winter Hat');
INSERT INTO `creature_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (4063, 21524, 0, 0.5, 0, 1, 0, 1, 1, 'Feeboz - Red Winter Hat');