-- Normalize the movement speed of the Valiants
UPDATE `creature_template` SET `speed_run` = 1.38571, `speed_walk` = 1 WHERE `entry` in (33564, 33562, 33561, 33559, 33558, 33384, 33383, 33382, 33306, 33285);

-- Normalize the movement speed of the Champions
UPDATE `creature_template` SET `speed_run` = 1.38571, `speed_walk` = 1 WHERE `entry` in (33738, 33739, 33740, 33743, 33744, 33745, 33746, 33747, 33748, 33749);

-- Normalize the movement speed of the Boneguard Commander and Lieutenant
UPDATE `creature_template` SET `speed_run` = 1.38571, `speed_walk` = 1 WHERE `entry` in (33429, 34127);