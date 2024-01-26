-- Increase rates of Rainbow Leather
DELETE FROM `skinning_loot_template` WHERE `Item` = 81012;
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (28860, 81012, 0, 3, 0, 1, 1, 1, 1, 'Sartharion - Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (32517, 81012, 0, 3, 0, 1, 1, 1, 1, 'Loque\'nahak - Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (34797, 81012, 0, 100, 0, 1, 0, 1, 1, 'Icehowl - Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70200, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70201, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70202, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70203, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70204, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70205, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70206, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70207, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70208, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70209, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70210, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70211, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70212, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70213, 81012, 0, 3, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70214, 81012, 0, 25, 0, 1, 1, 1, 1, 'Rainbow Leather');
INSERT INTO `skinning_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (70215, 81012, 0, 3, 0, 1, 1, 1, 2, 'Rainbow Leather');

-- Increase rate of disenchant for Rainbow Dust
DELETE FROM `disenchant_loot_template` WHERE `Item` = 81009;
UPDATE disenchant_loot_template SET `chance` = 80, `GroupId` = 1 WHERE `entry` = 68;
UPDATE disenchant_loot_template SET `chance` = 80, `GroupId` = 1 WHERE `entry` = 69;
UPDATE disenchant_loot_template SET `chance` = 94.5 WHERE `entry` = 54 AND `Item` = 34052;
INSERT INTO `disenchant_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (54, 81009, 0, 5, 0, 1, 1, 1, 1, 'Rainbow Dust');
INSERT INTO `disenchant_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (68, 81009, 0, 20, 0, 1, 1, 1, 1, 'Rainbow Dust');
INSERT INTO `disenchant_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (69, 81009, 0, 20, 0, 1, 1, 1, 2, 'Rainbow Dust');