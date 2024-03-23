-- Remove unsealed chest conditions on LK25 Heroic
DELETE FROM conditions WHERE sourceEntry = 51315 AND SourceGroup = 39168;
-- Remove Panic Peroidoc from Coilfang Striders
DELETE FROM creature_template_addon WHERE entry = 22056;
-- Disable root on Tainted Core
UPDATE item_template SET spellid_3 = 0, spelltrigger_3 = 0 WHERE entry = 31088;