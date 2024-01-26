-- Increase Required XP from 80-83
DELETE FROM player_xp_for_level WHERE LEVEL > 79;
INSERT INTO player_xp_for_level (Level, Experience) VALUES (80, 16708000);
INSERT INTO player_xp_for_level (Level, Experience) VALUES (81, 33416000);
INSERT INTO player_xp_for_level (Level, Experience) VALUES (82, 66832000);