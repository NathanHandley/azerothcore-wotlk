-- Legion Hold (Shadowmoon Valley): the Invis Legion Hold Casters' waypoint script casts 36804 on the nearest Legion Hold Fel Reaver within 30
-- yards, but two casters (74878, 74879) stand 31 and 35 yards from theirs (74882) on the ground, so the cell search never reaches it and the
-- server logs "no target unit found for spell 36804" every 3 seconds.  40 yards reaches it; each caster's nearest Fel Reaver stays the same
UPDATE `waypoint_scripts` SET `x` = 40 WHERE `id` = 1136 AND `command` = 15 AND `datalong` = 36804;
