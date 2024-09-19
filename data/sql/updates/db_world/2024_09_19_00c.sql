-- Increase rainbow item stack sizes to 100
UPDATE item_template SET stackable = 100 WHERE entry >= 81001 AND entry <= 81012;