--
DELETE FROM `command` WHERE `name` = 'character changeraceall';
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('character changeraceall', 3, 'Syntax: .character changeraceall\r\n\r\nGive every character on the server a race change, taken from the character screen.');
