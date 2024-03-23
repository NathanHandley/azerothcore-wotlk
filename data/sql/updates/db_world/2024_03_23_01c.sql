-- Remove unsealed chest conditions on LK25 Heroic
DELETE FROM conditions WHERE sourceEntry = 51315 AND SourceGroup = 39168;
-- Remove Panic Peroidoc from Coilfang Striders
DELETE FROM creature_template_addon WHERE entry = 22056;