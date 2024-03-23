-- Remove unsealed chest conditions on LK25 Heroic
DELETE FROM conditions WHERE sourceEntry = 51315 AND SourceGroup = 39168;