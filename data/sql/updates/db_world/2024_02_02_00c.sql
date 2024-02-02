-- Very slightly increase 15 pound mud snapper drop rate to reflect datamined data
UPDATE reference_loot_template SET `Chance` = 0.08 WHERE Item = 6295;
