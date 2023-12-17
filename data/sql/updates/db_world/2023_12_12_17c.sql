-- Fix times on Stranglethorn Fishing Extravaganza
UPDATE game_event SET start_time = '2016-12-23 01:00:00' WHERE evententry = 14;
UPDATE game_event SET start_time = '2023-12-24 13:00:00' WHERE eventEntry = 62;
UPDATE game_event SET start_time = '2023-12-24 14:00:00' WHERE eventEntry = 15;
UPDATE game_event SET start_time = '2023-12-24 14:00:00' WHERE eventEntry = 90;
-- Fix times on Kalu'ak Fishing Derby
UPDATE game_event SET start_time = '2023-12-23 14:00:00' WHERE eventEntry = 63;
UPDATE game_event SET start_time = '2023-12-23 14:00:00' WHERE eventEntry = 64;