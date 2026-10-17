-- Help text of the .rotation spec command added by this project.
-- INSERT IGNORE only adds the row when it is missing and never changes an
-- existing one.
INSERT IGNORE INTO `command` (`name`, `security`, `help`) VALUES
('rotation spec', 0, 'Syntax: .rotation spec [auto|1|2|3|spec name]\r\n\r\nChoose the specialization the rotation spell uses, for example .rotation spec balance. auto uses the talent tree with the most points. Without an argument, shows the current choice. The choice is saved with the character.');
