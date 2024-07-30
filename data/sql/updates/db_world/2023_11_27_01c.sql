-- Make ammo and soul bags usable by ALL
UPDATE item_template SET AllowableClass = -1 WHERE InventoryType = 18 AND AllowableClass != -1; 