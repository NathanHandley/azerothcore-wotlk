-- Give Abyssal Shatter a chance to give rainbow dust
DELETE FROM spell_loot_template WHERE Item = 81009;
INSERT INTO `spell_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`) VALUES (69412, 81009, 0, 10, 0, 1, 0, 1, 1, 'Abyssal Shatter - Rainbow Dust');
