-- Make most items non-unique
UPDATE item_template SET maxcount = 0 WHERE maxcount = 1 AND bonding < 4 AND (BagFamily & 256 = FALSE) AND startquest = 0 AND (Flags & 2 = FALSE) AND class != 12;
