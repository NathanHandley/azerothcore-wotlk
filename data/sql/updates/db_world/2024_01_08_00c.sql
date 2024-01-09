-- Cleanup previous mastery stuff
DELETE FROM mail_level_reward WHERE LEVEL >= 80;
DELETE FROM mail_loot_template WHERE Entry IN (500, 501, 502, 503);
DELETE FROM item_template WHERE entry >= 75000 AND entry <= 79974;
DELETE FROM conditions WHERE SourceTypeOrReferenceId = 15 AND SourceGroup between 40000 AND 40035;
DELETE FROM conditions WHERE SourceTypeOrReferenceId = 15 AND SourceGroup between 40100 AND 40103;
DELETE FROM npc_text WHERE ID IN (45000, 45001, 45002, 45003);
DELETE FROM `creature_template` WHERE `entry` between 400001 AND 400036;
DELETE FROM `creature_template` WHERE `entry` between 400101 AND 400104;
DELETE FROM `creature` WHERE `guid` between 500000 AND 500035;
DELETE FROM `creature` WHERE `guid` between 500100 AND 500103;
DELETE FROM `npc_vendor` WHERE `entry` between 400000 AND 400036;
DELETE FROM `npc_vendor` WHERE `entry` between 400101 AND 400104;
DELETE FROM `gossip_menu` WHERE `MenuID` between 40000 AND 40035;
DELETE FROM `gossip_menu` WHERE `MenuID` between 40100 AND 40103;
DELETE FROM `gossip_menu_option` WHERE `MenuID` between 40000 AND 40035;
DELETE FROM `gossip_menu_option` WHERE `MenuID` between 40100 AND 40103;
UPDATE item_template SET description = 'Useless relic from a failed civilization.' WHERE entry = 81200;