-- Update drop rate of rainbow leaves
DELETE FROM `gameobject_loot_template` WHERE `Item` = 81010;

INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (25094, 81010, 0, 4, 0, 1, 1, 1, 4, 'Frozen Herb - Rainbow Leaves');
UPDATE `gameobject_loot_template` SET `Chance` = 32 WHERE `Entry` = 25094 AND `Item` = 36901;
UPDATE `gameobject_loot_template` SET `Chance` = 32 WHERE `Entry` = 25094 AND `Item` = 36904;
UPDATE `gameobject_loot_template` SET `Chance` = 32 WHERE `Entry` = 25094 AND `Item` = 36907;

INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (25095, 81010, 0, 4, 0, 1, 1, 1, 4, 'Frozen Herb - Rainbow Leaves');
UPDATE `gameobject_loot_template` SET `Chance` = 32 WHERE `Entry` = 25095 AND `Item` = 36901;
UPDATE `gameobject_loot_template` SET `Chance` = 32 WHERE `Entry` = 25095 AND `Item` = 36904;
UPDATE `gameobject_loot_template` SET `Chance` = 32 WHERE `Entry` = 25095 AND `Item` = 36907;

INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (25096, 81010, 0, 4, 0, 1, 1, 2, 4, 'Frozen Herb - Rainbow Leaves');
UPDATE `gameobject_loot_template` SET `Chance` = 56 WHERE `Entry` = 25096 AND `Item` = 36901;

INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (24226, 81010, 0, 8, 0, 1, 0, 1, 1, 'Lichbloom - Rainbow Leaves');
INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (24227, 81010, 0, 8, 0, 1, 0, 1, 1, 'Icethorn - Rainbow Leaves');
INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (24228, 81010, 0, 20, 0, 1, 0, 1, 2, 'Frost Lotus - Rainbow Leaves');