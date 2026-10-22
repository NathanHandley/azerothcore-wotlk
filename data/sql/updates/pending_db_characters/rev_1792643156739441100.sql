-- MergedWorlds extended-effects: a spell can carry up to 12 effects (SpellExtendedEffects.dbc), so saved auras need wider effect masks
-- and the amounts of effects 3 to 11.  Guarded so running it again changes nothing
SET @dbname = DATABASE();

SET @characterAuraAmount3Exists = (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = @dbname AND TABLE_NAME = 'character_aura' AND COLUMN_NAME = 'amount3');
SET @characterAuraAlter = IF(@characterAuraAmount3Exists = 0,
    'ALTER TABLE `character_aura` MODIFY COLUMN `effectMask` SMALLINT UNSIGNED NOT NULL DEFAULT ''0'', MODIFY COLUMN `recalculateMask` SMALLINT UNSIGNED NOT NULL DEFAULT ''0'', ADD COLUMN `amount3` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount4` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount5` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount6` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount7` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount8` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount9` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount10` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount11` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount3` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount4` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount5` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount6` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount7` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount8` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount9` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount10` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount11` INT NOT NULL DEFAULT ''0''',
    'SELECT 1');
PREPARE characterAuraStatement FROM @characterAuraAlter;
EXECUTE characterAuraStatement;
DEALLOCATE PREPARE characterAuraStatement;

SET @petAuraAmount3Exists = (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = @dbname AND TABLE_NAME = 'pet_aura' AND COLUMN_NAME = 'amount3');
SET @petAuraAlter = IF(@petAuraAmount3Exists = 0,
    'ALTER TABLE `pet_aura` MODIFY COLUMN `effectMask` SMALLINT UNSIGNED NOT NULL DEFAULT ''0'', MODIFY COLUMN `recalculateMask` SMALLINT UNSIGNED NOT NULL DEFAULT ''0'', ADD COLUMN `amount3` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount4` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount5` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount6` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount7` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount8` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount9` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount10` INT NOT NULL DEFAULT ''0'', ADD COLUMN `amount11` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount3` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount4` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount5` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount6` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount7` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount8` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount9` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount10` INT NOT NULL DEFAULT ''0'', ADD COLUMN `base_amount11` INT NOT NULL DEFAULT ''0''',
    'SELECT 1');
PREPARE petAuraStatement FROM @petAuraAlter;
EXECUTE petAuraStatement;
DEALLOCATE PREPARE petAuraStatement;
