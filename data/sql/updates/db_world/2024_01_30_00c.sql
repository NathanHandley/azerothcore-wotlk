-- Update drop rate of rainbow leaves
DELETE FROM `gameobject_loot_template` WHERE `Item` = 81010 AND `Entry` = 24228;
INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (24228, 81010, 0, 25, 0, 1, 0, 1, 1, 'Frost Lotus - Rainbow Leaves');