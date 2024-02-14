-- Change stack max for rainbow bag components
UPDATE item_template SET stackable = 100 WHERE entry >= 81001 AND entry <= 81012;