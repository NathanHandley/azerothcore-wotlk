-- Give trade goods at least a 1 copper sell price so they can be sold to vendors
UPDATE item_template SET SellPrice = 1 WHERE class = 7 AND SellPrice = 0;